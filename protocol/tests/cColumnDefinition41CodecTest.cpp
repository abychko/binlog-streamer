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

#include "protocol/cColumnDefinition41Codec.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "protocol/cLengthEncodedString.hpp"

namespace binlog_streamer {
namespace {

std::vector<std::uint8_t> BuildFixture() {
  std::vector<std::uint8_t> payload;
  LengthEncodedString::Encode("def", payload);
  LengthEncodedString::Encode("", payload);
  LengthEncodedString::Encode("", payload);
  LengthEncodedString::Encode("", payload);
  LengthEncodedString::Encode("Variable_name", payload);
  LengthEncodedString::Encode("", payload);
  payload.push_back(0x0c);
  payload.push_back(0x21);
  payload.push_back(0x00);
  payload.push_back(0xFF);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0xFD);
  payload.push_back(0x00);
  payload.push_back(0x00);
  payload.push_back(0x00);
  return payload;
}

TEST(ColumnDefinition41CodecTest, ParsesFixture) {
  ColumnDefinition41 value;
  std::string error;
  ASSERT_TRUE(ColumnDefinition41Codec::Parse(BuildFixture(), value, error))
      << error;
  EXPECT_EQ(value.catalog, "def");
  EXPECT_EQ(value.name, "Variable_name");
  EXPECT_EQ(value.characterSet, 33);
  EXPECT_EQ(value.columnLength, 255u);
  EXPECT_EQ(value.type, 253);
  EXPECT_EQ(value.flags, 0u);
  EXPECT_EQ(value.decimals, 0u);
}

TEST(ColumnDefinition41CodecTest, RejectsTruncatedFixedFields) {
  std::vector<std::uint8_t> payload = BuildFixture();
  payload.resize(payload.size() - 3);
  ColumnDefinition41 value;
  std::string error;
  EXPECT_FALSE(ColumnDefinition41Codec::Parse(payload, value, error));
}

TEST(ColumnDefinition41CodecTest, EncodesExactBytes) {
  ColumnDefinition41 value;
  value.catalog = "def";
  value.name = "@@GLOBAL.SERVER_ID";
  value.characterSet = 0x003F;
  value.columnLength = 0x01020304;
  value.type = 0x08;
  value.flags = 0x00A0;
  value.decimals = 0x1F;

  std::vector<std::uint8_t> out = {0xEE};
  ColumnDefinition41Codec::Encode(value, out);

  std::vector<std::uint8_t> expected = {0xEE, 3, 'd', 'e', 'f', 0, 0, 0, 18};
  const std::string name = "@@GLOBAL.SERVER_ID";
  // Reserved up front: GCC 14 at -O2 misreads the reallocating insert below as
  // an overflow (-Wstringop-overflow).
  expected.reserve(expected.size() + name.size() + 14);
  expected.insert(expected.end(), name.begin(), name.end());
  expected.insert(expected.end(), {0, 0x0C, 0x3F, 0x00, 0x04, 0x03, 0x02, 0x01,
                                   0x08, 0xA0, 0x00, 0x1F, 0x00, 0x00});
  EXPECT_EQ(out, expected);
}

TEST(ColumnDefinition41CodecTest, ParseReadsBackWhatEncodeWrote) {
  ColumnDefinition41 original;
  original.catalog = "def";
  original.schema = "s";
  original.table = "t";
  original.orgTable = "ot";
  original.name = "n";
  original.orgName = "on";
  original.characterSet = 255;
  original.columnLength = 1024;
  original.type = 253;
  original.flags = 1;
  original.decimals = 31;
  std::vector<std::uint8_t> wire;
  ColumnDefinition41Codec::Encode(original, wire);

  ColumnDefinition41 parsed;
  std::string error;
  ASSERT_TRUE(ColumnDefinition41Codec::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.catalog, original.catalog);
  EXPECT_EQ(parsed.schema, original.schema);
  EXPECT_EQ(parsed.table, original.table);
  EXPECT_EQ(parsed.orgTable, original.orgTable);
  EXPECT_EQ(parsed.name, original.name);
  EXPECT_EQ(parsed.orgName, original.orgName);
  EXPECT_EQ(parsed.characterSet, original.characterSet);
  EXPECT_EQ(parsed.columnLength, original.columnLength);
  EXPECT_EQ(parsed.type, original.type);
  EXPECT_EQ(parsed.flags, original.flags);
  EXPECT_EQ(parsed.decimals, original.decimals);
}

}  // namespace
}  // namespace binlog_streamer
