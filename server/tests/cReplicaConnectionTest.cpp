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

#include "cFakeTransport.hpp"
#include "net/cCompressedTransport.hpp"
#include "net/cTlsCertificateGenerator.hpp"
#include "net/cTlsContext.hpp"
#include "protocol/cAuthMoreDataCodec.hpp"
#include "protocol/cAuthSwitchRequestCodec.hpp"
#include "protocol/cCachingSha2Scramble.hpp"
#include "protocol/cErrPacketCodec.hpp"
#include "protocol/cHandshakeResponse41Codec.hpp"
#include "protocol/cHandshakeV10Codec.hpp"
#include "protocol/cOkPacketCodec.hpp"
#include "protocol/cPacketFramer.hpp"
#include "protocol/eCommand.hpp"
#include "protocol/eCompressionAlgorithm.hpp"
#include "protocol/hCapabilityFlags.hpp"
#include "protocol/hProtocolLimits.hpp"
#include "server/hServerVersion.hpp"
#include "status/cRelayStatusTracker.hpp"

#include <gtest/gtest.h>
#include <functional>

namespace binlog_streamer {
namespace {

constexpr char SOURCE_VERSION[] = "8.4.11";
constexpr char RELAY_NAME[] = "binlog-streamer";
constexpr char RELAY_VERSION[] = "0.17.2";
// What the listener hands a connection, built here so the greeting
// assertions below still read as one string.
const std::string RELAY_SERVER_VERSION =
    ServerVersionString(SOURCE_VERSION, RELAY_NAME, RELAY_VERSION);

IpAddress MakeIpv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                   std::uint8_t d) {
  IpAddress address{};
  address.family = AddressFamily::Ipv4;
  address.bytes[0] = a;
  address.bytes[1] = b;
  address.bytes[2] = c;
  address.bytes[3] = d;
  return address;
}

ReplicaClient MakeClient(std::string user, std::string password,
                         const IpAddress &allowedAddress) {
  ReplicaClient client;
  client.user = std::move(user);
  client.password = std::move(password);
  AddressRange range;
  range.address = allowedAddress;
  range.prefixLength = 32;
  client.hosts.push_back(range);
  return client;
}

// A fixed 21-byte nonce every call, deterministic enough to precompute the
// matching CachingSha2Scramble::Compute() result.
ReplicaConnection::NonceGenerator FixedNonce() {
  return [] {
    std::array<std::uint8_t, SCRAMBLE_LENGTH + 1> nonce{};
    for (std::size_t i = 0; i < SCRAMBLE_LENGTH; ++i)
      nonce[i] = static_cast<std::uint8_t>('A' + (i % 26));
    nonce[SCRAMBLE_LENGTH] = 0;
    return nonce;
  };
}

std::array<std::uint8_t, SCRAMBLE_LENGTH> NonceHead(
    const std::array<std::uint8_t, SCRAMBLE_LENGTH + 1> &nonce) {
  std::array<std::uint8_t, SCRAMBLE_LENGTH> head{};
  std::copy(nonce.begin(), nonce.begin() + SCRAMBLE_LENGTH, head.begin());
  return head;
}

std::vector<std::uint8_t> EncodeHandshakeResponse(
    std::string_view username, std::span<const std::uint8_t> authResponse,
    std::string_view authPluginName, std::uint32_t extraCapabilities = 0,
    std::uint8_t zstdCompressionLevel = 0,
    std::vector<std::pair<std::string, std::string>> attributes = {}) {
  HandshakeResponse41 response;
  if (!attributes.empty()) extraCapabilities |= CLIENT_CONNECT_ATTRS;
  response.connectionAttributes = std::move(attributes);
  response.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH |
                          CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA |
                          CLIENT_LONG_PASSWORD | extraCapabilities;
  response.zstdCompressionLevel = zstdCompressionLevel;
  response.maxPacketSize = 16UL * 1024UL * 1024UL;
  response.characterSet = 8;
  response.username = std::string(username);
  response.authResponse.assign(authResponse.begin(), authResponse.end());
  response.authPluginName = std::string(authPluginName);
  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::Encode(response, encoded);
  return encoded;
}

void AppendClientPacket(test::FakeTransport &transport,
                        std::span<const std::uint8_t> payload,
                        std::uint8_t &sequenceId) {
  PacketFramer::Encode(payload, sequenceId, transport.incoming);
}

// Deliberately not a PacketChannel over a mirror transport: the connection
// shares one sequence-id counter between reads and writes, so server-written
// packets skip ids the client's own packets consumed - this reader trusts each
// packet's own header instead.
class ResponseReader {
 public:
  explicit ResponseReader(const test::FakeTransport &serverSide) {
    for (const auto &framed : serverSide.writes)
      m_buffer.insert(m_buffer.end(), framed.begin(), framed.end());
  }

  bool Next(std::vector<std::uint8_t> &payload, std::string &error) {
    if (m_offset >= m_buffer.size()) {
      error = "no more data";
      return false;
    }
    const std::span<const std::uint8_t> pending(m_buffer.data() + m_offset,
                                                m_buffer.size() - m_offset);
    if (pending.size() < PACKET_HEADER_SIZE) {
      error = "truncated packet header";
      return false;
    }
    std::uint8_t sequenceId = pending[3];
    const PacketDecodeResult decoded =
        PacketFramer::Decode(pending, sequenceId, payload);
    if (decoded.status != PacketDecodeStatus::Complete) {
      error = "incomplete or malformed packet";
      return false;
    }
    m_offset += decoded.bytesConsumed;
    return true;
  }

  // What Next() has not consumed - where a test reads on past the
  // uncompressed login packets into the compressed frames.
  std::span<const std::uint8_t> Remaining() const {
    return std::span<const std::uint8_t>(m_buffer.data() + m_offset,
                                         m_buffer.size() - m_offset);
  }

 private:
  std::vector<std::uint8_t> m_buffer;
  std::size_t m_offset = 0;
};

// One compressed frame: the 7-byte header, then the body. Every payload a
// test here sends or expects is below MIN_COMPRESS_LENGTH, so the body is
// the plain bytes and the length before compression is 0.
void AppendUncompressedFrame(test::FakeTransport &transport,
                             std::span<const std::uint8_t> body,
                             std::uint8_t frameSequenceId) {
  transport.incoming.push_back(static_cast<std::uint8_t>(body.size()));
  transport.incoming.push_back(static_cast<std::uint8_t>(body.size() >> 8));
  transport.incoming.push_back(static_cast<std::uint8_t>(body.size() >> 16));
  transport.incoming.push_back(frameSequenceId);
  transport.incoming.insert(transport.incoming.end(), 3, std::uint8_t{0});
  transport.incoming.insert(transport.incoming.end(), body.begin(), body.end());
}

testing::AssertionResult NextFrame(std::span<const std::uint8_t> &rest,
                                   std::uint8_t expectedFrameSequenceId,
                                   std::vector<std::uint8_t> &body) {
  if (rest.size() < COMPRESSED_HEADER_SIZE)
    return testing::AssertionFailure() << "no compressed header left";
  const std::size_t length = static_cast<std::size_t>(rest[0]) |
                             (static_cast<std::size_t>(rest[1]) << 8) |
                             (static_cast<std::size_t>(rest[2]) << 16);
  if (rest[3] != expectedFrameSequenceId)
    return testing::AssertionFailure()
           << "frame sequence id " << static_cast<unsigned>(rest[3])
           << ", expected " << static_cast<unsigned>(expectedFrameSequenceId);
  const std::size_t plainLength = static_cast<std::size_t>(rest[4]) |
                                  (static_cast<std::size_t>(rest[5]) << 8) |
                                  (static_cast<std::size_t>(rest[6]) << 16);
  if (plainLength != 0)
    return testing::AssertionFailure()
           << "expected a payload stored as is, got one compressed from "
           << plainLength << " bytes";
  if (rest.size() - COMPRESSED_HEADER_SIZE < length)
    return testing::AssertionFailure() << "frame body cut short";
  const auto frameBody = rest.subspan(COMPRESSED_HEADER_SIZE, length);
  body.assign(frameBody.begin(), frameBody.end());
  rest = rest.subspan(COMPRESSED_HEADER_SIZE + length);
  return testing::AssertionSuccess();
}

ConnectionServices CompressionOffered(
    CompressionAlgorithm algorithm = CompressionAlgorithm::Zstd) {
  ConnectionServices services;
  services.compression = algorithm;
  return services;
}

// A loaded server context, generated once: keygen is the slow part.
const TlsContext &ServerTls() {
  static const TlsContext &context = [] {
    auto *loaded = new TlsContext;  // lives for the process, as a static
    GeneratedCertificates certificates;
    std::string error;
    if (!TlsCertificateGenerator::Generate("test", certificates, error) ||
        !loaded->LoadServer(
            TlsMaterial{certificates.caCertPem, certificates.serverCertPem,
                        certificates.serverKeyPem},
            error))
      ADD_FAILURE() << error;
    return std::cref(*loaded);
  }();
  return context;
}

ConnectionServices TlsOffered(bool requireSecureTransport = false) {
  ConnectionServices services;
  services.tls = &ServerTls();
  services.requireSecureTransport = requireSecureTransport;
  return services;
}

ReplicaClientList::Snapshot Shared(std::vector<ReplicaClient> clients) {
  return std::make_shared<const std::vector<ReplicaClient>>(std::move(clients));
}

}  // namespace

TEST(ReplicaConnectionTest,
     TooManyConnectionsSendsErr1040AsTheFirstPacketAndCloses) {
  test::FakeTransport transport;
  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, MakeIpv4(198, 51, 100, 5), {}, RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::TooManyConnections);
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1040);
  // Nothing negotiated yet: no '#' + SQLSTATE, as mysqld sends it.
  EXPECT_NE(payload[3], '#');
  EXPECT_EQ(err.sqlState, "");
  EXPECT_EQ(err.message, "Too many connections");
  EXPECT_FALSE(responses.Next(payload, error));
  ASSERT_FALSE(logLines.empty());
  EXPECT_NE(logLines.front().find("too many connections"), std::string::npos);
}

TEST(ReplicaConnectionTest,
     AddressNotInAnyClientsHostsGetsErr1130BeforeAnyReadAndNoGreeting) {
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{
      MakeClient("repl", "secret", MakeIpv4(203, 0, 113, 9))};
  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, MakeIpv4(198, 51, 100, 5) /* not in 203.0.113.9/32 */,
      Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); });
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1130);
  EXPECT_NE(payload[3], '#');
  EXPECT_EQ(err.sqlState, "");
  EXPECT_EQ(
      err.message,
      "Host '198.51.100.5' is not allowed to connect to this MySQL server");
  EXPECT_EQ(transport.readCallCount, 0u);
}

TEST(ReplicaConnectionTest,
     CorrectPasswordFromTheAllowedHostLogsInWithoutAuthSwitch) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  // 0 is the greeting, written by the connection under test.
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password"),
      clientSequenceId);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  HandshakeV10 greeting;
  ASSERT_TRUE(HandshakeV10Codec::Parse(payload, greeting, error)) << error;
  EXPECT_EQ(greeting.authPluginName, "caching_sha2_password");
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  AuthMoreDataSignal signal{};
  std::span<const std::uint8_t> signalData;
  ASSERT_TRUE(AuthMoreDataCodec::Parse(payload, signal, signalData, error))
      << error;
  EXPECT_EQ(signal, AuthMoreDataSignal::FastAuthSuccess);
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
  EXPECT_FALSE(responses.Next(payload, error));

  // logLines[0], not .back(): with no post-login command scripted,
  // CommandLoop() immediately hits the end and logs its own "closed" line
  // right after "accepted".
  ASSERT_FALSE(logLines.empty());
  EXPECT_NE(logLines.front().find("accepted"), std::string::npos);
  EXPECT_NE(logLines.front().find("user=repl"), std::string::npos);
}

TEST(ReplicaConnectionTest,
     ClientOfferingADifferentPluginGetsAuthSwitchRequestThenLogsIn) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  // The switch reuses the same fixed generator; fine for this test.
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport, EncodeHandshakeResponse("repl", {}, "mysql_native_password"),
      clientSequenceId);
  // The connection's own AuthSwitchRequest takes the next turn (seq 2) first.
  ++clientSequenceId;
  AppendClientPacket(
      transport, scramble,
      clientSequenceId);  // raw scramble reply, no packet-type wrapper

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  AuthSwitchRequest switchRequest;
  ASSERT_TRUE(AuthSwitchRequestCodec::Parse(payload, switchRequest, error))
      << error;
  EXPECT_EQ(switchRequest.pluginName, "caching_sha2_password");
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));

  // logLines[0], not .back(): same reasoning as above - no post-login
  // command scripted here either.
  ASSERT_FALSE(logLines.empty());
  EXPECT_NE(logLines.front().find("accepted"), std::string::npos);
}

// A server replica names itself and its version in its connection
// attributes (sql/rpl_replica.cc, sql-common/client.cc); the status page
// shows them instead of the login.
TEST(ReplicaConnectionTest, RegistersTheProgramAndVersionTheReplicaSent) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password", 0, 0,
                              {{"_client_name", "libmysql"},
                               {"_client_version", "8.4.6-6"},
                               {"program_name", "mysqld"},
                               {"_client_role", "binary_log_listener"}}),
      clientSequenceId);

  RelayStatusTracker tracker("relay", "0.0.0", 151, nullptr);
  ConnectionServices services;
  services.status = &tracker;
  std::vector<ReplicaStatus> registered;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) {
        // "closed" is logged while the replica is still registered.
        if (line.find("closed") != std::string::npos)
          registered = tracker.Snapshot().replicas;
      },
      ConnectionRefusal::None, FixedNonce(), services);
  connection.Run();

  ASSERT_EQ(registered.size(), 1u);
  EXPECT_EQ(registered[0].facts.user, "repl");
  EXPECT_EQ(registered[0].facts.program, "mysqld");
  EXPECT_EQ(registered[0].facts.version, "8.4.6-6");
  EXPECT_TRUE(tracker.Snapshot().replicas.empty());
}

TEST(ReplicaConnectionTest, RegistersNoVersionWhenTheReplicaSentNone) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password"),
      clientSequenceId);

  RelayStatusTracker tracker("relay", "0.0.0", 151, nullptr);
  ConnectionServices services;
  services.status = &tracker;
  std::vector<ReplicaStatus> registered;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) {
        if (line.find("closed") != std::string::npos)
          registered = tracker.Snapshot().replicas;
      },
      ConnectionRefusal::None, FixedNonce(), services);
  connection.Run();

  ASSERT_EQ(registered.size(), 1u);
  EXPECT_EQ(registered[0].facts.program, "");
  EXPECT_EQ(registered[0].facts.version, "");
}

TEST(ReplicaConnectionTest, WrongPasswordGetsErr1045) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto wrongScramble = CachingSha2Scramble::Compute(
      "not-the-password",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", wrongScramble, "caching_sha2_password"),
      clientSequenceId);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1045);
  EXPECT_EQ(err.sqlState, "28000");
  EXPECT_EQ(
      err.message,
      "Access denied for user 'repl'@'203.0.113.9' (using password: YES)");
}

TEST(ReplicaConnectionTest,
     UnknownUserGoesThroughTheSameExchangeAsAKnownOneAndGetsErr1045) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto someScramble = CachingSha2Scramble::Compute(
      "whatever",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("ghost", someScramble, "caching_sha2_password"),
      clientSequenceId);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1045);
  EXPECT_EQ(
      err.message,
      "Access denied for user 'ghost'@'203.0.113.9' (using password: YES)");
}

TEST(ReplicaConnectionTest, MalformedHandshakeResponseGetsErr1043BadHandshake) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  std::uint8_t clientSequenceId = 1;
  // Far short of HandshakeResponse41's 32-byte fixed header.
  const std::vector<std::uint8_t> tooShort{0x00, 0x00};
  AppendClientPacket(transport, tooShort, clientSequenceId);

  ReplicaConnection connection(transport, peer, Shared(clients),
                               RELAY_SERVER_VERSION, {},
                               ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1043);
  EXPECT_EQ(err.sqlState, "08S01");
  EXPECT_EQ(err.message, "Bad handshake");
}

namespace {

// Runs a full successful login, then appends one or more already-framed
// post-login command packets (sequence ids restarting at 0, as a real
// client does) before Run() executes.
class LoggedInConnectionFixture {
 public:
  LoggedInConnectionFixture() {
    const std::vector<ReplicaClient> clients{
        MakeClient("repl", "s3cret", m_peer)};
    m_clients = clients;
    const auto nonce = FixedNonce()();
    const auto scramble = CachingSha2Scramble::Compute(
        "s3cret",
        std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
    std::uint8_t clientSequenceId = 1;
    AppendClientPacket(
        m_transport,
        EncodeHandshakeResponse("repl", scramble, "caching_sha2_password"),
        clientSequenceId);
    // This fixture queues login and post-login bytes into one buffer before
    // Run() starts, so an unbounded Read() would hand Login()'s short-lived
    // PacketChannel the command bytes too, losing them when it's destroyed -
    // capping bytes per Read() to 1 avoids that.
    m_transport.maxBytesPerRead = 1;
  }

  void AppendCommand(std::span<const std::uint8_t> payload) {
    std::uint8_t sequenceId = 0;
    AppendClientPacket(m_transport, payload, sequenceId);
  }

  void Run() {
    ReplicaConnection connection(
        m_transport, m_peer, Shared(m_clients), RELAY_SERVER_VERSION,
        [this](const std::string &line) { logLines.push_back(line); },
        ConnectionRefusal::None, FixedNonce());
    connection.Run();
  }

  test::FakeTransport m_transport;
  std::vector<std::string> logLines;

 private:
  IpAddress m_peer = MakeIpv4(203, 0, 113, 9);
  std::vector<ReplicaClient> m_clients;
};

}  // namespace

TEST(ReplicaConnectionTest,
     PingGetsAnOkAndTheConnectionStaysOpenForTheNextCommand) {
  LoggedInConnectionFixture fixture;
  fixture.AppendCommand(
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Ping)});
  // A second one - proves the loop continues, not just answers once.
  fixture.AppendCommand(
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Ping)});
  fixture.Run();

  ResponseReader responses(fixture.m_transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
}

TEST(ReplicaConnectionTest,
     UnknownCommandGetsErr1047AndTheConnectionStaysOpen) {
  LoggedInConnectionFixture fixture;
  // COM_QUERY: no queryResponder wired here.
  fixture.AppendCommand(std::vector<std::uint8_t>{0x03, 'x'});
  fixture.AppendCommand(
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Ping)});
  fixture.Run();

  ResponseReader responses(fixture.m_transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1047);
  EXPECT_EQ(err.sqlState, "08S01");
  EXPECT_EQ(err.message, "Unknown command");
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
}

TEST(ReplicaConnectionTest,
     DumpByFileAndPositionGetsErr1236NamingGtidAutoPositioning) {
  LoggedInConnectionFixture fixture;
  // COM_BINLOG_DUMP: position 4, flags 0, server_id 7, "binlog.000001".
  const auto command = static_cast<std::uint8_t>(Command::BinlogDump);
  std::vector<std::uint8_t> dump{command, 4, 0, 0, 0, 0, 0, 7, 0, 0, 0};
  for (const char c : std::string("binlog.000001"))
    dump.push_back(static_cast<std::uint8_t>(c));
  fixture.AppendCommand(dump);
  fixture.AppendCommand(
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Ping)});
  fixture.Run();

  ResponseReader responses(fixture.m_transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1236);
  EXPECT_EQ(err.sqlState, "HY000");
  EXPECT_NE(err.message.find("GTID auto-positioning only"), std::string::npos);
  // The connection stays open, as after any refused dump.
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
}

TEST(ReplicaConnectionTest, QuitClosesWithoutAnyResponse) {
  LoggedInConnectionFixture fixture;
  fixture.AppendCommand(
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Quit)});
  fixture.Run();

  ResponseReader responses(fixture.m_transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  ASSERT_TRUE(responses.Next(payload, error));
  EXPECT_FALSE(responses.Next(payload, error));
  ASSERT_FALSE(fixture.logLines.empty());
  EXPECT_NE(fixture.logLines.back().find("COM_QUIT"), std::string::npos);
}

TEST(ReplicaConnectionTest, GreetingOffersNoCompressionUnlessTheRelayAllowsIt) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  ReplicaConnection connection(transport, peer, Shared(clients),
                               RELAY_SERVER_VERSION, nullptr,
                               ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  HandshakeV10 greeting;
  ASSERT_TRUE(HandshakeV10Codec::Parse(payload, greeting, error)) << error;
  EXPECT_EQ(greeting.capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM, 0u);
}

TEST(ReplicaConnectionTest, GreetingOffersZstdWhenTheRelayAllowsIt) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION, nullptr,
      ConnectionRefusal::None, FixedNonce(), CompressionOffered());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  HandshakeV10 greeting;
  ASSERT_TRUE(HandshakeV10Codec::Parse(payload, greeting, error)) << error;
  EXPECT_NE(greeting.capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM, 0u);
}

TEST(ReplicaConnectionTest, ZstdReplicaGetsAnUncompressedOkAndThenFrames) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password",
                              CLIENT_ZSTD_COMPRESSION_ALGORITHM,
                              DEFAULT_ZSTD_COMPRESSION_LEVEL),
      clientSequenceId);
  // One byte per read: the login channel is replaced for the command
  // phase, so whatever it buffers past the handshake response is lost.
  transport.maxBytesPerRead = 1;
  // From here the wire is frames. Each command restarts the frame counter
  // with the packet sequence id, so both commands go in frame 0.
  const std::vector<std::uint8_t> ping{
      1, 0, 0, 0, static_cast<std::uint8_t>(Command::Ping)};
  AppendUncompressedFrame(transport, ping, 0);
  AppendUncompressedFrame(transport, ping, 0);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce(), CompressionOffered());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // fast auth success
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // login OK
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));

  std::span<const std::uint8_t> rest = responses.Remaining();
  std::vector<std::uint8_t> body;
  // Frame 1, not 0: one counter serves both directions, and the replica's
  // own frame took 0.
  ASSERT_TRUE(NextFrame(rest, 1, body));
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(body.size() > PACKET_HEADER_SIZE
                                            ? body[PACKET_HEADER_SIZE]
                                            : std::uint8_t{0},
                                        false));
  ASSERT_TRUE(NextFrame(rest, 1, body));
  EXPECT_TRUE(rest.empty());

  ASSERT_FALSE(logLines.empty());
  EXPECT_NE(logLines.front().find("zstd compression, level 3"),
            std::string::npos)
      << logLines.front();
}

TEST(ReplicaConnectionTest, ZstdLevelOutsideTheAllowedRangeGetsErr3923) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  for (const std::uint8_t level : {std::uint8_t{0}, std::uint8_t{23}}) {
    test::FakeTransport transport;
    const std::vector<ReplicaClient> clients{
        MakeClient("repl", "s3cret", peer)};
    const auto nonce = FixedNonce()();
    const auto scramble = CachingSha2Scramble::Compute(
        "s3cret",
        std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
    std::uint8_t clientSequenceId = 1;
    AppendClientPacket(
        transport,
        EncodeHandshakeResponse("repl", scramble, "caching_sha2_password",
                                CLIENT_ZSTD_COMPRESSION_ALGORITHM, level),
        clientSequenceId);

    ReplicaConnection connection(
        transport, peer, Shared(clients), RELAY_SERVER_VERSION, nullptr,
        ConnectionRefusal::None, FixedNonce(), CompressionOffered());
    connection.Run();

    ResponseReader responses(transport);
    std::vector<std::uint8_t> payload;
    std::string error;
    ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
    ASSERT_TRUE(responses.Next(payload, error)) << error;
    ErrPacket err;
    ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
    EXPECT_EQ(err.errorCode, 3923) << static_cast<unsigned>(level);
    EXPECT_EQ(err.sqlState, "HY000");
    EXPECT_EQ(err.message,
              "Invalid zstd compression level for algorithm "
              "'zstd'.");
    EXPECT_FALSE(responses.Next(payload, error));
  }
}

TEST(ReplicaConnectionTest, ZstdAskedForButNotOfferedLeavesTheWirePlain) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  // A level the relay would refuse if it had agreed to compress at all.
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password",
                              CLIENT_ZSTD_COMPRESSION_ALGORITHM, 0),
      clientSequenceId);
  transport.maxBytesPerRead = 1;
  std::uint8_t commandSequenceId = 0;
  AppendClientPacket(
      transport,
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Ping)},
      commandSequenceId);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // fast auth success
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // login OK
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // PING OK
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
}

TEST(ReplicaConnectionTest, GreetingOffersZlibWhenTheRelayAllowsIt) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  ReplicaConnection connection(transport, peer, Shared(clients),
                               RELAY_SERVER_VERSION, nullptr,
                               ConnectionRefusal::None, FixedNonce(),
                               CompressionOffered(CompressionAlgorithm::Zlib));
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  HandshakeV10 greeting;
  ASSERT_TRUE(HandshakeV10Codec::Parse(payload, greeting, error)) << error;
  EXPECT_NE(greeting.capabilities & CLIENT_COMPRESS, 0u);
  // One algorithm is offered, never both: a client that set each bit would
  // otherwise get to pick, and MySQL's own tie-break is zlib.
  EXPECT_EQ(greeting.capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM, 0u);
}

TEST(ReplicaConnectionTest, ZlibReplicaGetsAnUncompressedOkAndThenFrames) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  // No level byte follows CLIENT_COMPRESS: the protocol has none for zlib,
  // and the codec writes one only for the zstd capability.
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password",
                              CLIENT_COMPRESS),
      clientSequenceId);
  transport.maxBytesPerRead = 1;
  const std::vector<std::uint8_t> ping{
      1, 0, 0, 0, static_cast<std::uint8_t>(Command::Ping)};
  AppendUncompressedFrame(transport, ping, 0);
  AppendUncompressedFrame(transport, ping, 0);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce(),
      CompressionOffered(CompressionAlgorithm::Zlib));
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // fast auth success
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // login OK
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));

  std::span<const std::uint8_t> rest = responses.Remaining();
  std::vector<std::uint8_t> body;
  ASSERT_TRUE(NextFrame(rest, 1, body));
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(body.size() > PACKET_HEADER_SIZE
                                            ? body[PACKET_HEADER_SIZE]
                                            : std::uint8_t{0},
                                        false));
  ASSERT_TRUE(NextFrame(rest, 1, body));
  EXPECT_TRUE(rest.empty());

  ASSERT_FALSE(logLines.empty());
  // Level 6 without the replica saying so, because MySQL hardcodes it.
  EXPECT_NE(logLines.front().find("zlib compression, level 6"),
            std::string::npos)
      << logLines.front();
}

TEST(ReplicaConnectionTest, ZlibReplicaIsNeverRefusedOverACompressionLevel) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  // A zstd level the relay would call out of range, on a link where no
  // level was asked for: err 3923 belongs to zstd alone.
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password",
                              CLIENT_COMPRESS, 23),
      clientSequenceId);
  transport.maxBytesPerRead = 1;

  ReplicaConnection connection(transport, peer, Shared(clients),
                               RELAY_SERVER_VERSION, nullptr,
                               ConnectionRefusal::None, FixedNonce(),
                               CompressionOffered(CompressionAlgorithm::Zlib));
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // fast auth success
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // login OK
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
}

TEST(ReplicaConnectionTest,
     ZlibAskedForWhileTheRelayOffersZstdLeavesTheWirePlain) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password",
                              CLIENT_COMPRESS),
      clientSequenceId);
  transport.maxBytesPerRead = 1;
  std::uint8_t commandSequenceId = 0;
  AppendClientPacket(
      transport,
      std::vector<std::uint8_t>{static_cast<std::uint8_t>(Command::Ping)},
      commandSequenceId);

  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION, nullptr,
      ConnectionRefusal::None, FixedNonce(), CompressionOffered());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // fast auth success
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // login OK
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // PING OK
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false));
}

TEST(ReplicaConnectionTest, GreetingOffersTlsOnlyWithACertificateToPresent) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  for (const bool offered : {false, true}) {
    test::FakeTransport transport;
    ReplicaConnection connection(transport, peer, Shared(clients),
                                 RELAY_SERVER_VERSION, nullptr,
                                 ConnectionRefusal::None, FixedNonce(),
                                 offered ? TlsOffered() : ConnectionServices{});
    connection.Run();

    ResponseReader responses(transport);
    std::vector<std::uint8_t> payload;
    std::string error;
    ASSERT_TRUE(responses.Next(payload, error)) << error;
    HandshakeV10 greeting;
    ASSERT_TRUE(HandshakeV10Codec::Parse(payload, greeting, error)) << error;
    EXPECT_EQ((greeting.capabilities & CLIENT_SSL) != 0, offered);
  }
}

TEST(ReplicaConnectionTest, SslRequestWithoutACertificateGetsErr1043) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  HandshakeResponse41 request;
  request.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH | CLIENT_SSL;
  request.maxPacketSize = 16UL * 1024UL * 1024UL;
  request.characterSet = 8;
  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::EncodeSslRequest(request, encoded);
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(transport, encoded, clientSequenceId);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce());
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 1043);
  EXPECT_EQ(err.sqlState, "08S01");
  EXPECT_EQ(err.message, "Bad handshake");
  ASSERT_EQ(logLines.size(), 1u);
  EXPECT_NE(logLines[0].find("TLS requested but not offered"),
            std::string::npos)
      << logLines[0];
}

TEST(ReplicaConnectionTest, PlainLoginUnderRequireSecureTransportGetsErr3159) {
  const IpAddress peer = MakeIpv4(203, 0, 113, 9);
  test::FakeTransport transport;
  const std::vector<ReplicaClient> clients{MakeClient("repl", "s3cret", peer)};
  const auto nonce = FixedNonce()();
  const auto scramble = CachingSha2Scramble::Compute(
      "s3cret",
      std::span<const std::uint8_t, SCRAMBLE_LENGTH>(NonceHead(nonce)));
  std::uint8_t clientSequenceId = 1;
  AppendClientPacket(
      transport,
      EncodeHandshakeResponse("repl", scramble, "caching_sha2_password"),
      clientSequenceId);

  std::vector<std::string> logLines;
  ReplicaConnection connection(
      transport, peer, Shared(clients), RELAY_SERVER_VERSION,
      [&](const std::string &line) { logLines.push_back(line); },
      ConnectionRefusal::None, FixedNonce(), TlsOffered(true));
  connection.Run();

  ResponseReader responses(transport);
  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(responses.Next(payload, error)) << error;  // greeting
  ASSERT_TRUE(responses.Next(payload, error)) << error;
  ErrPacket err;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, err, error)) << error;
  EXPECT_EQ(err.errorCode, 3159);
  EXPECT_EQ(err.sqlState, "HY000");
  EXPECT_EQ(err.message,
            "Connections using insecure transport are prohibited while "
            "--require_secure_transport=ON.");
  ASSERT_EQ(logLines.size(), 1u);
  EXPECT_NE(logLines[0].find("insecure transport"), std::string::npos)
      << logLines[0];
}

}  // namespace binlog_streamer
