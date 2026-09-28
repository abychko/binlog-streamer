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

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>

namespace binlog_streamer {
namespace {

const std::string FULL_SETTINGS =
    "server:\n"
    "  server_id: 1001\n"
    "  max_connections: 128\n"
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

bool HasError(const std::vector<ConfigError> &errors,
              const std::string &keyPath) {
  return std::any_of(errors.begin(), errors.end(), [&](const auto &error) {
    return error.keyPath == keyPath;
  });
}

TEST(ConfigLoaderTest, FullFileProducesExpectedStructures) {
  const auto result = ConfigLoader::Parse(FULL_SETTINGS, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_EQ(result.value->server.serverId, 1001u);
  EXPECT_EQ(result.value->server.maxConnections, 128u);
  EXPECT_EQ(result.value->storage.dataDir, "/var/lib/binlog-streamer");
  EXPECT_EQ(result.value->storage.retention.policy, RetentionPolicy::Age);
  EXPECT_EQ(result.value->storage.retention.period,
            std::chrono::seconds(7 * 86400));
  EXPECT_EQ(result.value->storage.disk.maxSize, std::uint64_t{2} << 40);
  EXPECT_EQ(result.value->storage.disk.purgeHighWatermark,
            std::uint64_t{1900} << 30);
  EXPECT_EQ(result.value->storage.disk.purgeLowWatermark,
            std::uint64_t{1800} << 30);
  EXPECT_EQ(result.value->storage.disk.minFreeSpace, std::uint64_t{50} << 30);
  EXPECT_EQ(result.value->storage.disk.recoveryReserve,
            std::uint64_t{10} << 30);
  EXPECT_EQ(result.value->cache.policy, CachePolicy::TimeWindow);
  EXPECT_EQ(result.value->cache.window, std::chrono::hours(12));
  EXPECT_EQ(result.value->cache.maxSize, std::uint64_t{1} << 40);
}

TEST(ConfigLoaderTest, MissingDataDirGetsDefault) {
  std::string text = FULL_SETTINGS;
  const auto line = text.find("  data_dir: /var/lib/binlog-streamer\n");
  ASSERT_NE(line, std::string::npos);
  text.erase(line,
             std::string("  data_dir: /var/lib/binlog-streamer\n").size());
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->storage.dataDir,
            "/var/lib/binlog-streamer");  // hConfigDefaults.hpp
}

TEST(ConfigLoaderTest, MaxConnectionsIsOptionalAndBounded) {
  std::string text = FULL_SETTINGS;
  const std::string line = "  max_connections: 128\n";
  const auto at = text.find(line);
  ASSERT_NE(at, std::string::npos);

  // Absent: the packaged default (hConfigDefaults.hpp).
  std::string without = text;
  without.erase(at, line.size());
  auto result = ConfigLoader::Parse(without, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->server.maxConnections, 64u);

  std::string other = text;
  other.replace(at, line.size(), "  max_connections: 8\n");
  result = ConfigLoader::Parse(other, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->server.maxConnections, 8u);

  // A relay that accepts no one is a configuration mistake, not a way to
  // turn the listener off; above the ceiling is one too.
  for (const std::string &value :
       {std::string("0"), std::string("100001"), std::string("")}) {
    std::string broken = text;
    broken.replace(at, line.size(), "  max_connections: " + value + "\n");
    const auto refused = ConfigLoader::Parse(broken, "settings.yml");
    EXPECT_FALSE(refused.value) << value;
    ASSERT_TRUE(HasError(refused.errors, "server.max_connections")) << value;
  }
}

TEST(ConfigLoaderTest, SendLingerIsOptionalAndBounded) {
  auto result = ConfigLoader::Parse(FULL_SETTINGS, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->server.sendLinger, std::chrono::microseconds(200));

  const std::string line = "  max_connections: 128\n";
  const auto at = FULL_SETTINGS.find(line) + line.size();
  std::string text = FULL_SETTINGS;
  text.insert(at, "  send_linger: 500us\n");
  result = ConfigLoader::Parse(text, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->server.sendLinger, std::chrono::microseconds(500));

  text = FULL_SETTINGS;
  text.insert(at, "  send_linger: 0\n");
  result = ConfigLoader::Parse(text, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->server.sendLinger.count(), 0);

  for (const std::string &value :
       {std::string("1001ms"), std::string("5"), std::string("")}) {
    std::string broken = FULL_SETTINGS;
    broken.insert(at, "  send_linger: " + value + "\n");
    const auto refused = ConfigLoader::Parse(broken, "settings.yml");
    EXPECT_FALSE(refused.value) << value;
    ASSERT_TRUE(HasError(refused.errors, "server.send_linger")) << value;
  }
}

TEST(ConfigLoaderTest, MonitoringHttpIsOptionalWithDefaults) {
  auto result = ConfigLoader::Parse(FULL_SETTINGS, "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  const HttpSettings &http = result.value->monitoring.http;
  EXPECT_EQ(http.listenAddress.family, AddressFamily::Ipv4);
  EXPECT_EQ(http.listenAddress.bytes, (std::array<std::uint8_t, 16>{}));
  EXPECT_EQ(http.listenPort, 8080u);
  EXPECT_EQ(http.htmlDir, "/etc/binlog-streamer/html");

  // Unfilled keys mean the defaults, as in replica.yml; so does a section
  // with nothing under it.
  for (const std::string &tail :
       {std::string("monitoring:\n"), std::string("monitoring:\n  http:\n"),
        std::string("monitoring:\n  http:\n    listen_address:\n"
                    "    listen_port:\n")}) {
    result = ConfigLoader::Parse(FULL_SETTINGS + tail, "settings.yml");
    ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
    EXPECT_EQ(result.value->monitoring.http.listenPort, 8080u) << tail;
  }

  result = ConfigLoader::Parse(FULL_SETTINGS +
                                   "monitoring:\n  http:\n"
                                   "    listen_address: 127.0.0.1\n"
                                   "    listen_port: 9090\n",
                               "settings.yml");
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->monitoring.http.listenAddress.bytes[0], 127u);
  EXPECT_EQ(result.value->monitoring.http.listenAddress.bytes[3], 1u);
  EXPECT_EQ(result.value->monitoring.http.listenPort, 9090u);

  for (const std::string &value :
       {std::string("0"), std::string("65536"), std::string("http")}) {
    const auto refused = ConfigLoader::Parse(
        FULL_SETTINGS + "monitoring:\n  http:\n    listen_port: " + value +
            "\n",
        "settings.yml");
    EXPECT_FALSE(refused.value) << value;
    EXPECT_TRUE(HasError(refused.errors, "monitoring.http.listen_port"))
        << value;
  }
  const auto badAddress = ConfigLoader::Parse(
      FULL_SETTINGS + "monitoring:\n  http:\n    listen_address: relay\n",
      "settings.yml");
  EXPECT_FALSE(badAddress.value);
  EXPECT_TRUE(HasError(badAddress.errors, "monitoring.http.listen_address"));
  const auto unknown = ConfigLoader::Parse(
      FULL_SETTINGS + "monitoring:\n  http:\n    tls: true\n", "settings.yml");
  EXPECT_FALSE(unknown.value);
  EXPECT_TRUE(HasError(unknown.errors, "monitoring.http"));
  const auto unknownWay = ConfigLoader::Parse(
      FULL_SETTINGS + "monitoring:\n  snmp:\n", "settings.yml");
  EXPECT_FALSE(unknownWay.value);
  EXPECT_TRUE(HasError(unknownWay.errors, "monitoring"));
}

TEST(ConfigLoaderTest, UnknownKeyReportsPositionAndPath) {
  const auto text = FULL_SETTINGS + "  bogus: 1\n";
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "unknown key");
  EXPECT_EQ(result.errors[0].keyPath, "cache");  // path of the parent map
  EXPECT_EQ(result.errors[0].line, 19);
  EXPECT_EQ(result.errors[0].column, 3);
}

TEST(ConfigLoaderTest, MissingRequiredKeyIsError) {
  // "policy: age" is left in place so storage.retention stays a non-null map
  // (removing the section's only key would make it Null, exercising the
  // "expected a mapping" path instead of "required key is missing").
  std::string text = FULL_SETTINGS;
  const auto line = text.find("    period: 7d\n");
  ASSERT_NE(line, std::string::npos);
  text.erase(line, std::string("    period: 7d\n").size());
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_TRUE(HasError(result.errors, "storage.retention.period"));
  const auto found = std::find_if(
      result.errors.begin(), result.errors.end(), [](const auto &error) {
        return error.keyPath == "storage.retention.period";
      });
  EXPECT_EQ(found->message, "required key is missing");
}

TEST(ConfigLoaderTest, EmptyServerIdIsError) {
  std::string text = FULL_SETTINGS;
  const auto line = text.find("  server_id: 1001\n");
  ASSERT_NE(line, std::string::npos);
  text.replace(line, std::string("  server_id: 1001\n").size(),
               "  server_id:\n");
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "server.server_id");
  EXPECT_EQ(result.errors[0].message, "value is empty");
}

TEST(ConfigLoaderTest, WrongTypeIsError) {
  std::string text = FULL_SETTINGS;
  const auto line = text.find("  server_id: 1001\n");
  ASSERT_NE(line, std::string::npos);
  text.replace(line, std::string("  server_id: 1001\n").size(),
               "  server_id: [1, 2]\n");
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "server.server_id");
  EXPECT_EQ(result.errors[0].message, "wrong type");
}

TEST(ConfigLoaderTest, UnsupportedPolicyListsSupportedValues) {
  std::string text = FULL_SETTINGS;
  const auto line = text.find("    policy: age\n");
  ASSERT_NE(line, std::string::npos);
  text.replace(line, std::string("    policy: age\n").size(),
               "    policy: size\n");
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "storage.retention.policy");
  EXPECT_NE(result.errors[0].message.find("age"), std::string::npos);
}

TEST(ConfigLoaderTest, MultipleErrorsReportedTogether) {
  std::string text = FULL_SETTINGS;
  const auto policyLine = text.find("    policy: age\n");
  ASSERT_NE(policyLine, std::string::npos);
  text.replace(policyLine, std::string("    policy: age\n").size(),
               "    policy: size\n");
  text += "  bogus: 1\n";
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  EXPECT_EQ(result.errors.size(), 2u);
}

TEST(ConfigLoaderTest,
     SyntaxErrorThroughBrokenIndentationHasPositionAndFixedMessage) {
  const auto result =
      ConfigLoader::Parse("server_id: 1\n  storage:\n", "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "YAML syntax error");
  EXPECT_GT(result.errors[0].line, 0);
}

TEST(ConfigLoaderTest,
     SyntaxErrorThroughUndefinedAliasHasPositionAndFixedMessage) {
  const auto result =
      ConfigLoader::Parse("server:\n  server_id: *Secr3t\n", "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "YAML syntax error");
  EXPECT_EQ(result.errors[0].message.find("Secr3t"), std::string::npos);
  EXPECT_GT(result.errors[0].line, 0);
}

TEST(ConfigLoaderTest, NonexistentFileIsError) {
  const auto result =
      ConfigLoader::Load("/nonexistent/binlog-streamer/settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].file, "/nonexistent/binlog-streamer/settings.yml");
}

TEST(ConfigLoaderTest, DuplicateKeyErrorAtSecondPosition) {
  const auto text = FULL_SETTINGS + "server:\n  server_id: 2\n";
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "duplicate key");
  EXPECT_EQ(result.errors[0].keyPath, "server");
  EXPECT_EQ(result.errors[0].line,
            19);  // the repeated "server:" key, not the first
}

TEST(ConfigLoaderTest, RedirectsSourceSectionWithoutLeakingNestedValues) {
  const auto text = FULL_SETTINGS + "source:\n  password: Secr3tPass\n";
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "source");
  EXPECT_EQ(result.errors[0].message, "belongs to source.yml");
  EXPECT_EQ(result.errors[0].message.find("Secr3t"), std::string::npos);
}

TEST(ConfigLoaderTest, RedirectsReplicaSection) {
  const auto text = FULL_SETTINGS + "replica:\n  listen_port: 3307\n";
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].keyPath, "replica");
  EXPECT_EQ(result.errors[0].message, "belongs to replica.yml");
}

TEST(ConfigLoaderTest, RedirectsListenAddressAndListenPortUnderServer) {
  std::string text = FULL_SETTINGS;
  const auto line = text.find("  server_id: 1001\n");
  ASSERT_NE(line, std::string::npos);
  text.insert(line + std::string("  server_id: 1001\n").size(),
              "  listen_address: 0.0.0.0\n  listen_port: 3307\n");
  const auto result = ConfigLoader::Parse(text, "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 2u);
  for (const auto &error : result.errors)
    EXPECT_EQ(error.message, "belongs to replica.yml");
}

// yaml-cpp's single-document YAML::Load silently drops everything after the
// first "---", turning a typo into a silent fallback to defaults.
// YAML::LoadAll rejects it instead.
TEST(ConfigLoaderTest, MultipleDocumentsIsExactlyOneError) {
  const auto result = ConfigLoader::Parse("a: 1\n---\nb: 2\n", "settings.yml");
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors[0].message, "expected a single YAML document");
  EXPECT_TRUE(result.errors[0].keyPath.empty());
  EXPECT_EQ(result.errors[0].line, 3);
  EXPECT_EQ(result.errors[0].column, 1);
}

}  // namespace
}  // namespace binlog_streamer
