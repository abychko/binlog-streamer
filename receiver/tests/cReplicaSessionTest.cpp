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
#include "receiver/cSourceClock.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include "cCachingSha2FullAuthPasswordTestSupport.hpp"
#include "cFakeTransport.hpp"
#include "cScriptedSourceBuilder.hpp"
#include "cScriptedSourcePayloads.hpp"
#include "config/sServerSettings.hpp"
#include "config/sSourceSettings.hpp"
#include "gtid/cGtidSet.hpp"
#include "protocol/cComBinlogDumpGtidCommand.hpp"
#include "protocol/cLengthEncodedString.hpp"
#include "protocol/hBinlogDumpFlags.hpp"
#include "protocol/hProtocolLimits.hpp"
#include "receiver/hSessionDefaults.hpp"

namespace binlog_streamer {
namespace {

SourceSettings MakeSource() {
  SourceSettings source;
  source.host = "127.0.0.1";
  source.port = 3306;
  source.user = "repl";
  source.password = "secret";
  return source;
}

TEST(ReplicaSessionTest, SuccessfulSessionRegisters) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");

  const auto before = std::chrono::steady_clock::now();
  const SessionResult result = session.Run();
  const auto after = std::chrono::steady_clock::now();
  EXPECT_EQ(result.identity.unixTimestamp, 1700000000u);
  EXPECT_GE(result.identity.unixTimestampReadAt, before);
  EXPECT_LE(result.identity.unixTimestampReadAt, after);

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
  EXPECT_TRUE(result.identity.registered);
  EXPECT_EQ(result.identity.versionMajor, 8u);
  EXPECT_EQ(result.identity.serverId, 999u);
  EXPECT_EQ(result.identity.checksumAlgorithm, "CRC32");
  EXPECT_EQ(result.identity.gtidMode, "ON");
  EXPECT_EQ(result.identity.serverUuid, "11111111-1111-1111-1111-111111111111");
}

std::uint32_t RequestedCapabilities(const std::vector<std::uint8_t> &write) {
  return static_cast<std::uint32_t>(write[4]) |
         (static_cast<std::uint32_t>(write[5]) << 8) |
         (static_cast<std::uint32_t>(write[6]) << 16) |
         (static_cast<std::uint32_t>(write[7]) << 24);
}

TEST(ReplicaSessionTest, ZstdIsNotAskedForUnlessSourceYmlSaysSo) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  ASSERT_EQ(session.Run().outcome, SessionOutcome::Registered);
  ASSERT_FALSE(transport.writes.empty());
  EXPECT_EQ(RequestedCapabilities(transport.writes.front()) &
                CLIENT_ZSTD_COMPRESSION_ALGORITHM,
            0u);
  EXPECT_FALSE(session.compressed());
}

TEST(ReplicaSessionTest, ConfiguredZstdIsAskedForWithItsLevel) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11",
                                          CLIENT_ZSTD_COMPRESSION_ALGORITHM);
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.compression = CompressionAlgorithm::Zstd;
  source.zstdCompressionLevel = 7;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  EXPECT_EQ(session.Run().outcome, SessionOutcome::TransientFailure);

  ASSERT_GE(transport.writes.size(), 2u);
  const auto &response = transport.writes.front();
  EXPECT_NE(RequestedCapabilities(response) & CLIENT_ZSTD_COMPRESSION_ALGORITHM,
            0u);
  EXPECT_EQ(response.back(), 7);
  EXPECT_TRUE(session.compressed());

  // Under MIN_COMPRESS_LENGTH a frame stores the payload as is:
  // length-before-compression is 0.
  const auto &query = transport.writes[1];
  ASSERT_GT(query.size(), COMPRESSED_HEADER_SIZE);
  const std::size_t compressedLength =
      static_cast<std::size_t>(query[0]) |
      (static_cast<std::size_t>(query[1]) << 8) |
      (static_cast<std::size_t>(query[2]) << 16);
  EXPECT_EQ(compressedLength, query.size() - COMPRESSED_HEADER_SIZE);
  EXPECT_EQ(query[3], 0);
  EXPECT_EQ(query[4], 0);
  EXPECT_EQ(query[5], 0);
  EXPECT_EQ(query[6], 0);
  EXPECT_EQ(query[COMPRESSED_HEADER_SIZE + 4], 0x03);
}

TEST(ReplicaSessionTest, SourceThatDoesNotOfferZstdIsAPermanentFailure) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.compression = CompressionAlgorithm::Zstd;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  const SessionResult result = session.Run();
  EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(result.message.find("does not offer zstd compression"),
            std::string::npos)
      << result.message;
  // A response asking for a capability the source did not offer is never
  // written (the reference client refuses likewise).
  EXPECT_TRUE(transport.writes.empty());
}

TEST(ReplicaSessionTest, ConfiguredZlibIsAskedForWithoutALevel) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11", CLIENT_COMPRESS);
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.compression = CompressionAlgorithm::Zlib;
  // CLIENT_COMPRESS carries no level on the wire; the key belongs to zstd.
  source.zstdCompressionLevel = 7;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  EXPECT_EQ(session.Run().outcome, SessionOutcome::TransientFailure);

  ASSERT_GE(transport.writes.size(), 2u);
  const auto &response = transport.writes.front();
  EXPECT_NE(RequestedCapabilities(response) & CLIENT_COMPRESS, 0u);
  EXPECT_EQ(RequestedCapabilities(response) & CLIENT_ZSTD_COMPRESSION_ALGORITHM,
            0u);
  EXPECT_TRUE(session.compressed());

  const auto &query = transport.writes[1];
  ASSERT_GT(query.size(), COMPRESSED_HEADER_SIZE);
  const std::size_t compressedLength =
      static_cast<std::size_t>(query[0]) |
      (static_cast<std::size_t>(query[1]) << 8) |
      (static_cast<std::size_t>(query[2]) << 16);
  EXPECT_EQ(compressedLength, query.size() - COMPRESSED_HEADER_SIZE);
  EXPECT_EQ(query[3], 0);
  EXPECT_EQ(query[4], 0);
  EXPECT_EQ(query[5], 0);
  EXPECT_EQ(query[6], 0);
  EXPECT_EQ(query[COMPRESSED_HEADER_SIZE + 4], 0x03);
}

TEST(ReplicaSessionTest, SourceThatDoesNotOfferZlibIsAPermanentFailure) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.compression = CompressionAlgorithm::Zlib;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  const SessionResult result = session.Run();
  EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(result.message.find("does not offer zlib compression"),
            std::string::npos)
      << result.message;
  EXPECT_TRUE(transport.writes.empty());
}

TEST(ReplicaSessionTest, AZstdOnlySourceDoesNotSatisfyARequestForZlib) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11",
                                          CLIENT_ZSTD_COMPRESSION_ALGORITHM);
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.compression = CompressionAlgorithm::Zlib;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  const SessionResult result = session.Run();
  EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(result.message.find("does not offer zlib compression"),
            std::string::npos)
      << result.message;
  EXPECT_TRUE(transport.writes.empty());
}

TEST(ReplicaSessionTest, ConnectionAttributesReportRelayIdentity) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                         "test-relay-name", "0.0.1");
  const SessionResult result = session.Run();
  ASSERT_EQ(result.outcome, SessionOutcome::Registered);

  ASSERT_FALSE(transport.writes.empty());
  const auto &handshakeResponse = transport.writes.front();

  const auto expectAttribute = [&](const std::string &key,
                                   const std::string &value) {
    std::vector<std::uint8_t> expected;
    LengthEncodedString::Encode(key, expected);
    LengthEncodedString::Encode(value, expected);
    const auto found =
        std::search(handshakeResponse.begin(), handshakeResponse.end(),
                    expected.begin(), expected.end());
    EXPECT_NE(found, handshakeResponse.end())
        << "missing connection attribute " << key << "=" << value;
  };
  expectAttribute("_client_name", "test-relay-name");
  expectAttribute("program_name", "test-relay-name");
  expectAttribute("_client_version", "0.0.1");
}

TEST(ReplicaSessionTest, AuthenticationErrorIsTransient) {
  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME,
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(
      test::ScriptedSourcePayloads::Err(1045, "Access denied for user"));
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 1;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(result.message.find("Access denied"), std::string::npos)
      << result.message;
}

const std::vector<std::uint8_t> EXPECTED_FULL_AUTH_PLAINTEXT{'s', 'e', 'c', 'r',
                                                             'e', 't', 0};

TEST(ReplicaSessionTest,
     FullAuthenticationWithLocalKeySucceedsWithoutRequestingOne) {
  const auto keyPair =
      test::CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  const auto nonce = test::ScriptedSourcePayloads::Scramble();

  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME, nonce));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  script.AppendFullAuthenticationExchange(/*requestsPublicKey=*/false);
  script.AppendPreDumpQueries("999", "ON",
                              "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.sourcePublicKeyPem.assign(keyPair.publicKeyPem.begin(),
                                   keyPair.publicKeyPem.end());

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  ASSERT_EQ(result.outcome, SessionOutcome::Registered) << result.message;

  ASSERT_GE(transport.writes.size(), 2u);
  const auto &authResponse = transport.writes[1];
  const auto expectedCipherLength =
      static_cast<std::size_t>(EVP_PKEY_get_size(keyPair.privateKey.get()));
  ASSERT_EQ(authResponse.size(), PACKET_HEADER_SIZE + expectedCipherLength);
  const std::vector<std::uint8_t> ciphertext(
      authResponse.begin() + PACKET_HEADER_SIZE, authResponse.end());
  const auto plain =
      test::CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
          keyPair.privateKey.get(), ciphertext, nonce);
  EXPECT_EQ(plain, EXPECTED_FULL_AUTH_PLAINTEXT);
}

TEST(ReplicaSessionTest,
     FullAuthenticationRequestsPublicKeyWhenNoLocalKeyIsConfigured) {
  const auto keyPair =
      test::CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  const auto nonce = test::ScriptedSourcePayloads::Scramble();

  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME, nonce));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  script.AppendFullAuthenticationExchange(/*requestsPublicKey=*/true,
                                          keyPair.publicKeyPem);
  script.AppendPreDumpQueries("999", "ON",
                              "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.getSourcePublicKey = true;

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  ASSERT_EQ(result.outcome, SessionOutcome::Registered) << result.message;

  ASSERT_GE(transport.writes.size(), 3u);
  const auto &keyRequest = transport.writes[1];
  ASSERT_EQ(keyRequest.size(), PACKET_HEADER_SIZE + 1);
  EXPECT_EQ(keyRequest[PACKET_HEADER_SIZE], 0x02);

  const auto &authResponse = transport.writes[2];
  const std::vector<std::uint8_t> ciphertext(
      authResponse.begin() + PACKET_HEADER_SIZE, authResponse.end());
  const auto plain =
      test::CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
          keyPair.privateKey.get(), ciphertext, nonce);
  EXPECT_EQ(plain, EXPECTED_FULL_AUTH_PLAINTEXT);
}

TEST(
    ReplicaSessionTest,
    FullAuthenticationWithoutAnyKeySourceIsTransientWithSecureConnectionMessage) {
  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME,
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 1;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(result.message.find("Authentication requires secure connection."),
            std::string::npos)
      << result.message;
}

TEST(ReplicaSessionTest,
     FullAuthenticationPrefersLocalKeyEvenWhenBothSettingsAreConfigured) {
  const auto keyPair =
      test::CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  const auto nonce = test::ScriptedSourcePayloads::Scramble();

  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME, nonce));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  script.AppendFullAuthenticationExchange(/*requestsPublicKey=*/false);
  script.AppendPreDumpQueries("999", "ON",
                              "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.getSourcePublicKey = true;
  source.sourcePublicKeyPem.assign(keyPair.publicKeyPem.begin(),
                                   keyPair.publicKeyPem.end());

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  ASSERT_EQ(result.outcome, SessionOutcome::Registered) << result.message;

  ASSERT_GE(transport.writes.size(), 2u);
  const auto &authResponse = transport.writes[1];
  const auto expectedCipherLength =
      static_cast<std::size_t>(EVP_PKEY_get_size(keyPair.privateKey.get()));
  ASSERT_EQ(authResponse.size(), PACKET_HEADER_SIZE + expectedCipherLength);
  const std::vector<std::uint8_t> ciphertext(
      authResponse.begin() + PACKET_HEADER_SIZE, authResponse.end());
  const auto plain =
      test::CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
          keyPair.privateKey.get(), ciphertext, nonce);
  EXPECT_EQ(plain, EXPECTED_FULL_AUTH_PLAINTEXT);
}

TEST(ReplicaSessionTest,
     FullAuthenticationAfterAuthSwitchRequestUsesTheSwitchedNonce) {
  std::array<std::uint8_t, SCRAMBLE_LENGTH> switchNonce{};
  for (std::size_t i = 0; i < switchNonce.size(); ++i)
    switchNonce[i] = static_cast<std::uint8_t>(i + 100);

  const auto keyPair =
      test::CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();

  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", "mysql_native_password",
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthSwitchRequest(
      CACHING_SHA2_PASSWORD_PLUGIN_NAME, switchNonce));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  script.AppendFullAuthenticationExchange(/*requestsPublicKey=*/false);
  script.AppendPreDumpQueries("999", "ON",
                              "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.sourcePublicKeyPem.assign(keyPair.publicKeyPem.begin(),
                                   keyPair.publicKeyPem.end());

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  ASSERT_EQ(result.outcome, SessionOutcome::Registered) << result.message;

  ASSERT_GE(transport.writes.size(), 3u);
  const auto &authResponse = transport.writes[2];
  const std::vector<std::uint8_t> ciphertext(
      authResponse.begin() + PACKET_HEADER_SIZE, authResponse.end());
  const auto plain =
      test::CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
          keyPair.privateKey.get(), ciphertext, switchNonce);
  EXPECT_EQ(plain, EXPECTED_FULL_AUTH_PLAINTEXT);
}

TEST(ReplicaSessionTest,
     FullAuthenticationWithMalformedRequestedKeyIsTransient) {
  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME,
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  script.SkipClientPacket();
  const std::vector<std::uint8_t> garbage{'n', 'o', 't', ' ', 'a',
                                          ' ', 'k', 'e', 'y'};
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(
      std::span<const std::uint8_t>(garbage)));
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.getSourcePublicKey = true;

  ServerSettings server;
  server.serverId = 1;
  ReplicaSession session(transport, source, server, "uuid", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(result.message.find("public key"), std::string::npos)
      << result.message;
}

TEST(ReplicaSessionTest,
     FullAuthenticationWithEmptyPasswordIsTransientWithoutEncrypting) {
  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", CACHING_SHA2_PASSWORD_PLUGIN_NAME,
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x04));
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.password.clear();
  source.getSourcePublicKey = true;

  ServerSettings server;
  server.serverId = 1;
  ReplicaSession session(transport, source, server, "uuid", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(result.message.find("password"), std::string::npos)
      << result.message;
  EXPECT_EQ(transport.writes.size(), 1u);
}

TEST(ReplicaSessionTest, UnsupportedAuthPluginIsTransient) {
  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", "mysql_native_password",
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthSwitchRequest(
      "sha256_password", test::ScriptedSourcePayloads::Scramble()));
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 1;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(result.message.find("sha256_password"), std::string::npos)
      << result.message;
}

TEST(ReplicaSessionTest, AuthSwitchRequestToCachingSha2Succeeds) {
  std::array<std::uint8_t, SCRAMBLE_LENGTH> switchNonce{};
  for (std::size_t i = 0; i < switchNonce.size(); ++i)
    switchNonce[i] = static_cast<std::uint8_t>(i + 100);

  test::ScriptedSourceBuilder script;
  script.Push(test::ScriptedSourcePayloads::Greeting(
      "8.4.11", "mysql_native_password",
      test::ScriptedSourcePayloads::Scramble()));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthSwitchRequest(
      CACHING_SHA2_PASSWORD_PLUGIN_NAME, switchNonce));
  script.SkipClientPacket();
  script.Push(test::ScriptedSourcePayloads::AuthMoreData(0x03));
  script.Push(test::ScriptedSourcePayloads::Ok(false));
  script.AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_ID", "999");
  script.AppendCommandOk();
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@source_binlog_checksum", "CRC32");
  script.AppendSingleColumnRow("@@GLOBAL.GTID_MODE", "ON");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_UUID",
                               "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
}

TEST(ReplicaSessionTest, GtidModeOffIsPermanent) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999", "OFF");
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(result.message.find("GTID_MODE"), std::string::npos)
      << result.message;
}

TEST(ReplicaSessionTest, ServerIdCollisionIsPermanent) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("42");
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(result.message.find("server_id"), std::string::npos)
      << result.message;
}

TEST(ReplicaSessionTest, RegistrationErrorIsTransient) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandError(1236, "could not register replica");
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(result.message.find("registration"), std::string::npos)
      << result.message;
}

// An empty-string column is a single 0x00 byte, same as an OK header; it must
// not be taken for the DEPRECATE_EOF terminator (0xFE).
TEST(ReplicaSessionTest, EmptyStringRowValueIsNotMistakenForRowTerminator) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11");
  script.AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_ID", "999");
  script.AppendCommandOk();
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@source_binlog_checksum", std::string());
  script.AppendSingleColumnRow("@@GLOBAL.GTID_MODE", "ON");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_UUID",
                               "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
  EXPECT_EQ(result.identity.checksumAlgorithm, "");
}

TEST(ReplicaSessionTest, ClockQueryErrorLeavesSourceClockUnknown) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11");
  script.AppendSingleColumnError(1193, "Clock unavailable");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_ID", "999");
  script.AppendCommandOk();
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@source_binlog_checksum", "CRC32");
  script.AppendSingleColumnRow("@@GLOBAL.GTID_MODE", "ON");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_UUID",
                               "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
  EXPECT_EQ(result.identity.unixTimestamp, 0u);
  EXPECT_EQ(result.identity.unixTimestampReadAt,
            std::chrono::steady_clock::time_point{});
  EXPECT_FALSE(SourceClock::FromIdentity(result.identity).Known());
}

TEST(ReplicaSessionTest, ServerIdUnknownVariableSkipsEqualityCheck) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11");
  script.AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
  script.AppendSingleColumnError(1193, "Unknown system variable 'server_id'");
  script.AppendCommandOk();
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@source_binlog_checksum", "CRC32");
  script.AppendSingleColumnRow("@@GLOBAL.GTID_MODE", "ON");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_UUID",
                               "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
  EXPECT_EQ(result.identity.serverId, 0u);
}

TEST(ReplicaSessionTest, ChecksumUnknownVariableDefaultsToOff) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11");
  script.AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_ID", "999");
  script.AppendCommandOk();
  script.AppendCommandError(1193, "Unknown system variable 'binlog_checksum'");
  script.AppendSingleColumnRow("@@GLOBAL.GTID_MODE", "ON");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_UUID",
                               "11111111-1111-1111-1111-111111111111");
  script.AppendCommandOk();
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
  EXPECT_EQ(result.identity.checksumAlgorithm, "OFF");
}

TEST(ReplicaSessionTest, GtidModeUnknownVariableIsPermanent) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11");
  script.AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_ID", "999");
  script.AppendCommandOk();
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@source_binlog_checksum", "CRC32");
  script.AppendSingleColumnError(1193, "Unknown system variable 'gtid_mode'");
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server, "uuid", "name",
                         "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(result.message.find("GTID_MODE"), std::string::npos)
      << result.message;
}

TEST(ReplicaSessionTest, ServerUuidUnknownVariableSkipsCheck) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11");
  script.AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
  script.AppendSingleColumnRow("@@GLOBAL.SERVER_ID", "999");
  script.AppendCommandOk();
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@source_binlog_checksum", "CRC32");
  script.AppendSingleColumnRow("@@GLOBAL.GTID_MODE", "ON");
  script.AppendSingleColumnError(1193, "Unknown system variable 'server_uuid'");
  script.AppendCommandOk();
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");

  const SessionResult result = session.Run();

  EXPECT_EQ(result.outcome, SessionOutcome::Registered);
  EXPECT_EQ(result.identity.serverUuid, "");
}

TEST(ReplicaSessionTest,
     StartDumpSkipsTaggedGtidsAndSetsBothHeartbeatFlagsBelowVersion830) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid(
          "999", "ON", "11111111-1111-1111-1111-111111111111", "8.0.46");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");
  ASSERT_EQ(session.Run().outcome, SessionOutcome::Registered);

  const Uuid sourceUuid{{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                         0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}};
  GtidSet gtidSet;
  ASSERT_TRUE(gtidSet.AddInterval(GtidSource{sourceUuid, "primary"}, 1, 6));

  const auto dumpResult = session.StartDump(gtidSet);
  EXPECT_FALSE(dumpResult.has_value());

  ASSERT_FALSE(transport.writes.empty());
  const auto &sentPacket = transport.writes.back();
  ASSERT_GE(sentPacket.size(), 5u);
  const std::vector<std::uint8_t> sentPayload(sentPacket.begin() + 4,
                                              sentPacket.end());

  BinlogDumpGtidCommand expected;
  expected.flags =
      BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2 | BINLOG_DUMP_SKIP_TAGGED_GTIDS;
  expected.serverId = server.serverId;
  expected.gtidSetEncoded = gtidSet.Encode(/*skipTaggedGtids=*/true);
  EXPECT_EQ(sentPayload, ComBinlogDumpGtidCommand::Encode(expected));
  EXPECT_EQ(gtidSet.GetEncodedLength(/*skipTaggedGtids=*/true), 8u);
}

TEST(ReplicaSessionTest, StartDumpKeepsTaggedGtidsFromVersion830Onward) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid(
          "999", "ON", "11111111-1111-1111-1111-111111111111", "8.4.11");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");
  ASSERT_EQ(session.Run().outcome, SessionOutcome::Registered);

  const Uuid sourceUuid{{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                         0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}};
  GtidSet gtidSet;
  ASSERT_TRUE(gtidSet.AddInterval(GtidSource{sourceUuid, "primary"}, 1, 6));

  ASSERT_FALSE(session.StartDump(gtidSet).has_value());

  ASSERT_FALSE(transport.writes.empty());
  const auto &sentPacket = transport.writes.back();
  const std::vector<std::uint8_t> sentPayload(sentPacket.begin() + 4,
                                              sentPacket.end());

  BinlogDumpGtidCommand expected;
  expected.flags = BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2;
  expected.serverId = server.serverId;
  expected.gtidSetEncoded = gtidSet.Encode(/*skipTaggedGtids=*/false);
  EXPECT_EQ(sentPayload, ComBinlogDumpGtidCommand::Encode(expected));
  EXPECT_GT(gtidSet.GetEncodedLength(/*skipTaggedGtids=*/false), 8u);
}

TEST(ReplicaSessionTest, StartDumpKeepsTaggedGtidsAtExactlyVersion830) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid(
          "999", "ON", "11111111-1111-1111-1111-111111111111", "8.3.0");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name", "ver");
  ASSERT_EQ(session.Run().outcome, SessionOutcome::Registered);

  const Uuid sourceUuid{{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                         0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}};
  GtidSet gtidSet;
  ASSERT_TRUE(gtidSet.AddInterval(GtidSource{sourceUuid, "primary"}, 1, 6));

  ASSERT_FALSE(session.StartDump(gtidSet).has_value());

  ASSERT_FALSE(transport.writes.empty());
  const auto &sentPacket = transport.writes.back();
  const std::vector<std::uint8_t> sentPayload(sentPacket.begin() + 4,
                                              sentPacket.end());

  BinlogDumpGtidCommand expected;
  expected.flags = BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2;
  expected.serverId = server.serverId;
  expected.gtidSetEncoded = gtidSet.Encode(/*skipTaggedGtids=*/false);
  EXPECT_EQ(sentPayload, ComBinlogDumpGtidCommand::Encode(expected));
}

TEST(ReplicaSessionTest, TlsIsNotAskedForUnlessSourceYmlSaysSo) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, MakeSource(), server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  ASSERT_EQ(session.Run().outcome, SessionOutcome::Registered);
  EXPECT_EQ(RequestedCapabilities(transport.writes.front()) & CLIENT_SSL, 0u);
  EXPECT_FALSE(session.encrypted());
}

TEST(ReplicaSessionTest, PreferredStaysPlainWhenTheSourceOffersNoTls) {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.sslMode = SslMode::Preferred;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  ASSERT_EQ(session.Run().outcome, SessionOutcome::Registered);
  EXPECT_EQ(RequestedCapabilities(transport.writes.front()) & CLIENT_SSL, 0u);
  EXPECT_FALSE(session.encrypted());
}

TEST(ReplicaSessionTest, SourceThatDoesNotOfferTlsIsAPermanentFailure) {
  for (const SslMode mode :
       {SslMode::Required, SslMode::VerifyCa, SslMode::VerifyIdentity}) {
    test::ScriptedSourceBuilder script =
        test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
    script.AppendCommandOk();
    test::FakeTransport transport;
    transport.incoming = script.Bytes();

    SourceSettings source = MakeSource();
    source.sslMode = mode;
    ServerSettings server;
    server.serverId = 42;
    ReplicaSession session(transport, source, server,
                           "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                           "0.1.0-test");
    const SessionResult result = session.Run();
    EXPECT_EQ(result.outcome, SessionOutcome::PermanentFailure);
    EXPECT_NE(result.message.find("does not offer TLS"), std::string::npos)
        << result.message;
    EXPECT_TRUE(transport.writes.empty());
  }
}

TEST(ReplicaSessionTest, RequiredSendsTheSslRequestBeforeAnythingElse) {
  test::ScriptedSourceBuilder script;
  script.AppendGreetingAndFastAuthSuccess("8.4.11", CLIENT_SSL);
  test::FakeTransport transport;
  transport.incoming = script.Bytes();

  SourceSettings source = MakeSource();
  source.sslMode = SslMode::Required;
  ServerSettings server;
  server.serverId = 42;
  ReplicaSession session(transport, source, server,
                         "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                         "0.1.0-test");
  EXPECT_EQ(session.Run().outcome, SessionOutcome::TransientFailure);
  ASSERT_FALSE(transport.writes.empty());
  EXPECT_EQ(transport.writes.front().size(), 36u);
  EXPECT_NE(RequestedCapabilities(transport.writes.front()) & CLIENT_SSL, 0u);
}

}  // namespace
}  // namespace binlog_streamer
