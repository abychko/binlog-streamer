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

#include "cTagText.hpp"

#include <gtest/gtest.h>
#include <string>

namespace binlog_streamer {
namespace {

TEST(TagTextTest, AcceptsLowercaseTag) {
  std::string value;
  std::string error;
  ASSERT_TRUE(TagText::Parse("primary", value, error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(value, "primary");
}

TEST(TagTextTest, NormalizesUppercaseToLowercase) {
  std::string value;
  std::string error;
  ASSERT_TRUE(TagText::Parse("Primary_1", value, error));
  EXPECT_EQ(value, "primary_1");
}

TEST(TagTextTest, AcceptsLeadingUnderscore) {
  std::string value;
  std::string error;
  ASSERT_TRUE(TagText::Parse("_tag", value, error));
  EXPECT_EQ(value, "_tag");
}

TEST(TagTextTest, AcceptsMaximumLength) {
  const std::string maxTag(32, 'a');
  std::string value;
  std::string error;
  ASSERT_TRUE(TagText::Parse(maxTag, value, error));
  EXPECT_EQ(value.size(), 32u);
}

TEST(TagTextTest, RejectsTooLong) {
  const std::string tooLong(33, 'a');
  std::string value;
  std::string error;
  EXPECT_FALSE(TagText::Parse(tooLong, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(TagTextTest, RejectsEmpty) {
  std::string value;
  std::string error;
  EXPECT_FALSE(TagText::Parse("", value, error));
}

TEST(TagTextTest, RejectsLeadingDigit) {
  std::string value;
  std::string error;
  EXPECT_FALSE(TagText::Parse("1tag", value, error));
}

TEST(TagTextTest, RejectsInvalidCharacter) {
  std::string value;
  std::string error;
  EXPECT_FALSE(TagText::Parse("tag-name", value, error));
}

}  // namespace
}  // namespace binlog_streamer
