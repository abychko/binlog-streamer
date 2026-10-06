/* Copyright (c) 2026, Alexey Bychko.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of binlog-streamer hereby grant you an
   additional permission to link the program and your derivative works
   with the separately licensed software that they have either included
   with the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "cReplicaConnection.hpp"
#include "status/cRelayStatusTracker.hpp"

#include "cDumpSender.hpp"
#include "cDumpSessionRegistry.hpp"
#include "cDumpStartResolver.hpp"
#include "cQueryResponder.hpp"

#include "config/cAddressRangeMatcher.hpp"
#include "config/cIpAddressText.hpp"
#include "hServerDefaults.hpp"
#include "protocol/cAuthMoreDataCodec.hpp"
#include "protocol/cAuthSwitchRequestCodec.hpp"
#include "protocol/cCachingSha2Scramble.hpp"
#include "protocol/cColumnDefinition41Codec.hpp"
#include "protocol/cComBinlogDumpGtidCommand.hpp"
#include "protocol/cComQueryCommand.hpp"
#include "protocol/cComRegisterSlaveCommand.hpp"
#include "protocol/cEofPacketCodec.hpp"
#include "protocol/cErrPacketCodec.hpp"
#include "protocol/cHandshakeResponse41Codec.hpp"
#include "protocol/cHandshakeV10Codec.hpp"
#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cOkPacketCodec.hpp"
#include "protocol/cTextRowCodec.hpp"
#include "protocol/eCommand.hpp"
#include "protocol/eCompressionAlgorithm.hpp"
#include "protocol/hBinlogDumpFlags.hpp"
#include "protocol/hCapabilityFlags.hpp"
#include "protocol/hProtocolLimits.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <utility>

namespace binlog_streamer {
namespace {

std::string ConnectionAttribute(const HandshakeResponse41 &response,
                                std::string_view key) {
  for (const auto &[name, value] : response.connectionAttributes)
    if (name == key) return value;
  return {};
}

constexpr PacketChannelOptions LOGIN_CHANNEL_OPTIONS{
    LOGIN_TIMEOUT, LOGIN_TIMEOUT, INCOMING_PACKET_LIMIT, "replica"};
constexpr PacketChannelOptions COMMAND_CHANNEL_OPTIONS{
    COMMAND_READ_TIMEOUT, COMMAND_WRITE_TIMEOUT, INCOMING_PACKET_LIMIT,
    "replica"};

// Every login runs the same scramble check whether or not the username matched
// a configured client, so a mismatch never reveals which path ran.
constexpr char UNKNOWN_ACCOUNT_PLACEHOLDER_PASSWORD[] = "";

std::uint32_t NextThreadId() {
  // Relaxed is enough: the id only has to be unique, for diagnostics.
  static std::atomic<std::uint32_t> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

// As generate_user_salt(): printable, NUL- and '$'-free bytes plus a trailing
// 0x00, so the wire bytes match a real server's.
std::array<std::uint8_t, SCRAMBLE_LENGTH + 1> GenerateNonce() {
  std::array<std::uint8_t, SCRAMBLE_LENGTH + 1> nonce{};
  RAND_bytes(nonce.data(), SCRAMBLE_LENGTH);
  for (std::size_t i = 0; i < SCRAMBLE_LENGTH; ++i) {
    nonce[i] &= 0x7F;
    if (nonce[i] == 0x00 || nonce[i] == '$')
      nonce[i] = static_cast<std::uint8_t>(nonce[i] + 1);
  }
  nonce[SCRAMBLE_LENGTH] = 0x00;
  return nonce;
}

}  // namespace

std::string ServerVersionString(const std::string &sourceVersion,
                                const std::string &relayName,
                                const std::string &relayVersion) {
  if (sourceVersion.empty()) return {};
  return sourceVersion + "-" + relayName + "-" + relayVersion;
}

bool IsRelayServerVersion(const std::string &serverVersion,
                          const std::string &relayName) {
  // The last occurrence: in a chain of relays the answering one's name comes
  // last.
  const std::string mark = "-" + relayName + "-";
  const std::size_t at = serverVersion.rfind(mark);
  return at != std::string::npos && at + mark.size() < serverVersion.size();
}

ReplicaConnection::ReplicaConnection(Transport &transport,
                                     IpAddress peerAddress,
                                     ReplicaClientList::Snapshot clients,
                                     std::string serverVersion, LogFunction log,
                                     ConnectionRefusal refusal,
                                     NonceGenerator nonceGenerator,
                                     ConnectionServices services)
    : m_transport(transport),
      m_tls(transport),
      m_compressed(m_tls, COMMAND_READ_TIMEOUT),
      m_peerAddress(peerAddress),
      m_peerAddressText(IpAddressText::Format(peerAddress)),
      m_clients(clients ? std::move(clients)
                        : std::make_shared<const std::vector<ReplicaClient>>()),
      m_serverVersion(std::move(serverVersion)),
      m_log(std::move(log)),
      m_refusal(refusal),
      m_nonceGenerator(nonceGenerator ? std::move(nonceGenerator)
                                      : NonceGenerator(&GenerateNonce)),
      m_services(std::move(services)) {}

void ReplicaConnection::Run() {
  if (m_refusal != ConnectionRefusal::None) {
    PacketChannel channel(m_compressed, LOGIN_CHANNEL_OPTIONS);
    if (m_refusal == ConnectionRefusal::TooManyConnections) {
      SendRefusal(channel, 1040, "Too many connections");
      Log("rejected", "", "too many connections");
    } else {
      SendRefusal(channel, 3168,
                  "Server isn't available: nothing has been received from "
                  "the source yet");
      Log("rejected", "", "nothing received from the source yet");
    }
    m_transport.Close();
    return;
  }

  std::string username;
  if (Login(username)) CommandLoop(username);
  m_transport.Close();
  if (m_registration) m_services.status->UnregisterReplica(m_registration);
}

bool ReplicaConnection::CheckHost() const {
  for (const auto &client : *m_clients)
    for (const auto &range : client.hosts)
      if (AddressRangeMatcher::Contains(range, m_peerAddress)) return true;
  return false;
}

const ReplicaClient *ReplicaConnection::FindClient(
    const std::string &username) const {
  for (const auto &client : *m_clients) {
    if (client.user != username) continue;
    for (const auto &range : client.hosts)
      if (AddressRangeMatcher::Contains(range, m_peerAddress)) return &client;
  }
  return nullptr;
}

bool ReplicaConnection::Login(std::string &username) {
  PacketChannel channel(m_compressed, LOGIN_CHANNEL_OPTIONS);

  if (!CheckHost()) {
    SendRefusal(channel, 1130,
                "Host '" + m_peerAddressText +
                    "' is not allowed to connect to this MySQL server");
    Log("rejected", "", "host not allowed");
    return false;
  }

  const auto nonce = m_nonceGenerator();
  HandshakeV10 greeting;
  greeting.protocolVersion = PROTOCOL_VERSION;
  greeting.serverVersion = m_serverVersion;
  greeting.threadId = NextThreadId();
  greeting.authPluginData.assign(nonce.begin(), nonce.end());
  // A replica whose client library was told to compress refuses the connection
  // itself, before sending anything, when no compression bit is offered
  // (sql-common/client.cc, error 2066).
  const std::uint32_t compressionBit =
      CompressionCapabilityBit(m_services.compression);
  const std::uint32_t advertisedCapabilities =
      SERVER_CAPABILITIES | compressionBit |
      (m_services.tls != nullptr ? CLIENT_SSL : 0);
  greeting.capabilities = advertisedCapabilities;
  greeting.characterSet = 0;
  greeting.statusFlags = 0;
  greeting.authPluginName = CACHING_SHA2_PASSWORD_PLUGIN_NAME;

  std::vector<std::uint8_t> encodedGreeting;
  std::string error;
  if (!HandshakeV10Codec::Encode(greeting, encodedGreeting, error)) {
    Log("closed", "", "encoding greeting: " + error);
    return false;
  }
  if (!channel.WritePacket(encodedGreeting, error)) {
    Log("closed", "", "sending greeting: " + error);
    return false;
  }

  std::vector<std::uint8_t> payload;
  if (!channel.ReadPacket(payload, error)) {
    Log("closed", "", "reading handshake response: " + error);
    return false;
  }
  if (HandshakeResponse41Codec::IsSslRequest(payload)) {
    if (m_services.tls == nullptr) {
      SendErr(channel, 1043, "08S01", "Bad handshake");
      Log("rejected", "", "bad handshake: TLS requested but not offered");
      return false;
    }
    // The client's hello follows its SSL request at once, so part of it may
    // already sit in the channel's read-ahead.
    const std::vector<std::uint8_t> unread = channel.TakeUnread();
    if (!m_tls.Accept(*m_services.tls, LOGIN_TIMEOUT, error, unread)) {
      Log("closed", "", error);
      return false;
    }
    if (!channel.ReadPacket(payload, error)) {
      Log("closed", "", "reading handshake response: " + error);
      return false;
    }
  }
  HandshakeResponse41 response;
  if (!HandshakeResponse41Codec::Parse(payload, response, error)) {
    SendErr(channel, 1043, "08S01", "Bad handshake");
    Log("rejected", "", "bad handshake: " + error);
    return false;
  }
  username = response.username;
  m_clientCapabilities = response.capabilities & advertisedCapabilities;
  if (m_services.requireSecureTransport && !m_tls.Enabled()) {
    SendErr(channel, 3159, "HY000",
            "Connections using insecure transport are prohibited while "
            "--require_secure_transport=ON.");
    Log("rejected", username, "insecure transport");
    return false;
  }

  const bool compress =
      compressionBit != 0 && (m_clientCapabilities & compressionBit) != 0;
  const bool statesLevel =
      compress && m_services.compression == CompressionAlgorithm::Zstd;
  if (statesLevel &&
      (response.zstdCompressionLevel < MIN_ZSTD_COMPRESSION_LEVEL ||
       response.zstdCompressionLevel > MAX_ZSTD_COMPRESSION_LEVEL)) {
    SendErr(channel, 3923, "HY000",
            "Invalid zstd compression level for algorithm 'zstd'.");
    Log("rejected", username,
        "zstd compression level " +
            std::to_string(response.zstdCompressionLevel) +
            " is outside 1..22");
    return false;
  }

  std::vector<std::uint8_t> authResponse = response.authResponse;
  std::array<std::uint8_t, SCRAMBLE_LENGTH> nonceInEffect{};
  std::copy(nonce.begin(), nonce.begin() + SCRAMBLE_LENGTH,
            nonceInEffect.begin());

  if (response.authPluginName != CACHING_SHA2_PASSWORD_PLUGIN_NAME) {
    const auto switchNonce = m_nonceGenerator();
    AuthSwitchRequest switchRequest;
    switchRequest.pluginName = CACHING_SHA2_PASSWORD_PLUGIN_NAME;
    switchRequest.pluginData.assign(switchNonce.begin(), switchNonce.end());
    std::vector<std::uint8_t> encodedSwitch;
    AuthSwitchRequestCodec::Encode(switchRequest, encodedSwitch);
    if (!channel.WritePacket(encodedSwitch, error)) {
      Log("closed", username, "sending AuthSwitchRequest: " + error);
      return false;
    }
    std::vector<std::uint8_t> switchResponse;
    if (!channel.ReadPacket(switchResponse, error)) {
      Log("closed", username,
          "reading scramble after AuthSwitchRequest: " + error);
      return false;
    }
    authResponse = std::move(switchResponse);
    std::copy(switchNonce.begin(), switchNonce.begin() + SCRAMBLE_LENGTH,
              nonceInEffect.begin());
  }

  const ReplicaClient *client = FindClient(username);
  const std::string &passwordForCompare =
      client != nullptr ? client->password
                        : UNKNOWN_ACCOUNT_PLACEHOLDER_PASSWORD;
  const auto expected = CachingSha2Scramble::Compute(
      passwordForCompare, std::span<const std::uint8_t, SCRAMBLE_LENGTH>(
                              nonceInEffect.data(), SCRAMBLE_LENGTH));
  // CRYPTO_memcmp, not ==/std::equal: a data-dependent early exit would let
  // response time leak how close a guessed scramble is. Sizes are compared
  // first since CRYPTO_memcmp requires equal-length buffers.
  const bool scrambleMatches =
      authResponse.size() == expected.size() &&
      CRYPTO_memcmp(authResponse.data(), expected.data(), expected.size()) == 0;

  if (client == nullptr || !scrambleMatches) {
    const bool usingPassword = !authResponse.empty();
    SendErr(channel, 1045, "28000",
            "Access denied for user '" + username + "'@'" + m_peerAddressText +
                "' (using password: " + (usingPassword ? "YES" : "NO") + ")");
    Log("rejected", username, "access denied");
    return false;
  }

  std::vector<std::uint8_t> fastAuthSuccess;
  AuthMoreDataCodec::EncodeSignal(AuthMoreDataSignal::FastAuthSuccess,
                                  fastAuthSuccess);
  if (!channel.WritePacket(fastAuthSuccess, error)) {
    Log("closed", username, "sending fast-auth-success: " + error);
    return false;
  }
  OkPacket ok;
  ok.statusFlags = SERVER_STATUS_AUTOCOMMIT;
  std::vector<std::uint8_t> encodedOk;
  OkPacketCodec::Encode(ok, false, encodedOk);
  if (!channel.WritePacket(encodedOk, error)) {
    Log("closed", username, "sending login OK: " + error);
    return false;
  }

  const int level = CompressionLevelInEffect(m_services.compression,
                                             response.zstdCompressionLevel);
  if (compress) m_compressed.Enable(m_services.compression, level);

  std::string detail;
  if (m_tls.Enabled()) detail = m_tls.Version() + " " + m_tls.Cipher();
  if (compress) {
    if (!detail.empty()) detail += ", ";
    detail += std::string(CompressionAlgorithmName(m_services.compression)) +
              " compression, level " + std::to_string(level);
  }
  Log("accepted", username, detail);
  if (m_services.status != nullptr) {
    m_registration = m_services.status->RegisterReplica(ReplicaFacts{
        m_peerAddressText, username, m_tls.Enabled(),
        compress ? std::string(CompressionAlgorithmName(m_services.compression))
                 : "none",
        ConnectionAttribute(response, "program_name"),
        ConnectionAttribute(response, "_client_version")});
  }
  return true;
}

void ReplicaConnection::CommandLoop(const std::string &username) {
  // A fresh channel: this phase's timeouts (COMMAND_CHANNEL_OPTIONS) differ
  // from login's, as mysqld resets its net timeouts between the two phases.
  PacketChannel channel(m_compressed, COMMAND_CHANNEL_OPTIONS);
  for (;;) {
    channel.ResetSequence();
    std::vector<std::uint8_t> payload;
    std::string error;
    if (!channel.ReadPacket(payload, error, COMMAND_WAIT_TIMEOUT)) {
      Log("closed", username, error.empty() ? "connection ended" : error);
      return;
    }
    const Command command = payload.empty() ? static_cast<Command>(0)
                                            : static_cast<Command>(payload[0]);
    if (command == Command::Quit) {
      Log("closed", username, "COM_QUIT");
      return;
    }
    if (command == Command::Ping) {
      OkPacket ok;
      ok.statusFlags = SERVER_STATUS_AUTOCOMMIT;
      std::vector<std::uint8_t> encoded;
      OkPacketCodec::Encode(ok, false, encoded);
      if (!channel.WritePacket(encoded, error)) {
        Log("closed", username, "sending PING response: " + error);
        return;
      }
      continue;
    }
    if (command == Command::Query && m_services.queryResponder != nullptr) {
      std::string_view sql;
      if (!ComQueryCommand::Parse(payload, sql, error)) {
        SendErr(channel, 1835, "HY000", "Malformed communication packet.");
        continue;
      }
      if (!SendQueryResponse(channel, m_services.queryResponder->Respond(
                                          sql, m_sessionVariables))) {
        Log("closed", username, "sending a query response failed");
        return;
      }
      continue;
    }
    if (command == Command::RegisterSlave) {
      RegisterSlaveCommand registration;
      if (!ComRegisterSlaveCommand::Parse(payload, registration, error)) {
        SendErr(channel, 1835, "HY000", "Malformed communication packet.");
        continue;
      }
      m_registeredServerId = registration.serverId;
      if (m_registration)
        m_registration->SetReportHost(registration.reportHost);
      OkPacket ok;
      ok.statusFlags = SERVER_STATUS_AUTOCOMMIT;
      std::vector<std::uint8_t> encoded;
      OkPacketCodec::Encode(ok, false, encoded);
      if (!channel.WritePacket(encoded, error)) {
        Log("closed", username, "sending the registration response: " + error);
        return;
      }
      continue;
    }
    if (command == Command::BinlogDumpGtid &&
        m_services.storageReader != nullptr) {
      if (!HandleDump(channel, payload, username)) return;
      continue;
    }
    if (command == Command::BinlogDump) {
      // A dump by file name and position (a replica without
      // SOURCE_AUTO_POSITION, mysqlbinlog --read-from-remote-server): refused
      // as a source refuses a dump, so the reason reaches the replica's log.
      SendErr(channel, 1236, "HY000",
              "relay serves GTID auto-positioning only; a dump by file "
              "name and position is not offered");
      Log("refused", username,
          "dump by file name and position: GTID auto-positioning only");
      continue;
    }
    SendErr(channel, 1047, "08S01", "Unknown command");
  }
}

bool ReplicaConnection::HandleDump(PacketChannel &channel,
                                   const std::vector<std::uint8_t> &payload,
                                   const std::string &username) {
  std::string error;
  BinlogDumpGtidCommand command;
  if (!ComBinlogDumpGtidCommand::Parse(payload, command, error)) {
    SendErr(channel, 1835, "HY000", "Malformed communication packet.");
    return true;
  }
  DumpStart start = DumpStartResolver::Resolve(command, m_services.serverUuid,
                                               *m_services.storageReader);
  switch (start.kind) {
    case DumpStartKind::Found:
      break;
    case DumpStartKind::NoHistoryYet:
      // Closing looks like a lost connection, which the replica retries; an
      // error packet would stop its receiver thread for good.
      Log("closed", username, "dump requested, but " + start.message);
      return false;
    default:
      // ER_SOURCE_FATAL_ERROR_READING_BINLOG: how a source reports every
      // refusal of a dump.
      SendErr(channel, 1236, "HY000", start.message);
      Log("refused", username, "dump: " + start.message);
      return true;
  }

  std::string sessionKey = m_sessionVariables.Get("replica_uuid").value_or("");
  if (sessionKey.empty())
    sessionKey = m_sessionVariables.Get("slave_uuid").value_or("");
  if (sessionKey.empty()) {
    const std::uint32_t serverId =
        command.serverId != 0 ? command.serverId : m_registeredServerId;
    sessionKey = "server_id:" + std::to_string(serverId);
  }
  DumpSessionRegistry::SupersededFlag superseded;
  if (m_services.dumpSessions != nullptr)
    superseded = m_services.dumpSessions->Claim(sessionKey);

  Log("dump", username,
      "starts at " + start.cursor->FileName() + ", replica has " +
          start.replicaSet.ToText());

  DumpSenderOptions options;
  options.serverId = m_services.serverId;
  std::string checksum =
      m_sessionVariables.Get("source_binlog_checksum").value_or("");
  if (checksum.empty())
    checksum = m_sessionVariables.Get("master_binlog_checksum").value_or("");
  options.checksum = checksum == "CRC32";
  options.nonBlocking =
      (command.flags & BINLOG_DUMP_NON_BLOCK) != 0 || command.serverId == 0;
  std::string heartbeat =
      m_sessionVariables.Get("source_heartbeat_period").value_or("");
  if (heartbeat.empty())
    heartbeat = m_sessionVariables.Get("master_heartbeat_period").value_or("");
  if (!heartbeat.empty() &&
      heartbeat.find_first_not_of("0123456789") == std::string::npos &&
      heartbeat.size() < 19) {
    options.heartbeatPeriod = std::chrono::nanoseconds(std::stoll(heartbeat));
  }
  options.heartbeatV2 =
      (command.flags & BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2) != 0;
  options.stopRequested = m_services.stopRequested;
  options.superseded = superseded;
  options.sendLinger = m_services.sendLinger;
  if (m_registration) {
    options.progress = &m_registration->Progress();
    m_registration->SetDumping(true);
  }
  DumpSender sender(*m_services.storageReader, channel, options);
  const DumpEnd end = sender.Run(std::move(start.cursor), start.replicaSet);
  if (m_registration) m_registration->SetDumping(false);

  if (m_services.dumpSessions != nullptr)
    m_services.dumpSessions->Release(sessionKey, superseded);
  Log("closed", username,
      "dump ended after " + std::to_string(end.events) + " events sent and " +
          std::to_string(end.skipped) + " skipped: " + end.message);
  return false;
}

bool ReplicaConnection::SendQueryResponse(PacketChannel &channel,
                                          const QueryResponse &response) {
  std::string error;
  std::vector<std::uint8_t> packet;
  if (response.kind == QueryResponseKind::Error) {
    SendErr(channel, response.errorCode, response.sqlState, response.message);
    return true;
  }
  OkPacket ok;
  ok.statusFlags = SERVER_STATUS_AUTOCOMMIT;
  if (response.kind == QueryResponseKind::Ok) {
    OkPacketCodec::Encode(ok, false, packet);
    return channel.WritePacket(packet, error);
  }

  // With CLIENT_DEPRECATE_EOF the end is an OK (header 0xFE) and no EOF follows
  // the column definitions.
  const bool deprecateEof = (m_clientCapabilities & CLIENT_DEPRECATE_EOF) != 0;
  LengthEncodedInteger::Encode(response.columns.size(), packet);
  if (!channel.WritePacket(packet, error)) return false;

  for (const QueryColumn &responseColumn : response.columns) {
    ColumnDefinition41 column;
    column.catalog = "def";
    column.name = responseColumn.name;
    column.characterSet = 255;
    column.columnLength = 1024;
    column.type = 253;
    packet.clear();
    ColumnDefinition41Codec::Encode(column, packet);
    if (!channel.WritePacket(packet, error)) return false;
  }

  EofPacket eof;
  eof.statusFlags = SERVER_STATUS_AUTOCOMMIT;
  if (!deprecateEof) {
    packet.clear();
    EofPacketCodec::Encode(eof, packet);
    if (!channel.WritePacket(packet, error)) return false;
  }

  TextRow row;
  for (const QueryColumn &responseColumn : response.columns)
    row.columns.push_back(responseColumn.value);
  packet.clear();
  TextRowCodec::Encode(row, packet);
  if (!channel.WritePacket(packet, error)) return false;

  packet.clear();
  if (deprecateEof) {
    OkPacketCodec::Encode(ok, false, packet);
    packet[0] = 0xFE;
  } else {
    EofPacketCodec::Encode(eof, packet);
  }
  return channel.WritePacket(packet, error);
}

void ReplicaConnection::SendErr(PacketChannel &channel, std::uint16_t code,
                                std::string_view sqlState,
                                std::string message) {
  ErrPacket err;
  err.errorCode = code;
  err.sqlState = std::string(sqlState);
  err.message = std::move(message);
  std::vector<std::uint8_t> encoded;
  ErrPacketCodec::Encode(err, encoded);
  std::string error;
  channel.WritePacket(encoded, error);
}

void ReplicaConnection::SendRefusal(PacketChannel &channel, std::uint16_t code,
                                    std::string message) {
  SendErr(channel, code, "", std::move(message));
}

void ReplicaConnection::Log(std::string_view event, const std::string &username,
                            std::string_view detail) {
  if (!m_log) return;
  std::string line(event);
  line += ' ';
  line += m_peerAddressText;
  if (!username.empty()) {
    line += " user=";
    line += username;
  }
  if (!detail.empty()) {
    line += ": ";
    line += detail;
  }
  m_log(line);
}

}  // namespace binlog_streamer
