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

#include "cConfigLoader.hpp"
#include "cProtectedFileFixture.hpp"
#include "cReplicaConfigLoader.hpp"
#include "cSourceConfigLoader.hpp"
#include "config/cConfigurationLoader.hpp"
#include "config/cDiskProtectedFileReader.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

using test::ProtectedFileFixture;

const std::string SETTINGS =
    "server:\n"
    "  server_id: 1001\n"
    "storage:\n"
    "  data_dir: /var/lib/binlog-streamer\n"
    "  retention:\n"
    "    policy: age\n"
    "    period: 7d\n"
    "  disk:\n"
    "    max_size: 2T\n"
    "    purge_high_watermark: 1900G\n"
    "    purge_low_watermark: 1800G\n"
    "    min_free_space: 50G\n"
    "    recovery_reserve: 10G\n"
    "cache:\n"
    "  policy: time_window\n"
    "  window: 12h\n"
    "  max_size: 1T\n";

const std::string SOURCE =
    "host: 127.0.0.1\nuser: repl\npassword: Secr3tPass\n";

const std::string REPLICA =
    "listen_address: 0.0.0.0\nlisten_port: 3307\nclients:\n  - user: "
    "replica01\n    password: secret\n    hosts: [10.0.1.15]\n";

TEST(ConfigurationLoaderTest, FullSetLoadsWithoutErrors) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("settings.yml", SETTINGS);
  fixture.WriteFile("source.yml", SOURCE);
  fixture.WriteFile("replica.yml", REPLICA);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ConfigurationLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "settings.yml").string());
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_EQ(result.value->source.host, "127.0.0.1");
  ASSERT_EQ(result.value->replica.clients.size(), 1u);
}

TEST(ConfigurationLoaderTest, MissingReplicaFileGivesDefaultReceptionSettings) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("settings.yml", SETTINGS);
  fixture.WriteFile("source.yml", SOURCE);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ConfigurationLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "settings.yml").string());
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(result.value->replica.clients.empty());
}

TEST(ConfigurationLoaderTest, MissingSourceFileIsExactlyOneError) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("settings.yml", SETTINGS);
  fixture.WriteFile("replica.yml", REPLICA);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ConfigurationLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "settings.yml").string());
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("source.yml"), std::string::npos);
}

TEST(ConfigurationLoaderTest, ErrorsInSettingsAndReplicaArriveTogether) {
  ProtectedFileFixture fixture;
  std::string brokenSettings = SETTINGS;
  const auto line = brokenSettings.find("  server_id: 1001\n");
  ASSERT_NE(line, std::string::npos);
  brokenSettings.replace(line, std::string("  server_id: 1001\n").size(),
                         "  server_id:\n");
  fixture.WriteFile("settings.yml", brokenSettings);
  fixture.WriteFile("source.yml", SOURCE);
  fixture.WriteFile("replica.yml", "listen_port: 0\n");
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ConfigurationLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "settings.yml").string());
  EXPECT_FALSE(result.value);
  EXPECT_EQ(result.errors.size(), 2u);
}

TEST(ConfigurationLoaderTest, DirectoryViolationIsReportedOnce) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("settings.yml", SETTINGS);
  fixture.WriteFile("source.yml", SOURCE);
  fixture.WriteFile("replica.yml", REPLICA);
  ASSERT_EQ(chmod(fixture.Directory().c_str(), 0770),
            0);  // group-writable: both protected files see the violation
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ConfigurationLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "settings.yml").string());
  EXPECT_FALSE(result.value);
  // ConfigurationLoader deduplicates the identical directory error raised
  // separately for source.yml and replica.yml.
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].file, fixture.Directory().string());
}

// Proves expectedOwner reaches SourceConfigLoader's public-key-file check
// too, not just DiskProtectedFileReader: a wrong owner there would fail
// with "wrong owner" before RsaPublicKey::Parse, not the PEM message below.
TEST(ConfigurationLoaderTest,
     SourcePublicKeyPathOwnerCheckUsesConfigurationLoadersExpectedOwner) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("settings.yml", SETTINGS);
  const auto keyPath = fixture.WriteFile("key.pem", "not a key\n", 0644);
  fixture.WriteFile("source.yml", SOURCE + "source_public_key_path: " +
                                      keyPath.string() + "\n");
  fixture.WriteFile("replica.yml", REPLICA);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      ConfigurationLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((fixture.Directory() / "settings.yml").string());
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source_public_key_path");
  EXPECT_EQ(result.errors[0].message, "malformed RSA public key PEM");
}

TEST(ConfigurationLoaderTest, PackagedSettingsExampleLoadsWithoutErrors) {
  const auto result = ConfigLoader::Load(
      std::string(BINLOG_STREAMER_PACKAGING_DIR) + "/settings.yml");
  EXPECT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
}

// Keep the shipped cache.max_size at or above the validator's 2 MiB minimum
// so a typo in the template is caught here rather than during installation.
TEST(ConfigurationLoaderTest,
     PackagedSettingsCacheMaxSizeParsesToAtLeastTwoMebibytes) {
  const auto result = ConfigLoader::Load(
      std::string(BINLOG_STREAMER_PACKAGING_DIR) + "/settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_GE(result.value->cache.maxSize, std::uint64_t{2} << 20);  // 2 MiB
}

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

bool AnyErrorHasField(const std::vector<ConfigError> &errors,
                      const std::string &field) {
  return std::any_of(errors.begin(), errors.end(),
                     [&](const auto &error) { return error.file == field; });
}

TEST(ConfigurationLoaderTest,
     UnfilledSourceTemplateFailsOnlyOnEmptyScalarKeys) {
  ProtectedFileFixture fixture;
  const auto content =
      ReadFile(std::string(BINLOG_STREAMER_PACKAGING_DIR) + "/source.yml");
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto result =
      SourceConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load(fixture.WriteFile("source.yml", content).string());
  EXPECT_FALSE(result.value);
  std::vector<std::string> keyPaths;
  for (const auto &error : result.errors) keyPaths.push_back(error.keyPath);
  std::sort(keyPaths.begin(), keyPaths.end());
  EXPECT_EQ(keyPaths,
            (std::vector<std::string>{"host", "password", "port", "user"}));
  EXPECT_FALSE(AnyErrorHasField(result.errors, fixture.Directory().string()));
  for (const auto &error : result.errors) {
    EXPECT_EQ(error.message, "value is empty");
  }
}

// Unlike source.yml, an unfilled replica.yml template loads without errors:
// empty listen_address/listen_port mean "use the default", matching the
// file being absent entirely - both leave the relay serving no replica.
TEST(ConfigurationLoaderTest, UnfilledReplicaTemplateLoadsSameAsAbsentFile) {
  ProtectedFileFixture fixture;
  const auto content =
      ReadFile(std::string(BINLOG_STREAMER_PACKAGING_DIR) + "/replica.yml");
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
  const auto templateResult =
      ReplicaConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load(fixture.WriteFile("replica.yml", content).string());
  ASSERT_TRUE(templateResult.value)
      << ::testing::PrintToString(templateResult.errors);
  EXPECT_TRUE(templateResult.errors.empty());

  ProtectedFileFixture absentFixture;
  const auto absentResult =
      ReplicaConfigLoader(reader, ProtectedFileFixture::CurrentUserName())
          .Load((absentFixture.Directory() / "replica.yml").string());
  ASSERT_TRUE(absentResult.value)
      << ::testing::PrintToString(absentResult.errors);

  EXPECT_EQ(templateResult.value->listenAddress,
            absentResult.value->listenAddress);
  EXPECT_EQ(templateResult.value->listenPort, absentResult.value->listenPort);
  EXPECT_TRUE(templateResult.value->clients.empty());
  EXPECT_EQ(templateResult.value->clients.size(),
            absentResult.value->clients.size());
}

}  // namespace
}  // namespace binlog_streamer
