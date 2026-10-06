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

#include "binlog/cBinlogFileName.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(BinlogFileNameTest, ParsesBasenameAndNumber) {
  std::string basename;
  std::uint64_t number = 0;
  std::string error;
  ASSERT_TRUE(BinlogFileName::Parse("binlog.000041", basename, number, error))
      << error;
  EXPECT_EQ(basename, "binlog");
  EXPECT_EQ(number, 41u);
}

TEST(BinlogFileNameTest, FormatsWithSixDigitZeroPadding) {
  EXPECT_EQ(BinlogFileName::Format("binlog", 41), "binlog.000041");
  EXPECT_EQ(BinlogFileName::Format("binlog", 0), "binlog.000000");
}

TEST(BinlogFileNameTest, FormatGrowsPastSixDigitsWithoutWrapping) {
  // A fixed-width-6 formatter would wrap binlog.999999 + 1 back to
  // binlog.000000.
  EXPECT_EQ(BinlogFileName::Format("binlog", 999999), "binlog.999999");
  EXPECT_EQ(BinlogFileName::Format("binlog", 1000000), "binlog.1000000");
}

TEST(BinlogFileNameTest, RoundTripsThroughFormatAndParse) {
  const std::string formatted = BinlogFileName::Format("binlog", 1000000);
  std::string basename;
  std::uint64_t number = 0;
  std::string error;
  ASSERT_TRUE(BinlogFileName::Parse(formatted, basename, number, error))
      << error;
  EXPECT_EQ(basename, "binlog");
  EXPECT_EQ(number, 1000000u);
}

TEST(BinlogFileNameTest, RejectsMissingDot) {
  std::string basename;
  std::uint64_t number = 0;
  std::string error;
  EXPECT_FALSE(BinlogFileName::Parse("binlog000041", basename, number, error));
  EXPECT_FALSE(error.empty());
}

TEST(BinlogFileNameTest, RejectsFewerThanSixDigits) {
  std::string basename;
  std::uint64_t number = 0;
  std::string error;
  EXPECT_FALSE(BinlogFileName::Parse("binlog.4", basename, number, error));
}

TEST(BinlogFileNameTest, RejectsNonNumericSuffix) {
  std::string basename;
  std::uint64_t number = 0;
  std::string error;
  EXPECT_FALSE(BinlogFileName::Parse("binlog.abcdef", basename, number, error));
}

}  // namespace
}  // namespace binlog_streamer
