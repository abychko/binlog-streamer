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

#include "cProtectedFileFixture.hpp"
#include "cSourceConfigLoader.hpp"
#include "config/cDiskProtectedFileReader.hpp"
#include "config/hConfigDefaults.hpp"
#include "net/cTlsCertificateGenerator.hpp"

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/stat.h>
#include <memory>

namespace binlog_streamer {
namespace {

using test::ProtectedFileFixture;

LoadResult<SourceSettings> LoadSource(const ProtectedFileFixture &fixture,
                                      const std::string &content,
                                      int mode = 0640) {
  const auto path = fixture.WriteFile("source.yml", content, mode);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  return SourceConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
      .Load(path.string());
}

const std::string &ValidPublicKeyPem() {
  static const std::string pem = [] {
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keygenCtx(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr),
        &EVP_PKEY_CTX_free);
    EVP_PKEY_keygen_init(keygenCtx.get());
    EVP_PKEY_CTX_set_rsa_keygen_bits(keygenCtx.get(), 2048);
    EVP_PKEY *raw = nullptr;
    EVP_PKEY_keygen(keygenCtx.get(), &raw);
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        raw, &EVP_PKEY_free);
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()),
                                                        &BIO_free);
    PEM_write_bio_PUBKEY(bio.get(), key.get());
    char *data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, static_cast<std::size_t>(length));
  }();
  return pem;
}

TEST(SourceConfigLoaderTest, ParsesFullFile) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(
      fixture,
      "host: 127.0.0.1\nport: 3307\nuser: repl\npassword: Secr3tPass\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_EQ(result.value->host, "127.0.0.1");
  EXPECT_EQ(result.value->port, 3307);
  EXPECT_EQ(result.value->user, "repl");
  EXPECT_EQ(result.value->password, "Secr3tPass");
}

TEST(SourceConfigLoaderTest, DefaultsPortWhenOmitted) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, "host: db.example\nuser: repl\npassword: secret\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->port, DEFAULT_SOURCE_PORT);
}

TEST(SourceConfigLoaderTest, MissingHostIsError) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, "user: repl\npassword: secret\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "host");
}

TEST(SourceConfigLoaderTest, MissingUserIsError) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, "host: db\npassword: secret\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "user");
}

TEST(SourceConfigLoaderTest, MissingPasswordIsError) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, "host: db\nuser: repl\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "password");
}

TEST(SourceConfigLoaderTest, EmptyPasswordIsError) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, "host: db\nuser: repl\npassword:\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "password");
  EXPECT_EQ(result.errors[0].message, "value is empty");
}

TEST(SourceConfigLoaderTest, PortOutOfRangeIsError) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, "host: db\nport: 0\nuser: repl\npassword: secret\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "port");
}

TEST(SourceConfigLoaderTest, UnknownKeyIsError) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, "host: db\nuser: repl\npassword: secret\nextra: 1\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "unknown key");
}

TEST(SourceConfigLoaderTest, SyntaxErrorsDoNotLeakThePassword) {
  ProtectedFileFixture fixture;
  for (const std::string content : {
           "host: db\nuser: repl\npassword: *Secr3tPass\n",
           "host: db\nuser: repl\npassword: \"ab\\qSecr3t\"\n",
           "host: db\nuser: repl\npassword: \"\\uD800\"\n",
           "host: db\nuser: repl\npassword: secret\n  bad: 1\n",
       }) {
    const auto result = LoadSource(fixture, content);
    EXPECT_FALSE(result.value) << content;
    ASSERT_EQ(result.errors.size(), 1u) << content;
    EXPECT_EQ(result.errors[0].message, "YAML syntax error") << content;
    EXPECT_EQ(result.errors[0].message.find("Secr3t"), std::string::npos)
        << content;
  }
}

TEST(SourceConfigLoaderTest, WrongPermissionsFailsWithoutParsing) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, "host: db\nuser: repl\npassword: secret\n", 0644);
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_TRUE(result.value == std::nullopt);
}

TEST(SourceConfigLoaderTest, AbsentFileIsAnError) {
  ProtectedFileFixture fixture;
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      SourceConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "source.yml").string());
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("source.yml"), std::string::npos);
}

TEST(SourceConfigLoaderTest, EmptyFileIsExpectedAMappingAtOneOne) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, "");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "expected a mapping");
  EXPECT_EQ(result.errors[0].line, 1);
  EXPECT_EQ(result.errors[0].column, 1);
}

const std::string BASE = "host: db\nuser: repl\npassword: secret\n";

TEST(SourceConfigLoaderTest, GetSourcePublicKeyDefaultsToFalseWhenAbsent) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE);
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_FALSE(result.value->getSourcePublicKey);
  EXPECT_TRUE(result.value->sourcePublicKeyPath.empty());
}

TEST(SourceConfigLoaderTest, GetSourcePublicKeyTrueIsParsed) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, BASE + "get_source_public_key: true\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->getSourcePublicKey);
}

TEST(SourceConfigLoaderTest, GetSourcePublicKeyFalseIsParsed) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, BASE + "get_source_public_key: false\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_FALSE(result.value->getSourcePublicKey);
}

TEST(SourceConfigLoaderTest, GetSourcePublicKeyEmptyIsError) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE + "get_source_public_key:\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "get_source_public_key");
  EXPECT_EQ(result.errors[0].message, "value is empty");
}

TEST(SourceConfigLoaderTest, GetSourcePublicKeyNonBoolIsErrorWithPosition) {
  ProtectedFileFixture fixture;
  const std::string content = BASE + "get_source_public_key: yes\n";
  const auto result = LoadSource(fixture, content);
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "get_source_public_key");
  EXPECT_EQ(result.errors[0].message, "expected true or false");
  EXPECT_EQ(result.errors[0].line, 4);
  EXPECT_EQ(result.errors[0].column, 1);
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPathEmptyIsNotSet) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE + "source_public_key_path:\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->sourcePublicKeyPath.empty());
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPathRelativeIsError) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, BASE + "source_public_key_path: relative/key.pem\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source_public_key_path");
  EXPECT_EQ(result.errors[0].message, "expected an absolute filesystem path");
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPathMissingFileIsError) {
  ProtectedFileFixture fixture;
  const auto missing = (fixture.Directory() / "no-such-key.pem").string();
  const auto result =
      LoadSource(fixture, BASE + "source_public_key_path: " + missing + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source_public_key_path");
  EXPECT_EQ(result.errors[0].message, "file not found");
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPathMalformedPemIsError) {
  ProtectedFileFixture fixture;
  const auto keyPath = fixture.WriteFile("key.pem", "not a key\n", 0644);
  const auto result = LoadSource(
      fixture, BASE + "source_public_key_path: " + keyPath.string() + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source_public_key_path");
  EXPECT_EQ(result.errors[0].message, "malformed RSA public key PEM");
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPathWrongPermissionsIsError) {
  ProtectedFileFixture fixture;
  const auto keyPath = fixture.WriteFile("key.pem", ValidPublicKeyPem(), 0664);
  const auto result = LoadSource(
      fixture, BASE + "source_public_key_path: " + keyPath.string() + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source_public_key_path");
  EXPECT_NE(result.errors[0].message.find("group has write access"),
            std::string::npos);
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPathValidKeyAt0644IsAccepted) {
  ProtectedFileFixture fixture;
  const auto keyPath = fixture.WriteFile("key.pem", ValidPublicKeyPem(), 0644);
  const auto result = LoadSource(
      fixture, BASE + "source_public_key_path: " + keyPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->sourcePublicKeyPath, keyPath.string());
}

TEST(SourceConfigLoaderTest,
     SourcePublicKeyPathPopulatesPemFieldWithFileBytes) {
  ProtectedFileFixture fixture;
  const auto keyPath = fixture.WriteFile("key.pem", ValidPublicKeyPem(), 0644);
  const auto result = LoadSource(
      fixture, BASE + "source_public_key_path: " + keyPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->sourcePublicKeyPem, ValidPublicKeyPem());
}

TEST(SourceConfigLoaderTest, CompressionIsOffUnlessAskedForByName) {
  ProtectedFileFixture fixture;
  const auto absent = LoadSource(fixture, BASE);
  ASSERT_TRUE(absent.value) << ::testing::PrintToString(absent.errors);
  EXPECT_EQ(absent.value->compression, CompressionAlgorithm::None);
  EXPECT_EQ(absent.value->zstdCompressionLevel, DEFAULT_ZSTD_COMPRESSION_LEVEL);

  const auto empty = LoadSource(fixture, BASE + "compression:\n");
  ASSERT_TRUE(empty.value) << ::testing::PrintToString(empty.errors);
  EXPECT_EQ(empty.value->compression, CompressionAlgorithm::None);

  const auto on = LoadSource(
      fixture, BASE + "compression: zstd\nzstd_compression_level: 22\n");
  ASSERT_TRUE(on.value) << ::testing::PrintToString(on.errors);
  EXPECT_EQ(on.value->compression, CompressionAlgorithm::Zstd);
  EXPECT_EQ(on.value->zstdCompressionLevel, 22);

  const auto zlib = LoadSource(fixture, BASE + "compression: zlib\n");
  ASSERT_TRUE(zlib.value);
  EXPECT_EQ(zlib.value->compression, CompressionAlgorithm::Zlib);

  const auto off = LoadSource(fixture, BASE + "compression: uncompressed\n");
  ASSERT_TRUE(off.value) << ::testing::PrintToString(off.errors);
  EXPECT_EQ(off.value->compression, CompressionAlgorithm::None);
}

TEST(SourceConfigLoaderTest, UnknownCompressionAlgorithmIsAnError) {
  ProtectedFileFixture fixture;
  for (const std::string value : {"true", "lz4", "zstd,zlib"}) {
    const auto result =
        LoadSource(fixture, BASE + "compression: " + value + "\n");
    EXPECT_FALSE(result.value) << value;
    ASSERT_EQ(result.errors.size(), 1u) << value;
    EXPECT_EQ(result.errors[0].keyPath, "compression");
    EXPECT_EQ(result.errors[0].message,
              "supported values: uncompressed zstd zlib");
  }
}

TEST(SourceConfigLoaderTest,
     ZstdCompressionLevelOutsideOneToTwentyTwoIsAnError) {
  ProtectedFileFixture fixture;
  for (const std::string value : {"0", "23"}) {
    const auto result =
        LoadSource(fixture, BASE + "zstd_compression_level: " + value + "\n");
    EXPECT_FALSE(result.value) << value;
    ASSERT_EQ(result.errors.size(), 1u) << value;
    EXPECT_EQ(result.errors[0].keyPath, "zstd_compression_level");
  }
}

TEST(SourceConfigLoaderTest, SourcePublicKeyPemIsEmptyWhenPathIsNotSet) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE);
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->sourcePublicKeyPem.empty());
}

TEST(SourceConfigLoaderTest, BothPublicKeySettingsCanBeSetTogether) {
  ProtectedFileFixture fixture;
  const auto keyPath = fixture.WriteFile("key.pem", ValidPublicKeyPem(), 0644);
  const auto result = LoadSource(
      fixture, BASE + "get_source_public_key: true\nsource_public_key_path: " +
                   keyPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->getSourcePublicKey);
  EXPECT_EQ(result.value->sourcePublicKeyPath, keyPath.string());
}

// Unlike rsa_init() in the server, a source_public_key_path that fails to parse
// is an error; the relay never falls back silently.
TEST(SourceConfigLoaderTest,
     BadKeyFileIsAnErrorEvenWhenGetSourcePublicKeyIsTrue) {
  ProtectedFileFixture fixture;
  const auto keyPath = fixture.WriteFile("key.pem", "not a key\n", 0644);
  const auto result = LoadSource(
      fixture, BASE + "get_source_public_key: true\nsource_public_key_path: " +
                   keyPath.string() + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source_public_key_path");
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

TEST(SourceConfigLoaderTest, SslModeDefaultsToDisabled) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE);
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->sslMode, SslMode::Disabled);
  EXPECT_TRUE(result.value->sslCa.empty());
  EXPECT_TRUE(result.value->tls.caPem.empty());
}

TEST(SourceConfigLoaderTest, SslModeTakesTheFiveNamesAClientTakes) {
  const std::pair<const char *, SslMode> names[] = {
      {"DISABLED", SslMode::Disabled},
      {"PREFERRED", SslMode::Preferred},
      {"REQUIRED", SslMode::Required},
      {"VERIFY_CA", SslMode::VerifyCa},
      {"VERIFY_IDENTITY", SslMode::VerifyIdentity},
  };
  for (const auto &[name, mode] : names) {
    ProtectedFileFixture fixture;
    const std::string ca =
        mode >= SslMode::VerifyCa
            ? "ssl_ca: " +
                  fixture.WriteFile("ca.pem", Certificates().caCertPem, 0644)
                      .string() +
                  "\n"
            : "";
    const auto result =
        LoadSource(fixture, BASE + "ssl_mode: " + name + "\n" + ca);
    ASSERT_TRUE(result.value)
        << name << ": " << ::testing::PrintToString(result.errors);
    EXPECT_EQ(result.value->sslMode, mode) << name;
  }
}

TEST(SourceConfigLoaderTest, SslModeRejectsAnUnknownName) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE + "ssl_mode: yes\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_mode");
  EXPECT_NE(result.errors[0].message.find("VERIFY_IDENTITY"), std::string::npos)
      << result.errors[0].message;
}

TEST(SourceConfigLoaderTest, VerifyingTheChainNeedsSslCa) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE + "ssl_mode: VERIFY_CA\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_mode");
  EXPECT_EQ(result.errors[0].message, "VERIFY_CA requires ssl_ca");
}

TEST(SourceConfigLoaderTest, SslCertAndSslKeyGoTogether) {
  ProtectedFileFixture fixture;
  const auto result =
      LoadSource(fixture, BASE + "ssl_cert: /etc/relay/client-cert.pem\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_key");
  EXPECT_EQ(result.errors[0].message, "ssl_cert and ssl_key go together");
}

TEST(SourceConfigLoaderTest, SslPathsHaveToBeAbsolute) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(fixture, BASE + "ssl_ca: ca.pem\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_ca");
  EXPECT_EQ(result.errors[0].message, "expected an absolute filesystem path");
}

TEST(SourceConfigLoaderTest, SslFilesAreNotReadUnderDisabled) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(
      fixture,
      BASE + "ssl_ca: " + (fixture.Directory() / "absent.pem").string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->sslCa, (fixture.Directory() / "absent.pem").string());
  EXPECT_TRUE(result.value->tls.caPem.empty());
}

TEST(SourceConfigLoaderTest, SslCaIsReadWhenTlsIsOn) {
  ProtectedFileFixture fixture;
  const auto caPath =
      fixture.WriteFile("ca.pem", Certificates().caCertPem, 0644);
  const auto result = LoadSource(
      fixture, BASE + "ssl_mode: REQUIRED\nssl_ca: " + caPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->tls.caPem, Certificates().caCertPem);
}

TEST(SourceConfigLoaderTest, AMissingSslCaIsReportedAtItsKey) {
  ProtectedFileFixture fixture;
  const auto result = LoadSource(
      fixture, BASE + "ssl_mode: REQUIRED\nssl_ca: " +
                   (fixture.Directory() / "absent.pem").string() + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_ca");
  EXPECT_EQ(result.errors[0].message, "file not found");
  EXPECT_EQ(result.errors[0].line, 5);
}

TEST(SourceConfigLoaderTest, SslCertAndKeyAreReadWhenTlsIsOn) {
  ProtectedFileFixture fixture;
  const auto certPath =
      fixture.WriteFile("cert.pem", Certificates().serverCertPem, 0644);
  const auto keyPath =
      fixture.WriteFile("key.pem", Certificates().serverKeyPem, 0640);
  const auto result = LoadSource(
      fixture, BASE + "ssl_mode: REQUIRED\nssl_cert: " + certPath.string() +
                   "\nssl_key: " + keyPath.string() + "\n");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->tls.certPem, Certificates().serverCertPem);
  EXPECT_EQ(result.value->tls.keyPem, Certificates().serverKeyPem);
}

TEST(SourceConfigLoaderTest, AWorldReadableSslKeyIsRefused) {
  ProtectedFileFixture fixture;
  const auto certPath =
      fixture.WriteFile("cert.pem", Certificates().serverCertPem, 0644);
  const auto keyPath =
      fixture.WriteFile("key.pem", Certificates().serverKeyPem, 0644);
  const auto result = LoadSource(
      fixture, BASE + "ssl_mode: REQUIRED\nssl_cert: " + certPath.string() +
                   "\nssl_key: " + keyPath.string() + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_key");
}

TEST(SourceConfigLoaderTest, AKeyThatDoesNotMatchTheCertificateIsRefused) {
  ProtectedFileFixture fixture;
  const auto certPath =
      fixture.WriteFile("cert.pem", Certificates().serverCertPem, 0644);
  const auto keyPath =
      fixture.WriteFile("key.pem", Certificates().caKeyPem, 0640);
  const auto result = LoadSource(
      fixture, BASE + "ssl_mode: REQUIRED\nssl_cert: " + certPath.string() +
                   "\nssl_key: " + keyPath.string() + "\n");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "ssl_key");
  EXPECT_EQ(result.errors[0].message, "ssl_key does not match ssl_cert");
}

}  // namespace
}  // namespace binlog_streamer
