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

#include "cCommandLine.hpp"
#include "config/hConfigDefaults.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <vector>

namespace binlog_streamer {
namespace {

LoadResult<CommandLineOptions> Parse(std::vector<const char *> arguments) {
  arguments.insert(arguments.begin(), "binlog-streamer");
  return CommandLine::Parse(static_cast<int>(arguments.size()),
                            arguments.data());
}

TEST(CommandLineTest, ConfigWithSeparateValue) {
  const auto result = Parse({"--config", "/tmp/settings.yml"});
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->settingsPath, "/tmp/settings.yml");
}

TEST(CommandLineTest, ConfigWithEqualsValue) {
  const auto result = Parse({"--config=/tmp/settings.yml"});
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->settingsPath, "/tmp/settings.yml");
}

TEST(CommandLineTest, DefaultPathWithoutConfigOption) {
  // Must not touch the filesystem: the default is a compile-time constant.
  const auto result = Parse({});
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->settingsPath, DEFAULT_SETTINGS_PATH);
  EXPECT_FALSE(result.value->validateOnly);
  EXPECT_FALSE(result.value->showHelp);
  EXPECT_FALSE(result.value->showVersion);
}

TEST(CommandLineTest, ValidateConfigFlag) {
  const auto result = Parse({"--validate-config"});
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->validateOnly);
}

TEST(CommandLineTest, HelpAndVersionFlags) {
  const auto result = Parse({"--help", "--version"});
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_TRUE(result.value->showHelp);
  EXPECT_TRUE(result.value->showVersion);
}

TEST(CommandLineTest, UnknownOptionIsAnError) {
  const auto result = Parse({"--bogus"});
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("unknown option"), std::string::npos);
}

TEST(CommandLineTest, ConfigAsLastArgumentWithoutValueIsAnError) {
  const auto result = Parse({"--config"});
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("requires a value"),
            std::string::npos);
}

TEST(CommandLineTest, ConfigWithEmptyEqualsValueIsAnError) {
  const auto result = Parse({"--config="});
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("requires a value"),
            std::string::npos);
}

TEST(CommandLineTest, ConfigWithEmptySeparateValueIsAnError) {
  // The empty string is consumed as "no value" (not as a literal path) and
  // then re-parsed as its own (unexpected) argument, so this reports two
  // errors, unlike the single-error cases above.
  const auto result = Parse({"--config", ""});
  EXPECT_FALSE(result.value);
  const auto found = std::find_if(
      result.errors.begin(), result.errors.end(), [](const auto &error) {
        return error.message.find("requires a value") != std::string::npos;
      });
  EXPECT_NE(found, result.errors.end());
}

TEST(CommandLineTest, ConfigFollowedByAnotherOptionIsAnError) {
  // The next argument looking like an option (starts with "--") is taken as
  // evidence --config's value was omitted, rather than as a literal path.
  const auto result = Parse({"--config", "--validate-config"});
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("requires a value"),
            std::string::npos);
}

TEST(CommandLineTest, BareArgumentIsAnError) {
  const auto result = Parse({"bogus"});
  EXPECT_FALSE(result.value);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_NE(result.errors[0].message.find("unexpected argument"),
            std::string::npos);
}

TEST(CommandLineTest, RepeatedConfigOptionKeepsTheLastValue) {
  // Matches mysqld, where a repeated option takes its last value (checked
  // with Percona Server 8.4 and 8.0: --port=1111 --port=2222 gives 2222).
  const auto result =
      Parse({"--config", "/tmp/first.yml", "--config", "/tmp/second.yml"});
  ASSERT_TRUE(result.value) << ::testing::PrintToString(result.errors);
  EXPECT_EQ(result.value->settingsPath, "/tmp/second.yml");
}

}  // namespace
}  // namespace binlog_streamer
