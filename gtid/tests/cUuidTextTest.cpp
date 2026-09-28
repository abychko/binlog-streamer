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

#include "cUuidText.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(UuidTextTest, ParsesAndPrintsLowercase) {
  Uuid value;
  std::string error;
  ASSERT_TRUE(
      UuidText::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562", value, error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(UuidText::ToString(value), "3e11fa47-71ca-11e1-9e33-c80aa9429562");
}

TEST(UuidTextTest, AcceptsUppercaseHexAndNormalizesToLowercase) {
  Uuid value;
  std::string error;
  ASSERT_TRUE(
      UuidText::Parse("3E11FA47-71CA-11E1-9E33-C80AA9429562", value, error));
  EXPECT_EQ(UuidText::ToString(value), "3e11fa47-71ca-11e1-9e33-c80aa9429562");
}

TEST(UuidTextTest, PreservesByteOrder) {
  // The first and last bytes differ (0x3e vs 0x62): a codec that reversed
  // or transposed sections would still print the same digits back if
  // fields were symmetrical, so this fixture is chosen to be asymmetric.
  Uuid value;
  std::string error;
  ASSERT_TRUE(
      UuidText::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562", value, error));
  EXPECT_EQ(value.bytes[0], 0x3e);
  EXPECT_EQ(value.bytes[15], 0x62);
}

TEST(UuidTextTest, RejectsWrongLength) {
  Uuid value;
  std::string error;
  EXPECT_FALSE(
      UuidText::Parse("3e11fa47-71ca-11e1-9e33-c80aa942956", value, error));
  EXPECT_FALSE(error.empty());
}

TEST(UuidTextTest, RejectsMissingDash) {
  Uuid value;
  std::string error;
  EXPECT_FALSE(
      UuidText::Parse("3e11fa4771ca-11e1-9e33-c80aa9429562", value, error));
}

TEST(UuidTextTest, RejectsNonHexCharacter) {
  Uuid value;
  std::string error;
  EXPECT_FALSE(
      UuidText::Parse("3e11fa47-71ca-11e1-9e33-c80aa942956g", value, error));
}

}  // namespace
}  // namespace binlog_streamer
