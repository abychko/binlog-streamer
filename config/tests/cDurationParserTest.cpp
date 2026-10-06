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

#include "cDurationParser.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>
#include <utility>

namespace binlog_streamer {
namespace {

void ExpectParses(std::string_view text, std::chrono::seconds expected) {
  std::chrono::seconds value{};
  std::string error;
  ASSERT_TRUE(DurationParser::Parse(text, value, error)) << text;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(value, expected) << text;
}

void ExpectFails(std::string_view text) {
  std::chrono::seconds value{};
  std::string error;
  EXPECT_FALSE(DurationParser::Parse(text, value, error)) << text;
  EXPECT_FALSE(error.empty());
}

TEST(DurationParserTest, ParsesEachSuffix) {
  ExpectParses("7d", std::chrono::seconds(7 * 86400));
  ExpectParses("12h", std::chrono::seconds(12 * 3600));
  ExpectParses("30m", std::chrono::seconds(30 * 60));
  ExpectParses("45s", std::chrono::seconds(45));
}

TEST(DurationParserTest, RejectsMalformedInput) {
  ExpectFails("7D");
  ExpectFails("7");
  ExpectFails("0h");
  ExpectFails("1.5h");
  ExpectFails("9999999999999999d");
}

TEST(DurationParserTest, ParsesADelayInMicrosecondsOrMilliseconds) {
  std::chrono::microseconds value{};
  std::string error;
  for (const auto &[text, expected] :
       {std::pair<std::string_view, std::int64_t>{"0", 0},
        {"0us", 0},
        {"250us", 250},
        {"2ms", 2000}}) {
    ASSERT_TRUE(DurationParser::ParseDelay(text, value, error)) << text;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(value.count(), expected) << text;
  }
  for (const std::string_view text :
       {"", "5", "us", "ms", "1s", "1.5ms", "-1us", "2MS",
        "99999999999999999999us"}) {
    EXPECT_FALSE(DurationParser::ParseDelay(text, value, error)) << text;
    EXPECT_FALSE(error.empty()) << text;
  }
}

}  // namespace
}  // namespace binlog_streamer
