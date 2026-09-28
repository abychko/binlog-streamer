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

#include "cAddressRangeParser.hpp"
#include "cProtectedFileFixture.hpp"
#include "cReplicaConfigLoader.hpp"
#include "config/cDiskProtectedFileReader.hpp"
#include "config/hConfigDefaults.hpp"
#include "net/cTlsCertificateGenerator.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

using test::ProtectedFileFixture;

LoadResult<ReplicaSettings> LoadReplica(const ProtectedFileFixture &fixture,
                                        const std::string &content,
                                        int mode = 0640) {
  const auto path = fixture.WriteFile("replica.yml", content, mode);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  return ReplicaConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
      .Load(path.string());
}

const std::string TWO_CLIENTS =
    "listen_address: 0.0.0.0\n"
    "listen_port: 3307\n"
    "clients:\n"
    "  - user: replica01\n"
    "    password: Secr3tPass1\n"
    "    hosts:\n"
    "      - 10.0.1.15\n"
    "      - 10.0.1.16\n"
    "  - user: replica02\n"
    "    password: Secr3tPass2\n"
    "    hosts:\n"
    "      - 10.0.2.0/24\n"
    "      - 2001:db8::/64\n";

TEST(ReplicaConfigLoaderTest, ParsesFullFileWithTwoClients) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, TWO_CLIENTS);
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_EQ(result.value->listenPort, 3307);
  ASSERT_EQ(result.value->clients.size(), 2u);
  EXPECT_EQ(result.value->clients[0].user, "replica01");
  EXPECT_EQ(result.value->clients[0].password, "Secr3tPass1");
  EXPECT_EQ(result.value->clients[0].hosts.size(), 2u);
  EXPECT_EQ(result.value->clients[1].user, "replica02");
  EXPECT_EQ(result.value->clients[1].hosts.size(), 2u);
}

TEST(ReplicaConfigLoaderTest, AbsentFileGivesDefaultsAndNoClients) {
  ProtectedFileFixture fixture;
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ReplicaConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "replica.yml").string());
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_EQ(result.value->listenPort, DEFAULT_LISTEN_PORT);
  EXPECT_TRUE(result.value->clients.empty());
  IpAddress defaultAddress{};
  std::string error;
  AddressRangeParser::ParseAddress(DEFAULT_LISTEN_ADDRESS, defaultAddress,
                                   error);
  EXPECT_EQ(result.value->listenAddress, defaultAddress);
}

TEST(ReplicaConfigLoaderTest, EmptyFileGivesDefaultsAndNoClients) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, "");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.value->clients.empty());
}

// A comment-only document also yields zero documents from LoadAll; the Null
// normalization in cYamlMapReader.cpp::Document must apply here too.
TEST(ReplicaConfigLoaderTest, CommentOnlyFileGivesDefaultsAndNoClients) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, "# just a comment\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.value->clients.empty());
}

TEST(ReplicaConfigLoaderTest, EmptyClientsSequenceIsNotAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(
      fixture, "listen_address: 0.0.0.0\nlisten_port: 3307\nclients: []\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->clients.empty());
}

TEST(ReplicaConfigLoaderTest, ScalarClientsKeyWithoutValueIsNotAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(
      fixture, "listen_address: 0.0.0.0\nlisten_port: 3307\nclients:\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->clients.empty());
}

// listen_address/listen_port left empty (an unfilled template) mean "not
// set", same as the file being absent - unlike every other optional
// scalar leaf in this codebase, where present-but-empty is an error.
TEST(ReplicaConfigLoaderTest, EmptyListenPortDefaultsWhenListenAddressIsSet) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadReplica(fixture, "listen_address: 0.0.0.0\nlisten_port:\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_EQ(result.value->listenPort, DEFAULT_LISTEN_PORT);
}

TEST(ReplicaConfigLoaderTest, EmptyListenAddressDefaultsWhenListenPortIsSet) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadReplica(fixture, "listen_address:\nlisten_port: 3307\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  IpAddress defaultAddress{};
  std::string error;
  AddressRangeParser::ParseAddress(DEFAULT_LISTEN_ADDRESS, defaultAddress,
                                   error);
  EXPECT_EQ(result.value->listenAddress, defaultAddress);
}

// A non-scalar listen_port is a type error, not "not set":
// OptionalNonEmptyString only treats an absent, null or empty-scalar node as
// "not set" and raises "wrong type" for anything else, same as String() for
// every other leaf.
TEST(ReplicaConfigLoaderTest, NonScalarListenPortIsAWrongTypeError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, "listen_port: [3307]\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "listen_port");
  EXPECT_EQ(result.errors[0].message, "wrong type");
}

TEST(ReplicaConfigLoaderTest, InvalidPortIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, "listen_port: 0\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "listen_port");
}

TEST(ReplicaConfigLoaderTest, CompressionDefaultsToZstdAndIsNamedNotFlagged) {
  ProtectedFileFixture fixture;
  const auto absent = LoadReplica(fixture, "listen_port: 3307\n");
  ASSERT_TRUE(absent.value) << ::testing::PrintToString(absent.errors);
  EXPECT_EQ(absent.value->compression, CompressionAlgorithm::Zstd);

  const auto empty = LoadReplica(fixture, "compression:\n");
  ASSERT_TRUE(empty.value) << ::testing::PrintToString(empty.errors);
  EXPECT_EQ(empty.value->compression, CompressionAlgorithm::Zstd);

  const auto off = LoadReplica(fixture, "compression: uncompressed\n");
  ASSERT_TRUE(off.value) << ::testing::PrintToString(off.errors);
  EXPECT_EQ(off.value->compression, CompressionAlgorithm::None);

  const auto zlib = LoadReplica(fixture, "compression: zlib\n");
  ASSERT_TRUE(zlib.value);
  EXPECT_EQ(zlib.value->compression, CompressionAlgorithm::Zlib);

  const auto on = LoadReplica(fixture, "compression: zstd\n");
  ASSERT_TRUE(on.value) << ::testing::PrintToString(on.errors);
  EXPECT_EQ(on.value->compression, CompressionAlgorithm::Zstd);
}

TEST(ReplicaConfigLoaderTest, UnknownCompressionAlgorithmIsAnError) {
  ProtectedFileFixture fixture;
  // Not a boolean key: "true" names no algorithm.
  for (const std::string value : {"true", "lz4", "zstd,zlib"}) {
    const auto result = LoadReplica(fixture, "compression: " + value + "\n");
    EXPECT_FALSE(result.value) << value;
    ASSERT_EQ(result.errors.size(), 1u) << value;
    EXPECT_EQ(result.errors[0].keyPath, "compression");
    EXPECT_EQ(result.errors[0].message,
              "supported values: uncompressed zstd zlib");
  }
}

TEST(ReplicaConfigLoaderTest, UnknownTopLevelKeyIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, "bogus: 1\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "unknown key");
}

TEST(ReplicaConfigLoaderTest, DuplicateUserIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user: replica01\n"
                                  "    password: secret1\n"
                                  "    hosts: [10.0.1.15]\n"
                                  "  - user: replica01\n"
                                  "    password: secret2\n"
                                  "    hosts: [10.0.1.16]\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "clients[1].user");
  EXPECT_EQ(result.errors[0].message, "duplicate user");
}

// Unlike the top-level listen_address/listen_port above, an empty value
// inside a client entry stays an error: a client with an empty user is a
// real administrator mistake, not an unfilled template.
TEST(ReplicaConfigLoaderTest, EmptyClientUserIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user:\n"
                                  "    password: secret\n"
                                  "    hosts: [10.0.1.15]\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "clients[0].user");
  EXPECT_EQ(result.errors[0].message, "value is empty");
}

TEST(ReplicaConfigLoaderTest, EmptyClientPasswordIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user: replica01\n"
                                  "    password:\n"
                                  "    hosts: [10.0.1.15]\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "clients[0].password");
  EXPECT_EQ(result.errors[0].message, "value is empty");
}

TEST(ReplicaConfigLoaderTest, MissingHostsIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user: replica01\n"
                                  "    password: secret\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "clients[0].hosts");
}

TEST(ReplicaConfigLoaderTest, EmptyHostsSequenceIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user: replica01\n"
                                  "    password: secret\n"
                                  "    hosts: []\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "clients[0].hosts");
}

TEST(ReplicaConfigLoaderTest, UnknownKeyInClientSectionIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user: replica01\n"
                                  "    password: secret\n"
                                  "    hosts: [10.0.1.15]\n"
                                  "    bogus: 1\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "clients[0]");
  EXPECT_EQ(result.errors[0].message, "unknown key");
}

TEST(ReplicaConfigLoaderTest, DuplicateHostInSameClientIsAnError) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture,
                                  "clients:\n"
                                  "  - user: replica01\n"
                                  "    password: secret\n"
                                  "    hosts: [10.0.1.15, 10.0.1.15]\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "duplicate address range");
}

TEST(ReplicaConfigLoaderTest, WrongPermissionsFailWithoutParsing) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, "listen_port: 3307\n", 0644);
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
}

TEST(ReplicaConfigLoaderTest, SyntaxErrorsDoNotLeakClientPasswords) {
  ProtectedFileFixture fixture;
  for (const std::string content : {
           "clients:\n  - user: replica01\n    password: *Secr3tPass\n    "
           "hosts: [10.0.1.15]\n",
           "clients:\n  - user: replica01\n    password: \"ab\\qSecr3t\"\n    "
           "hosts: [10.0.1.15]\n",
           "clients:\n  - user: replica01\n    password: \"\\uD800\"\n    "
           "hosts: [10.0.1.15]\n",
       }) {
    const auto result = LoadReplica(fixture, content);
    EXPECT_FALSE(result.value) << content;
    ASSERT_EQ(result.errors.size(), 1u) << content;
    EXPECT_EQ(result.errors[0].message, "YAML syntax error") << content;
    EXPECT_EQ(result.errors[0].message.find("Secr3t"), std::string::npos)
        << content;
  }
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

TEST(ReplicaConfigLoaderTest, RequireSecureTransportDefaultsToOff) {
  ProtectedFileFixture fixture;
  const auto result = LoadReplica(fixture, TWO_CLIENTS);
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_FALSE(result.value->requireSecureTransport);
  EXPECT_TRUE(result.value->sslCert.empty());
  EXPECT_TRUE(result.value->tls.certPem.empty());
}

TEST(ReplicaConfigLoaderTest, RequireSecureTransportTakesABoolean) {
  ProtectedFileFixture fixture;
  auto result =
      LoadReplica(fixture, TWO_CLIENTS + "require_secure_transport: true\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->requireSecureTransport);

  result =
      LoadReplica(fixture, TWO_CLIENTS + "require_secure_transport: false\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_FALSE(result.value->requireSecureTransport);

  result =
      LoadReplica(fixture, TWO_CLIENTS + "require_secure_transport: maybe\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "require_secure_transport");
}

TEST(ReplicaConfigLoaderTest, SslCertAndSslKeyGoTogether) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadReplica(fixture, TWO_CLIENTS + "ssl_key: /etc/relay/key.pem\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_cert");
  EXPECT_EQ(result.errors[0].message, "ssl_cert and ssl_key go together");
}

TEST(ReplicaConfigLoaderTest, SslCaAloneIsRead) {
  ProtectedFileFixture fixture;
  const auto caPath =
      fixture.WriteFile("ca.pem", Certificates().caCertPem, 0644);
  const auto result =
      LoadReplica(fixture, TWO_CLIENTS + "ssl_ca: " + caPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->tls.caPem, Certificates().caCertPem);
  EXPECT_TRUE(result.value->tls.certPem.empty());
}

TEST(ReplicaConfigLoaderTest, SslCertAndKeyAreReadAndChecked) {
  ProtectedFileFixture fixture;
  const auto certPath =
      fixture.WriteFile("cert.pem", Certificates().serverCertPem, 0644);
  const auto keyPath =
      fixture.WriteFile("key.pem", Certificates().serverKeyPem, 0640);
  const auto result =
      LoadReplica(fixture, TWO_CLIENTS + "ssl_cert: " + certPath.string() +
                               "\nssl_key: " + keyPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->tls.certPem, Certificates().serverCertPem);
  EXPECT_EQ(result.value->tls.keyPem, Certificates().serverKeyPem);

  const auto wrongKeyPath =
      fixture.WriteFile("wrong-key.pem", Certificates().caKeyPem, 0640);
  const auto mismatch =
      LoadReplica(fixture, TWO_CLIENTS + "ssl_cert: " + certPath.string() +
                               "\nssl_key: " + wrongKeyPath.string() + "\n");
  EXPECT_FALSE(mismatch.value);
  ASSERT_EQ(mismatch.errors.size(), 1u);
  EXPECT_EQ(mismatch.errors[0].keyPath, "ssl_key");
  EXPECT_EQ(mismatch.errors[0].message, "ssl_key does not match ssl_cert");
}

}  // namespace
}  // namespace binlog_streamer
