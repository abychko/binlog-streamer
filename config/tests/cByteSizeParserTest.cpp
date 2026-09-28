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

#include "cByteSizeParser.hpp"

#include <gtest/gtest.h>
#include <cstdint>

namespace binlog_streamer {
namespace {

void ExpectParses(std::string_view text, std::uint64_t expected) {
  std::uint64_t value = 0;
  std::string error;
  ASSERT_TRUE(ByteSizeParser::Parse(text, value, error)) << text;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(value, expected) << text;
}

void ExpectFails(std::string_view text) {
  std::uint64_t value = 0;
  std::string error;
  EXPECT_FALSE(ByteSizeParser::Parse(text, value, error)) << text;
  EXPECT_FALSE(error.empty());
}

TEST(ByteSizeParserTest, ParsesEachSuffix) {
  ExpectParses("1K", std::uint64_t{1} << 10);
  ExpectParses("1k", std::uint64_t{1} << 10);
  ExpectParses("500G", std::uint64_t{500} << 30);
  ExpectParses("1T", std::uint64_t{1} << 40);
  ExpectParses("1E", std::uint64_t{1} << 60);
}

TEST(ByteSizeParserTest, LargestRepresentableExabytesParse) {
  ExpectParses("15E", std::uint64_t{15} << 60);
}

TEST(ByteSizeParserTest, OverflowIsRejected) {
  ExpectFails("16E");  // 16 * 2^60 == 2^64, one past the representable maximum
  ExpectFails("17E");
}

TEST(ByteSizeParserTest, RejectsMalformedInput) {
  ExpectFails("1024");  // no suffix
  ExpectFails("1.5G");  // fractional
  ExpectFails("-1G");   // negative
  ExpectFails("");      // empty
  ExpectFails("1 G");   // embedded space
  ExpectFails("1KB");   // extra letter
}

}  // namespace
}  // namespace binlog_streamer
