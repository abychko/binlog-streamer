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

#include "server/cReplicaListener.hpp"

#include "net/cPacketChannel.hpp"
#include "net/cTcpTransport.hpp"
#include "net/cTlsCertificateGenerator.hpp"
#include "net/cTlsContext.hpp"
#include "protocol/cAuthMoreDataCodec.hpp"
#include "protocol/cAuthSwitchRequestCodec.hpp"
#include "protocol/cCachingSha2Scramble.hpp"
#include "protocol/cErrPacketCodec.hpp"
#include "protocol/cHandshakeResponse41Codec.hpp"
#include "protocol/cHandshakeV10Codec.hpp"
#include "protocol/cOkPacketCodec.hpp"
#include "protocol/hCapabilityFlags.hpp"
#include "protocol/hProtocolLimits.hpp"
#include "receiver/cReplicaSession.hpp"
#include "server/hServerVersion.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace binlog_streamer {
namespace {

constexpr PacketChannelOptions CLIENT_CHANNEL_OPTIONS{
    std::chrono::milliseconds{3000}, std::chrono::milliseconds{3000},
    16UL * 1024UL * 1024UL, "relay"};

IpAddress Loopback() {
  IpAddress address{};
  address.family = AddressFamily::Ipv4;
  address.bytes[0] = 127;
  address.bytes[3] = 1;
  return address;
}

ReplicaClient MakeClient(std::string user, std::string password,
                         const IpAddress &allowedAddress,
                         std::uint8_t prefixLength = 32) {
  ReplicaClient client;
  client.user = std::move(user);
  client.password = std::move(password);
  AddressRange range;
  range.address = allowedAddress;
  range.prefixLength = prefixLength;
  client.hosts.push_back(range);
  return client;
}

struct LoginAttempt {
  bool ok = false;
  ErrPacket err;
};

LoginAttempt AttemptLogin(std::uint16_t port, std::string_view username,
                          std::string_view password) {
  LoginAttempt result;
  TcpTransport transport;
  std::string error;
  if (!transport.Connect("127.0.0.1", port, std::chrono::milliseconds(3000),
                         error)) {
    ADD_FAILURE() << "connect: " << error;
    return result;
  }
  PacketChannel channel(transport, CLIENT_CHANNEL_OPTIONS);

  std::vector<std::uint8_t> payload;
  if (!channel.ReadPacket(payload, error)) {
    ADD_FAILURE() << "reading greeting: " << error;
    return result;
  }
  if (ErrPacketCodec::IsErrPacket(payload)) {
    ErrPacketCodec::Parse(payload, result.err, error);
    return result;
  }
  HandshakeV10 greeting;
  if (!HandshakeV10Codec::Parse(payload, greeting, error)) {
    ADD_FAILURE() << "parsing greeting: " << error;
    return result;
  }
  EXPECT_EQ(greeting.authPluginName, "caching_sha2_password");
  if (greeting.authPluginData.size() < SCRAMBLE_LENGTH) {
    ADD_FAILURE() << "greeting nonce shorter than expected";
    return result;
  }
  const auto scramble = CachingSha2Scramble::Compute(
      password, std::span<const std::uint8_t, SCRAMBLE_LENGTH>(
                    greeting.authPluginData.data(), SCRAMBLE_LENGTH));

  HandshakeResponse41 response;
  response.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH |
                          CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA |
                          CLIENT_LONG_PASSWORD;
  response.maxPacketSize = 16UL * 1024UL * 1024UL;
  response.characterSet = 8;
  response.username = std::string(username);
  response.authResponse.assign(scramble.begin(), scramble.end());
  response.authPluginName = "caching_sha2_password";
  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::Encode(response, encoded);
  if (!channel.WritePacket(encoded, error)) {
    ADD_FAILURE() << "sending handshake response: " << error;
    return result;
  }

  if (!channel.ReadPacket(payload, error)) {
    ADD_FAILURE() << "reading login result: " << error;
    return result;
  }
  if (ErrPacketCodec::IsErrPacket(payload)) {
    ErrPacketCodec::Parse(payload, result.err, error);
    return result;
  }
  AuthMoreDataSignal signal{};
  std::span<const std::uint8_t> signalData;
  if (!AuthMoreDataCodec::Parse(payload, signal, signalData, error) ||
      signal != AuthMoreDataSignal::FastAuthSuccess) {
    ADD_FAILURE() << "expected AuthMoreData fast-auth-success";
    return result;
  }
  if (!channel.ReadPacket(payload, error)) {
    ADD_FAILURE() << "reading final OK: " << error;
    return result;
  }
  result.ok = OkPacketCodec::IsOkPacket(
      payload.empty() ? std::uint8_t{0} : payload[0], false);
  if (result.ok) transport.Close();
  return result;
}

class ReplicaListenerLoopbackTest : public ::testing::Test {
 protected:
  std::atomic<bool> stopRequested{false};
  WakeupPipe wakeupPipe;

  void SetUp() override {
    std::string error;
    ASSERT_TRUE(wakeupPipe.Open(error)) << error;
  }
};

std::uint16_t FindFreePort() {
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  bind(probe, reinterpret_cast<struct sockaddr *>(&address), sizeof(address));
  socklen_t addressLength = sizeof(address);
  getsockname(probe, reinterpret_cast<struct sockaddr *>(&address),
              &addressLength);
  const std::uint16_t port = ntohs(address.sin_port);
  close(probe);
  return port;
}

class FixedServerState : public ServerState {
 public:
  explicit FixedServerState(std::string sourceVersion = "8.4.11")
      : m_sourceVersion(std::move(sourceVersion)) {}
  std::string GtidPurged() const override { return ""; }
  std::string GtidExecuted() const override {
    return "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9";
  }
  std::string SourceVersion() const override { return m_sourceVersion; }
  std::string BinlogChecksum() const override { return "CRC32"; }
  std::optional<PreviousGtidsEvent> PreviousGtids(
      const std::string &) const override {
    return std::nullopt;
  }

 private:
  std::string m_sourceVersion;
};

constexpr char RELAY_NAME[] = "binlog-streamer";
constexpr char RELAY_VERSION[] = "0.27.0";

ServerIdentity TestIdentity() {
  return ServerIdentity{1001, "8a94f357-aab4-11df-86ab-c80aa9429562",
                        "test relay", RELAY_NAME, RELAY_VERSION};
}

TEST_F(ReplicaListenerLoopbackTest, RealClientLogsInWithTheCorrectPassword) {
  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  settings.clients = {MakeClient("repl", "s3cret", Loopback())};
  FixedServerState state;
  ReplicaListener listener(settings, DEFAULT_MAX_CONNECTIONS, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  const LoginAttempt attempt =
      AttemptLogin(settings.listenPort, "repl", "s3cret");
  EXPECT_TRUE(attempt.ok);

  listener.Stop();
}

TEST_F(ReplicaListenerLoopbackTest, RealClientWithTheWrongPasswordGetsErr1045) {
  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  settings.clients = {MakeClient("repl", "s3cret", Loopback())};
  FixedServerState state;
  ReplicaListener listener(settings, DEFAULT_MAX_CONNECTIONS, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  const LoginAttempt attempt =
      AttemptLogin(settings.listenPort, "repl", "wrong-password");
  EXPECT_FALSE(attempt.ok);
  EXPECT_EQ(attempt.err.errorCode, 1045);

  listener.Stop();
}

TEST_F(ReplicaListenerLoopbackTest,
       RealClientFromADisallowedAddressGetsErr1130) {
  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  // 203.0.113.0/24 (RFC 5737, a documentation range): no configured client
  // allows loopback, and this keeps the test independent of what loopback
  // resolves to.
  IpAddress documentationRange{};
  documentationRange.family = AddressFamily::Ipv4;
  documentationRange.bytes = {203, 0, 113, 0};
  settings.clients = {MakeClient("repl", "s3cret", documentationRange, 24)};
  FixedServerState state;
  ReplicaListener listener(settings, DEFAULT_MAX_CONNECTIONS, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  const LoginAttempt attempt =
      AttemptLogin(settings.listenPort, "repl", "s3cret");
  EXPECT_FALSE(attempt.ok);
  EXPECT_EQ(attempt.err.errorCode, 1130);
  EXPECT_EQ(attempt.err.sqlState, "");
  EXPECT_EQ(attempt.err.message.rfind("Host '", 0), 0u) << attempt.err.message;

  listener.Stop();
}

TEST_F(ReplicaListenerLoopbackTest,
       StopReturnsPromptlyWithConnectionsStillOpenAndWaitingForACommand) {
  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  settings.clients = {MakeClient("repl", "s3cret", Loopback())};
  FixedServerState state;
  ReplicaListener listener(settings, DEFAULT_MAX_CONNECTIONS, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  TcpTransport clientA;
  TcpTransport clientB;
  ASSERT_TRUE(clientA.Connect("127.0.0.1", settings.listenPort,
                              std::chrono::milliseconds(2000), error))
      << error;
  ASSERT_TRUE(clientB.Connect("127.0.0.1", settings.listenPort,
                              std::chrono::milliseconds(2000), error))
      << error;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto start = std::chrono::steady_clock::now();
  stopRequested.store(true);
  wakeupPipe.Wake();
  listener.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

TEST_F(ReplicaListenerLoopbackTest, TheConnectionPastTheLimitGetsErr1040) {
  constexpr unsigned CONNECTION_LIMIT = 3;

  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  settings.clients = {MakeClient("repl", "s3cret", Loopback())};
  FixedServerState state;
  ReplicaListener listener(settings, CONNECTION_LIMIT, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  std::vector<std::unique_ptr<TcpTransport>> held;
  for (unsigned i = 0; i < CONNECTION_LIMIT; ++i) {
    auto transport = std::make_unique<TcpTransport>();
    ASSERT_TRUE(transport->Connect("127.0.0.1", settings.listenPort,
                                   std::chrono::milliseconds(3000), error))
        << "connection " << i << ": " << error;
    PacketChannel channel(*transport, CLIENT_CHANNEL_OPTIONS);
    std::vector<std::uint8_t> greeting;
    ASSERT_TRUE(channel.ReadPacket(greeting, error))
        << "connection " << i << ": " << error;
    ASSERT_FALSE(ErrPacketCodec::IsErrPacket(greeting))
        << "connection " << i << " was refused";
    held.push_back(std::move(transport));
  }

  const LoginAttempt attempt =
      AttemptLogin(settings.listenPort, "repl", "s3cret");
  EXPECT_FALSE(attempt.ok);
  EXPECT_EQ(attempt.err.errorCode, 1040);
  EXPECT_EQ(attempt.err.sqlState, "");
  EXPECT_EQ(attempt.err.message, "Too many connections");

  held.clear();
  listener.Stop();
}

TEST_F(ReplicaListenerLoopbackTest,
       AConnectionGetsErr3168UntilSomethingIsHeld) {
  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  settings.clients = {MakeClient("repl", "s3cret", Loopback())};
  FixedServerState state(/*sourceVersion=*/"");
  ReplicaListener listener(settings, DEFAULT_MAX_CONNECTIONS, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  const LoginAttempt attempt =
      AttemptLogin(settings.listenPort, "repl", "s3cret");
  EXPECT_FALSE(attempt.ok);
  EXPECT_EQ(attempt.err.errorCode, 3168);
  EXPECT_EQ(attempt.err.sqlState, "");

  listener.Stop();
}

TEST_F(ReplicaListenerLoopbackTest,
       TheRelaysOwnClientSideCompletesItsPreparationAgainstTheListener) {
  constexpr char RELAY_UUID[] = "8a94f357-aab4-11df-86ab-c80aa9429562";
  ReplicaSettings settings;
  settings.listenAddress = Loopback();
  settings.listenPort = FindFreePort();
  settings.clients = {MakeClient("repl", "s3cret", Loopback())};
  FixedServerState state;
  ReplicaListener listener(settings, DEFAULT_MAX_CONNECTIONS, &stopRequested,
                           &wakeupPipe, {}, TestIdentity(), &state);
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  SourceSettings source;
  source.host = "127.0.0.1";
  source.port = settings.listenPort;
  source.user = "repl";
  source.password = "s3cret";
  ServerSettings downstream;
  downstream.serverId = 2002;
  ReplicaSessionOptions options;
  TcpTransport transport;
  ReplicaSession session(transport, source, downstream,
                         "11111111-2222-3333-4444-555555555555",
                         "binlog-streamer", "0.18.1", options);
  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered) << result.message;
  EXPECT_EQ(result.identity.serverId, 1001u);
  EXPECT_EQ(result.identity.serverUuid, RELAY_UUID);
  // A downstream relay reads its source's version as the version of the server
  // the events came from; the suffix tells it the answer came from a relay.
  EXPECT_EQ(result.identity.versionString,
            ServerVersionString("8.4.11", RELAY_NAME, RELAY_VERSION));
  EXPECT_TRUE(IsRelayServerVersion(result.identity.versionString, RELAY_NAME));
  EXPECT_EQ(result.identity.gtidMode, "ON");
  EXPECT_EQ(result.identity.checksumAlgorithm, "CRC32");
  EXPECT_GT(result.identity.unixTimestamp, 1700000000u);
  EXPECT_TRUE(result.identity.registered);

  transport.Close();
  listener.Stop();
}

const GeneratedCertificates &Certificates() {
  static const GeneratedCertificates certificates = [] {
    GeneratedCertificates generated;
    std::string error;
    if (!TlsCertificateGenerator::Generate("test", generated, error))
      ADD_FAILURE() << error;
    return generated;
  }();
  return certificates;
}

const TlsContext &ServerTls() {
  static const TlsContext &context = [] {
    auto *loaded = new TlsContext;
    std::string error;
    if (!loaded->LoadServer(
            TlsMaterial{Certificates().caCertPem, Certificates().serverCertPem,
                        Certificates().serverKeyPem},
            error))
      ADD_FAILURE() << error;
    return std::cref(*loaded);
  }();
  return context;
}

SourceSettings SourceFor(const ReplicaSettings &settings, SslMode mode) {
  SourceSettings source;
  source.host = "127.0.0.1";
  source.port = settings.listenPort;
  source.user = "repl";
  source.password = "s3cret";
  source.sslMode = mode;
  return source;
}

class ReplicaListenerTlsLoopbackTest : public ReplicaListenerLoopbackTest {
 protected:
  ReplicaSettings settings;
  FixedServerState state;
  std::unique_ptr<ReplicaListener> listener;

  void StartListener(bool requireSecureTransport = false) {
    settings.listenAddress = Loopback();
    settings.listenPort = FindFreePort();
    settings.clients = {MakeClient("repl", "s3cret", Loopback())};
    settings.requireSecureTransport = requireSecureTransport;
    listener = std::make_unique<ReplicaListener>(
        settings, DEFAULT_MAX_CONNECTIONS, &stopRequested, &wakeupPipe,
        ReplicaListener::LogFunction{}, TestIdentity(), &state, nullptr,
        &ServerTls());
    std::string error;
    ASSERT_TRUE(listener->Start(error)) << error;
  }

  SessionResult RunSession(const SourceSettings &source, bool &encrypted) {
    ServerSettings downstream;
    downstream.serverId = 2002;
    TcpTransport transport;
    ReplicaSession session(
        transport, source, downstream, "11111111-2222-3333-4444-555555555555",
        "binlog-streamer", "0.29.0", ReplicaSessionOptions{});
    const SessionResult result = session.Run();
    encrypted = session.encrypted();
    transport.Close();
    return result;
  }

  void TearDown() override {
    if (listener) listener->Stop();
  }
};

TEST_F(ReplicaListenerTlsLoopbackTest,
       TheRelaysOwnClientSideRegistersOverTlsWhenItRequiresIt) {
  StartListener();
  bool encrypted = false;
  const SessionResult result =
      RunSession(SourceFor(settings, SslMode::Required), encrypted);
  EXPECT_EQ(result.outcome, SessionOutcome::Registered) << result.message;
  EXPECT_TRUE(encrypted);
  EXPECT_NE(result.message.find(" over TLSv"), std::string::npos)
      << result.message;
  EXPECT_TRUE(result.identity.registered);
}

TEST_F(ReplicaListenerTlsLoopbackTest,
       TheRelaysOwnClientSideVerifiesTheListenersCa) {
  StartListener();
  bool encrypted = false;
  SourceSettings source = SourceFor(settings, SslMode::VerifyCa);
  source.tls.caPem = Certificates().caCertPem;
  const SessionResult result = RunSession(source, encrypted);
  EXPECT_EQ(result.outcome, SessionOutcome::Registered) << result.message;
  EXPECT_TRUE(encrypted);
}

TEST_F(ReplicaListenerTlsLoopbackTest,
       TheRelaysOwnClientSideStaysPlainUnderDisabled) {
  StartListener();
  bool encrypted = true;
  const SessionResult result =
      RunSession(SourceFor(settings, SslMode::Disabled), encrypted);
  EXPECT_EQ(result.outcome, SessionOutcome::Registered) << result.message;
  EXPECT_FALSE(encrypted);
  EXPECT_EQ(result.message.find(" over TLSv"), std::string::npos)
      << result.message;
}

TEST_F(ReplicaListenerTlsLoopbackTest,
       APlainClientGetsErr3159UnderRequireSecureTransport) {
  StartListener(/*requireSecureTransport=*/true);
  const LoginAttempt attempt =
      AttemptLogin(settings.listenPort, "repl", "s3cret");
  EXPECT_FALSE(attempt.ok);
  EXPECT_EQ(attempt.err.errorCode, 3159);
  EXPECT_EQ(attempt.err.sqlState, "HY000");

  bool encrypted = false;
  const SessionResult result =
      RunSession(SourceFor(settings, SslMode::Required), encrypted);
  EXPECT_EQ(result.outcome, SessionOutcome::Registered) << result.message;
  EXPECT_TRUE(encrypted);
}

}  // namespace
}  // namespace binlog_streamer
