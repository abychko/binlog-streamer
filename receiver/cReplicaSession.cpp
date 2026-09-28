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

#include "receiver/cReplicaSession.hpp"

#include "protocol/cAuthMoreDataCodec.hpp"
#include "protocol/cAuthSwitchRequestCodec.hpp"
#include "protocol/cCachingSha2FullAuthPassword.hpp"
#include "protocol/cCachingSha2Scramble.hpp"
#include "protocol/cColumnDefinition41Codec.hpp"
#include "protocol/cComBinlogDumpGtidCommand.hpp"
#include "protocol/cComQueryCommand.hpp"
#include "protocol/cComRegisterSlaveCommand.hpp"
#include "protocol/cErrPacketCodec.hpp"
#include "protocol/cHandshakeResponse41Codec.hpp"
#include "protocol/cHandshakeV10Codec.hpp"
#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cOkPacketCodec.hpp"
#include "protocol/cPublicKeyRequestCodec.hpp"
#include "protocol/cRsaPublicKey.hpp"
#include "protocol/cTextRowCodec.hpp"
#include "protocol/hBinlogDumpFlags.hpp"
#include "protocol/hProtocolLimits.hpp"
#include "receiver/hSessionDefaults.hpp"

#include <sys/utsname.h>
#include <unistd.h>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace binlog_streamer {
namespace {

struct SingleColumnOutcome {
  enum class Kind { Row, ServerError, Failed } kind = Kind::Failed;
  std::optional<std::string>
      value;  // Kind::Row; nullopt covers both SQL NULL and an empty result set
  ErrPacket err;               // Kind::ServerError
  std::string failureMessage;  // Kind::Failed: a transport/framing failure,
                               // always retryable
};

SingleColumnOutcome QuerySingleColumn(PacketChannel &channel,
                                      std::string_view sql) {
  SingleColumnOutcome outcome;
  std::string error;
  channel.ResetSequence();
  if (!channel.WritePacket(ComQueryCommand::Encode(sql), error)) {
    outcome.failureMessage = "sending '" + std::string(sql) + "': " + error;
    return outcome;
  }
  std::vector<std::uint8_t> payload;
  if (!channel.ReadPacket(payload, error)) {
    outcome.failureMessage =
        "reading response to '" + std::string(sql) + "': " + error;
    return outcome;
  }
  if (ErrPacketCodec::IsErrPacket(payload)) {
    if (!ErrPacketCodec::Parse(payload, outcome.err, error)) {
      outcome.failureMessage =
          "malformed ERR for '" + std::string(sql) + "': " + error;
      return outcome;
    }
    outcome.kind = SingleColumnOutcome::Kind::ServerError;
    return outcome;
  }
  std::uint64_t columnCount = 0;
  bool isNull = false;
  if (LengthEncodedInteger::Decode(payload, columnCount, isNull) == 0 ||
      isNull || columnCount != 1) {
    outcome.failureMessage =
        "unexpected column count in response to '" + std::string(sql) + "'";
    return outcome;
  }
  ColumnDefinition41 column;
  if (!channel.ReadPacket(payload, error) ||
      !ColumnDefinition41Codec::Parse(payload, column, error)) {
    outcome.failureMessage =
        "reading column definition for '" + std::string(sql) + "': " + error;
    return outcome;
  }
  if (!channel.ReadPacket(payload, error)) {
    outcome.failureMessage =
        "reading row for '" + std::string(sql) + "': " + error;
    return outcome;
  }
  // Checked directly, not via OkPacketCodec::IsOkPacket (0x00 header also
  // matches a row whose first column is empty). 0xFE is unambiguous here:
  // a real lenenc-0xFE value needs >= 9 bytes, more than these rows carry.
  if (!payload.empty() && payload[0] == 0xFE &&
      payload.size() < MAX_PAYLOAD_PER_PACKET) {
    outcome.kind = SingleColumnOutcome::Kind::Row;
    outcome.value = std::nullopt;
    return outcome;
  }
  TextRow row;
  if (!TextRowCodec::Parse(payload, 1, row, error)) {
    outcome.failureMessage =
        "malformed row for '" + std::string(sql) + "': " + error;
    return outcome;
  }
  // None of these queries can legitimately return more than one row; a
  // defensive drain keeps the connection in sync if a source misbehaves.
  std::vector<std::uint8_t> terminator;
  if (!channel.ReadPacket(terminator, error)) {
    outcome.failureMessage =
        "reading result terminator for '" + std::string(sql) + "': " + error;
    return outcome;
  }
  if (!(!terminator.empty() && terminator[0] == 0xFE &&
        terminator.size() < MAX_PAYLOAD_PER_PACKET)) {
    outcome.failureMessage =
        "'" + std::string(sql) + "' returned more than one row";
    return outcome;
  }
  outcome.kind = SingleColumnOutcome::Kind::Row;
  outcome.value = row.columns.empty() ? std::nullopt : row.columns[0];
  return outcome;
}

struct PlainCommandOutcome {
  enum class Kind { Ok, ServerError, Failed } kind = Kind::Failed;
  ErrPacket err;
  std::string failureMessage;
};

PlainCommandOutcome ExecutePlainCommand(PacketChannel &channel,
                                        std::string_view sql) {
  PlainCommandOutcome outcome;
  std::string error;
  channel.ResetSequence();
  if (!channel.WritePacket(ComQueryCommand::Encode(sql), error)) {
    outcome.failureMessage = "sending '" + std::string(sql) + "': " + error;
    return outcome;
  }
  std::vector<std::uint8_t> payload;
  if (!channel.ReadPacket(payload, error)) {
    outcome.failureMessage =
        "reading response to '" + std::string(sql) + "': " + error;
    return outcome;
  }
  if (ErrPacketCodec::IsErrPacket(payload)) {
    if (!ErrPacketCodec::Parse(payload, outcome.err, error)) {
      outcome.failureMessage =
          "malformed ERR for '" + std::string(sql) + "': " + error;
      return outcome;
    }
    outcome.kind = PlainCommandOutcome::Kind::ServerError;
    return outcome;
  }
  // false: a plain command's OK is always header 0x00, never the
  // DEPRECATE_EOF row-terminator form (0xFE, only inside a result set).
  if (!OkPacketCodec::IsOkPacket(payload.empty() ? std::uint8_t{0} : payload[0],
                                 false)) {
    outcome.failureMessage =
        "unexpected response to '" + std::string(sql) + "'";
    return outcome;
  }
  outcome.kind = PlainCommandOutcome::Kind::Ok;
  return outcome;
}

// The attribute keys match a real replica's own (session_connect_attrs uses
// the same names); the values are this relay's own identity, not borrowed.
std::vector<std::pair<std::string, std::string>> BuildConnectionAttributes(
    const std::string &relayName, const std::string &relayVersion) {
  struct utsname systemInfo{};
  uname(&systemInfo);
  return {
      {"_client_name", relayName},
      {"_client_version", relayVersion},
      {"_os", systemInfo.sysname},
      {"_platform", systemInfo.machine},
      {"_pid", std::to_string(getpid())},
      {"program_name", relayName},
      {"_client_role", "binary_log_listener"},
      {"_client_replication_channel_name", ""},
  };
}

}  // namespace

ReplicaSession::ReplicaSession(Transport &transport,
                               const SourceSettings &source,
                               const ServerSettings &server,
                               std::string replicaUuid, std::string relayName,
                               std::string relayVersion,
                               ReplicaSessionOptions options)
    : m_tls(transport),
      m_compressed(m_tls, REPLICA_NET_TIMEOUT),
      m_source(source),
      m_server(server),
      m_replicaUuid(std::move(replicaUuid)),
      m_relayName(std::move(relayName)),
      m_relayVersion(std::move(relayVersion)),
      m_options(options),
      m_channel(m_compressed, SOURCE_CHANNEL_OPTIONS) {}

SessionResult ReplicaSession::Transient(std::string message) const {
  SessionResult result;
  // A Read() interrupted by a caller-requested stop (m_channel's sticky
  // flag) is Stopped, not TransientFailure - the one distinction among
  // Transient() call sites that must not be retried by StartupSequence.
  result.outcome = m_channel.WasInterrupted()
                       ? SessionOutcome::Stopped
                       : SessionOutcome::TransientFailure;
  result.message = std::move(message);
  return result;
}

SessionResult ReplicaSession::Permanent(std::string message) const {
  SessionResult result;
  result.outcome = SessionOutcome::PermanentFailure;
  result.identity = m_identity;
  result.message = std::move(message);
  return result;
}

SessionResult ReplicaSession::Run() {
  if (auto result = Connect()) return *result;
  if (auto result = Handshake()) return *result;
  if (auto result = Authenticate()) return *result;
  if (auto result = CheckVersion()) return *result;
  if (auto result = ReportClockSkew()) return *result;
  if (auto result = CheckServerId()) return *result;
  if (auto result = SetHeartbeatPeriod()) return *result;
  if (auto result = NegotiateChecksum()) return *result;
  if (auto result = CheckGtidMode()) return *result;
  if (auto result = CheckServerUuid()) return *result;
  if (auto result = SetReplicaUuid()) return *result;
  return m_options.registerAsReplica ? Register() : ReadyWithoutRegistering();
}

SessionResult ReplicaSession::ReadyWithoutRegistering() {
  SessionResult result;
  // identity.registered, not this outcome, says whether COM_REGISTER_SLAVE
  // itself ran.
  result.outcome = SessionOutcome::Registered;
  result.identity = m_identity;
  result.message =
      "pre-dump exchange complete (not registered - options.registerAsReplica "
      "is false)";
  return result;
}

std::optional<SessionResult> ReplicaSession::Connect() {
  std::string error;
  if (!m_compressed.Connect(m_source.host, m_source.port, REPLICA_NET_TIMEOUT,
                            error))
    return Transient("connecting to " + m_source.host + ":" +
                     std::to_string(m_source.port) + ": " + error);
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::Handshake() {
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!m_channel.ReadPacket(payload, error))
    return Transient("reading greeting: " + error);

  // Report the source's own message rather than the generic parse failure
  // HandshakeV10Codec::Parse would give.
  if (ErrPacketCodec::IsErrPacket(payload)) {
    ErrPacket err;
    if (!ErrPacketCodec::Parse(payload, err, error))
      return Transient("malformed ERR in place of greeting: " + error);
    return Transient("source rejected the connection: " + err.message);
  }

  HandshakeV10 greeting;
  if (!HandshakeV10Codec::Parse(payload, greeting, error))
    return Transient("malformed greeting: " + error);
  m_identity.versionString = greeting.serverVersion;
  m_authPluginName = greeting.authPluginName;
  m_authPluginData = greeting.authPluginData;

  // A strict request, not a preference: a source that does not offer the
  // algorithm named in source.yml ends the session instead of quietly
  // streaming uncompressed. The reference client refuses the same
  // way, before sending anything, with error 2066.
  const std::uint32_t compressionBit =
      CompressionCapabilityBit(m_source.compression);
  if (compressionBit != 0 && (greeting.capabilities & compressionBit) == 0)
    return Permanent(
        "source does not offer " +
        std::string(CompressionAlgorithmName(m_source.compression)) +
        " compression; set compression: uncompressed in source.yml to "
        "connect without it");

  // Strict the same way, except under PREFERRED, which is what the name
  // says. The bit is asked for only when it was offered: a
  // server that did not offer it reads the SSL request as a bad handshake.
  const bool sourceOffersTls = (greeting.capabilities & CLIENT_SSL) != 0;
  const bool useTls = m_source.sslMode != SslMode::Disabled && sourceOffersTls;
  if (m_source.sslMode != SslMode::Disabled &&
      m_source.sslMode != SslMode::Preferred && !sourceOffersTls)
    return Permanent(
        "source does not offer TLS; set ssl_mode: DISABLED in source.yml to "
        "connect without it");

  HandshakeResponse41 response;
  response.capabilities =
      REPLICA_CLIENT_CAPABILITIES | compressionBit | (useTls ? CLIENT_SSL : 0);
  // Encoded only under CLIENT_ZSTD_COMPRESSION_ALGORITHM; zlib's bit
  // carries no level.
  response.zstdCompressionLevel =
      static_cast<std::uint8_t>(m_source.zstdCompressionLevel);
  response.maxPacketSize = REPLICA_MAX_PACKET_SIZE;
  response.characterSet = REPLICA_CHARACTER_SET;
  response.username = m_source.user;
  response.authPluginName = m_authPluginName;
  if (m_authPluginName == CACHING_SHA2_PASSWORD_PLUGIN_NAME) {
    if (m_authPluginData.size() < SCRAMBLE_LENGTH)
      return Transient("greeting scramble shorter than expected");
    const auto scramble = CachingSha2Scramble::Compute(
        m_source.password, std::span<const std::uint8_t, SCRAMBLE_LENGTH>(
                               m_authPluginData.data(), SCRAMBLE_LENGTH));
    response.authResponse.assign(scramble.begin(), scramble.end());
  }
  // else: left empty; Authenticate() handles the AuthSwitchRequest.
  response.connectionAttributes =
      BuildConnectionAttributes(m_relayName, m_relayVersion);

  if (useTls) {
    if (!m_tlsContext.Loaded() &&
        !m_tlsContext.LoadClient(m_source.sslMode, m_source.tls, error))
      return Permanent("TLS: " + error);
    // The SSL request goes in the clear, the handshake follows it on the
    // socket directly, and the full response continues the packet
    // sequence over TLS (sql-common/client.cc).
    std::vector<std::uint8_t> request;
    HandshakeResponse41Codec::EncodeSslRequest(response, request);
    if (!m_channel.WritePacket(request, error))
      return Transient("sending the SSL request: " + error);
    if (!m_tls.Handshake(m_tlsContext, m_source.host, REPLICA_NET_TIMEOUT,
                         error))
      return Transient(error);
  }

  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::Encode(response, encoded);
  if (!m_channel.WritePacket(encoded, error))
    return Transient("sending HandshakeResponse41: " + error);
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::Authenticate() {
  std::vector<std::uint8_t> payload;
  std::string error;
  if (!m_channel.ReadPacket(payload, error))
    return Transient("reading authentication response: " + error);

  // 5: a well-behaved exchange takes at most 3 passes; the cap turns a
  // stuck source into a clear error instead of an infinite loop.
  for (int round = 0; round < 5; ++round) {
    if (ErrPacketCodec::IsErrPacket(payload)) {
      ErrPacket err;
      if (!ErrPacketCodec::Parse(payload, err, error))
        return Transient("malformed ERR during authentication: " + error);
      // Transient, matching a real replica: an authentication ERR
      // rejoins the connect-retry loop instead of a one-shot give-up.
      return Transient("authentication rejected by source: " + err.message);
    }
    // false: 0xFE means AuthSwitchRequest here, never an OK-as-terminator
    // (that ambiguity only exists inside a result set - see
    // cOkPacketCodec.hpp).
    if (OkPacketCodec::IsOkPacket(
            payload.empty() ? std::uint8_t{0} : payload[0], false)) {
      // Here and not in Handshake(): the OK ending authentication is the
      // last uncompressed packet, and only reaching it means the source
      // accepted the capability (sql/sql_connect.cc).
      if (m_source.compression != CompressionAlgorithm::None)
        m_compressed.Enable(
            m_source.compression,
            CompressionLevelInEffect(m_source.compression,
                                     m_source.zstdCompressionLevel));
      return std::nullopt;
    }
    if (AuthSwitchRequestCodec::IsAuthSwitchRequest(payload)) {
      AuthSwitchRequest switchRequest;
      if (!AuthSwitchRequestCodec::Parse(payload, switchRequest, error))
        return Transient("malformed AuthSwitchRequest: " + error);
      if (switchRequest.pluginName != CACHING_SHA2_PASSWORD_PLUGIN_NAME)
        return Transient(
            "source requested unsupported authentication plugin: " +
            switchRequest.pluginName);
      if (switchRequest.pluginData.size() < SCRAMBLE_LENGTH)
        return Transient("AuthSwitchRequest nonce shorter than expected");
      // Keeps m_authPluginData in sync with the nonce in effect - a
      // later PerformFullAuthentication must encrypt against this
      // one, not the greeting's.
      m_authPluginData.assign(switchRequest.pluginData.begin(),
                              switchRequest.pluginData.end());
      const auto scramble = CachingSha2Scramble::Compute(
          m_source.password, std::span<const std::uint8_t, SCRAMBLE_LENGTH>(
                                 m_authPluginData.data(), SCRAMBLE_LENGTH));
      if (!m_channel.WritePacket(
              std::span<const std::uint8_t>(scramble.data(), scramble.size()),
              error))
        return Transient("sending authentication scramble: " + error);
    } else if (AuthMoreDataCodec::IsAuthMoreData(payload)) {
      AuthMoreDataSignal signal{};
      std::span<const std::uint8_t> data;
      if (!AuthMoreDataCodec::Parse(payload, signal, data, error))
        return Transient("malformed AuthMoreData: " + error);
      if (signal == AuthMoreDataSignal::PerformFullAuthentication) {
        // Only reachable with an empty password if a caller built
        // SourceSettings directly, bypassing this relay's own config loader.
        if (m_source.password.empty())
          return Transient(
              "source requires full caching_sha2_password authentication, but "
              "no password is "
              "configured for this account");

        RsaPublicKey publicKey;
        std::string keyError;
        if (!m_source.sourcePublicKeyPem.empty()) {
          // A local key always wins over asking the source,
          // matching the reference client's own order (rsa_init()
          // first, network request only a fallback).
          const std::span<const std::uint8_t> pem(
              reinterpret_cast<const std::uint8_t *>(
                  m_source.sourcePublicKeyPem.data()),
              m_source.sourcePublicKeyPem.size());
          if (!RsaPublicKey::Parse(pem, publicKey, keyError))
            return Transient("parsing the configured RSA public key: " +
                             keyError);
        } else if (m_source.getSourcePublicKey) {
          // Read inline rather than through another pass of this
          // loop's round counter (see that counter's comment above).
          if (!m_channel.WritePacket(PublicKeyRequestCodec::Encode(), error))
            return Transient("requesting the source's RSA public key: " +
                             error);
          std::vector<std::uint8_t> keyPayload;
          if (!m_channel.ReadPacket(keyPayload, error))
            return Transient("reading the source's RSA public key: " + error);
          AuthMoreDataSignal keySignal{};
          std::span<const std::uint8_t> keyPem;
          if (!AuthMoreDataCodec::Parse(keyPayload, keySignal, keyPem, error) ||
              keyPem.empty())
            return Transient(
                "source did not answer the RSA public key request with a key");
          if (!RsaPublicKey::Parse(keyPem, publicKey, keyError))
            return Transient("parsing the source's RSA public key: " +
                             keyError);
        } else {
          // Matches the reference client's own message text
          // exactly - administrators search for it verbatim.
          return Transient(
              "Authentication requires secure connection. Configure "
              "source_public_key_path or "
              "get_source_public_key in source.yml to allow full "
              "authentication without TLS.");
        }

        if (m_authPluginData.size() < SCRAMBLE_LENGTH)
          return Transient("authentication nonce shorter than expected");
        std::vector<std::uint8_t> ciphertext;
        std::string encryptError;
        if (!CachingSha2FullAuthPassword::Encrypt(
                m_source.password,
                std::span<const std::uint8_t, SCRAMBLE_LENGTH>(
                    m_authPluginData.data(), SCRAMBLE_LENGTH),
                publicKey, ciphertext, encryptError))
          return Transient("encrypting the password for full authentication: " +
                           encryptError);
        if (!m_channel.WritePacket(ciphertext, error))
          return Transient("sending the encrypted password: " + error);
      }
    } else {
      return Transient("unexpected packet during authentication");
    }
    if (!m_channel.ReadPacket(payload, error))
      return Transient("reading authentication response: " + error);
  }
  return Transient(
      "authentication did not complete after repeated "
      "AuthMoreData/AuthSwitchRequest exchanges");
}

namespace {
// Just enough for the "8.4.11" version shape, not a general-purpose parser.
std::pair<unsigned, std::size_t> LeadingNumber(std::string_view text) {
  std::size_t digitCount = 0;
  while (digitCount < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[digitCount])))
    ++digitCount;
  if (digitCount == 0) return {0, 0};
  unsigned value = 0;
  try {
    value = static_cast<unsigned>(
        std::stoul(std::string(text.substr(0, digitCount))));
  } catch (const std::exception &) {
    return {0, 0};
  }
  return {value, digitCount};
}
}  // namespace

std::optional<SessionResult> ReplicaSession::CheckVersion() {
  const auto [major, majorDigits] = LeadingNumber(m_identity.versionString);
  if (majorDigits == 0)
    return Permanent("source version string is not numeric: '" +
                     m_identity.versionString + "'");
  if (major < MINIMUM_SUPPORTED_MAJOR_VERSION)
    return Permanent("source major version " + std::to_string(major) +
                     " is not supported (relay requires " +
                     std::to_string(MINIMUM_SUPPORTED_MAJOR_VERSION) + ".0+)");
  m_identity.versionMajor = major;

  // Matches mysql_get_server_version() (sql-common/client.cc): a version
  // string without ".<minor>.<patch>" leaves them at 0, not a failure.
  unsigned minor = 0;
  unsigned patch = 0;
  std::string_view rest = m_identity.versionString;
  rest.remove_prefix(majorDigits);
  if (!rest.empty() && rest.front() == '.') {
    rest.remove_prefix(1);
    const auto [minorValue, minorDigits] = LeadingNumber(rest);
    minor = minorValue;
    if (minorDigits > 0) {
      rest.remove_prefix(minorDigits);
      if (!rest.empty() && rest.front() == '.') {
        rest.remove_prefix(1);
        patch = LeadingNumber(rest).first;
      }
    }
  }
  m_identity.versionNumber = major * 10000 + minor * 100 + patch;
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::ReportClockSkew() {
  const auto outcome = QuerySingleColumn(m_channel, "SELECT UNIX_TIMESTAMP()");
  // Captured here, right after the round trip, to stay close to when the
  // source actually read its own clock.
  const auto readAt = std::chrono::steady_clock::now();
  if (outcome.kind == SingleColumnOutcome::Kind::Failed)
    return Transient(outcome.failureMessage);
  // The value is the source's own clock, used later as "now" from the
  // source's point of view, not this relay's.
  if (outcome.kind == SingleColumnOutcome::Kind::Row && outcome.value) {
    try {
      m_identity.unixTimestamp = std::stoull(*outcome.value);
      m_identity.unixTimestampReadAt = readAt;
    } catch (const std::exception &) {
      // Left at 0 - same "non-fatal, nothing further to do" handling
      // as a server error or missing value above.
    }
  }
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::CheckServerId() {
  const auto outcome =
      QuerySingleColumn(m_channel, "SELECT @@GLOBAL.SERVER_ID");
  if (outcome.kind == SingleColumnOutcome::Kind::Failed)
    return Transient(outcome.failureMessage);
  if (outcome.kind == SingleColumnOutcome::Kind::ServerError) {
    if (outcome.err.errorCode == ER_UNKNOWN_SYSTEM_VARIABLE)
      return std::nullopt;  // skip the equality check, as a real replica does
    return Permanent("SELECT @@GLOBAL.SERVER_ID failed: " +
                     outcome.err.message);
  }
  if (!outcome.value)
    return Permanent("SELECT @@GLOBAL.SERVER_ID returned no value");
  std::uint32_t sourceServerId = 0;
  try {
    sourceServerId = static_cast<std::uint32_t>(std::stoul(*outcome.value));
  } catch (const std::exception &) {
    return Permanent(
        "SELECT @@GLOBAL.SERVER_ID returned a non-numeric value: '" +
        *outcome.value + "'");
  }
  if (sourceServerId == m_server.serverId)
    return Permanent("source server_id (" + std::to_string(sourceServerId) +
                     ") equals this relay's own server_id");
  m_identity.serverId = sourceServerId;
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::SetHeartbeatPeriod() {
  const auto heartbeatNanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          m_options.heartbeatPeriod)
          .count();
  const std::string sql =
      "SET @master_heartbeat_period = " + std::to_string(heartbeatNanoseconds) +
      ", @source_heartbeat_period = " + std::to_string(heartbeatNanoseconds);
  const auto outcome = ExecutePlainCommand(m_channel, sql);
  if (outcome.kind == PlainCommandOutcome::Kind::Failed)
    return Transient(outcome.failureMessage);
  if (outcome.kind == PlainCommandOutcome::Kind::ServerError)
    return Permanent("setting heartbeat period failed: " + outcome.err.message);
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::NegotiateChecksum() {
  const auto setOutcome = ExecutePlainCommand(
      m_channel,
      "SET @master_binlog_checksum = @@global.binlog_checksum, "
      "@source_binlog_checksum = @@global.binlog_checksum");
  if (setOutcome.kind == PlainCommandOutcome::Kind::Failed)
    return Transient(setOutcome.failureMessage);
  if (setOutcome.kind == PlainCommandOutcome::Kind::ServerError) {
    if (setOutcome.err.errorCode == ER_UNKNOWN_SYSTEM_VARIABLE) {
      m_identity.checksumAlgorithm =
          "OFF";  // source predates binlog_checksum support
      return std::nullopt;
    }
    return Permanent("setting binlog checksum failed: " +
                     setOutcome.err.message);
  }
  const auto selectOutcome =
      QuerySingleColumn(m_channel, "SELECT @source_binlog_checksum");
  if (selectOutcome.kind == SingleColumnOutcome::Kind::Failed)
    return Transient(selectOutcome.failureMessage);
  if (selectOutcome.kind == SingleColumnOutcome::Kind::ServerError)
    return Permanent("SELECT @source_binlog_checksum failed: " +
                     selectOutcome.err.message);
  if (!selectOutcome.value)
    return Permanent("SELECT @source_binlog_checksum returned no value");
  m_identity.checksumAlgorithm = *selectOutcome.value;
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::CheckGtidMode() {
  const auto outcome =
      QuerySingleColumn(m_channel, "SELECT @@GLOBAL.GTID_MODE");
  if (outcome.kind == SingleColumnOutcome::Kind::Failed)
    return Transient(outcome.failureMessage);
  if (outcome.kind == SingleColumnOutcome::Kind::ServerError) {
    if (outcome.err.errorCode == ER_UNKNOWN_SYSTEM_VARIABLE)
      return Permanent(
          "source has no GTID_MODE (predates GTID support); relay requires "
          "GTID_MODE=ON");
    // The one exception among pre-dump SELECTs: other errors here are a
    // reconnect, not a fatal incompatibility.
    return Transient("SELECT @@GLOBAL.GTID_MODE failed: " +
                     outcome.err.message);
  }
  if (!outcome.value)
    return Transient("SELECT @@GLOBAL.GTID_MODE returned no value");
  if (*outcome.value != "ON")
    return Permanent("source GTID_MODE=" + *outcome.value +
                     "; relay requires GTID_MODE=ON");
  m_identity.gtidMode = *outcome.value;
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::CheckServerUuid() {
  const auto outcome =
      QuerySingleColumn(m_channel, "SELECT @@GLOBAL.SERVER_UUID");
  if (outcome.kind == SingleColumnOutcome::Kind::Failed)
    return Transient(outcome.failureMessage);
  if (outcome.kind == SingleColumnOutcome::Kind::ServerError) {
    if (outcome.err.errorCode == ER_UNKNOWN_SYSTEM_VARIABLE)
      return std::nullopt;  // source predates SERVER_UUID
    return Permanent("SELECT @@GLOBAL.SERVER_UUID failed: " +
                     outcome.err.message);
  }
  if (!outcome.value)
    return Permanent("SELECT @@GLOBAL.SERVER_UUID returned no value");
  if (*outcome.value == m_replicaUuid)
    return Permanent(
        "source server_uuid collides with this relay's session uuid");
  m_identity.serverUuid = *outcome.value;
  return std::nullopt;
}

std::optional<SessionResult> ReplicaSession::SetReplicaUuid() {
  // m_replicaUuid is SessionUuid::Generate()'s own output, never
  // external input, so interpolating it into SQL here is injection-safe.
  const std::string sql = "SET @slave_uuid = '" + m_replicaUuid +
                          "', @replica_uuid = '" + m_replicaUuid + "'";
  const auto outcome = ExecutePlainCommand(m_channel, sql);
  if (outcome.kind == PlainCommandOutcome::Kind::Failed)
    return Transient(outcome.failureMessage);
  if (outcome.kind == PlainCommandOutcome::Kind::ServerError)
    return Permanent("setting replica uuid failed: " + outcome.err.message);
  return std::nullopt;
}

SessionResult ReplicaSession::Register() {
  RegisterSlaveCommand command;
  command.serverId = m_server.serverId;
  std::string error;
  m_channel.ResetSequence();
  if (!m_channel.WritePacket(ComRegisterSlaveCommand::Encode(command), error))
    return Transient("sending COM_REGISTER_SLAVE: " + error);
  std::vector<std::uint8_t> payload;
  if (!m_channel.ReadPacket(payload, error))
    return Transient("reading COM_REGISTER_SLAVE response: " + error);
  if (ErrPacketCodec::IsErrPacket(payload)) {
    ErrPacket err;
    if (!ErrPacketCodec::Parse(payload, err, error))
      return Transient("malformed ERR for COM_REGISTER_SLAVE: " + error);
    // Transient, matching a real replica: an ERR here rejoins the
    // connect-retry loop rather than giving up outright.
    return Transient("registration rejected by source: " + err.message);
  }
  if (!OkPacketCodec::IsOkPacket(payload.empty() ? std::uint8_t{0} : payload[0],
                                 false))
    return Transient("unexpected response to COM_REGISTER_SLAVE");

  m_identity.registered = true;
  SessionResult result;
  result.outcome = SessionOutcome::Registered;
  result.identity = m_identity;
  result.message = "registered with source as server_id " +
                   std::to_string(m_server.serverId);
  if (m_tls.Enabled()) result.message += " over " + tlsDescription();
  return result;
}

ReplicaSession::TextQueryResult ReplicaSession::QueryText(
    std::string_view sql) {
  const auto outcome = QuerySingleColumn(m_channel, sql);
  TextQueryResult result;
  if (outcome.kind == SingleColumnOutcome::Kind::Failed) {
    result.failure = Transient(outcome.failureMessage);
    return result;
  }
  if (outcome.kind == SingleColumnOutcome::Kind::ServerError) {
    result.failure =
        Permanent("'" + std::string(sql) + "' failed: " + outcome.err.message);
    return result;
  }
  result.ok = true;
  result.value = outcome.value;
  return result;
}

ReplicaSession::RowQueryResult ReplicaSession::QueryRow(
    std::string_view sql, std::size_t columnCount) {
  RowQueryResult result;
  std::string error;
  m_channel.ResetSequence();
  if (!m_channel.WritePacket(ComQueryCommand::Encode(sql), error)) {
    result.failure = Transient("sending '" + std::string(sql) + "': " + error);
    return result;
  }
  std::vector<std::uint8_t> payload;
  if (!m_channel.ReadPacket(payload, error)) {
    result.failure =
        Transient("reading response to '" + std::string(sql) + "': " + error);
    return result;
  }
  if (ErrPacketCodec::IsErrPacket(payload)) {
    ErrPacket err;
    if (!ErrPacketCodec::Parse(payload, err, error)) {
      result.failure =
          Transient("malformed ERR for '" + std::string(sql) + "': " + error);
      return result;
    }
    result.failure =
        Permanent("'" + std::string(sql) + "' failed: " + err.message);
    return result;
  }
  std::uint64_t declaredColumnCount = 0;
  bool isNull = false;
  if (LengthEncodedInteger::Decode(payload, declaredColumnCount, isNull) == 0 ||
      isNull || declaredColumnCount != columnCount) {
    result.failure = Permanent("unexpected column count in response to '" +
                               std::string(sql) + "'");
    return result;
  }
  for (std::size_t i = 0; i < columnCount; ++i) {
    ColumnDefinition41 column;
    if (!m_channel.ReadPacket(payload, error) ||
        !ColumnDefinition41Codec::Parse(payload, column, error)) {
      result.failure = Transient("reading column definition for '" +
                                 std::string(sql) + "': " + error);
      return result;
    }
  }
  if (!m_channel.ReadPacket(payload, error)) {
    result.failure =
        Transient("reading row for '" + std::string(sql) + "': " + error);
    return result;
  }
  // Same DEPRECATE_EOF disambiguation as QuerySingleColumn() above.
  if (!payload.empty() && payload[0] == 0xFE &&
      payload.size() < MAX_PAYLOAD_PER_PACKET) {
    result.ok = true;
    return result;
  }
  TextRow row;
  if (!TextRowCodec::Parse(payload, columnCount, row, error)) {
    result.failure =
        Permanent("malformed row for '" + std::string(sql) + "': " + error);
    return result;
  }
  std::vector<std::uint8_t> terminator;
  if (!m_channel.ReadPacket(terminator, error)) {
    result.failure = Transient("reading result terminator for '" +
                               std::string(sql) + "': " + error);
    return result;
  }
  if (!(!terminator.empty() && terminator[0] == 0xFE &&
        terminator.size() < MAX_PAYLOAD_PER_PACKET)) {
    result.failure =
        Permanent("'" + std::string(sql) + "' returned more than one row");
    return result;
  }
  result.ok = true;
  result.columns = std::move(row.columns);
  return result;
}

std::optional<SessionResult> ReplicaSession::StartDump(const GtidSet &gtidSet) {
  // Below this, the source cannot understand a tagged GTID.
  constexpr std::uint32_t FIRST_VERSION_WITH_TAGGED_GTIDS = 80300;
  const bool skipTaggedGtids =
      m_identity.versionNumber < FIRST_VERSION_WITH_TAGGED_GTIDS;

  BinlogDumpGtidCommand command;
  command.serverId = m_server.serverId;
  // Matches a real replica (sql/rpl_replica.cc, request_dump()).
  command.flags = BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2;
  if (skipTaggedGtids) command.flags |= BINLOG_DUMP_SKIP_TAGGED_GTIDS;
  command.gtidSetEncoded = gtidSet.Encode(skipTaggedGtids);

  // Not read here: EventStreamReader::Run() distinguishes an ERR from the
  // first event's bytes for the rest of the stream too.
  m_channel.ResetSequence();
  std::string error;
  if (!m_channel.WritePacket(ComBinlogDumpGtidCommand::Encode(command), error))
    return Transient("sending COM_BINLOG_DUMP_GTID: " + error);
  return std::nullopt;
}

}  // namespace binlog_streamer
