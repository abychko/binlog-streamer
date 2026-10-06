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

#include "protocol/cTextRowCodec.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "protocol/cLengthEncodedString.hpp"

namespace binlog_streamer {
namespace {

TEST(TextRowCodecTest, ParsesTwoColumnsWithoutNull) {
  std::vector<std::uint8_t> payload;
  LengthEncodedString::Encode("gtid_mode", payload);
  LengthEncodedString::Encode("ON", payload);

  TextRow value;
  std::string error;
  ASSERT_TRUE(TextRowCodec::Parse(payload, 2, value, error)) << error;
  ASSERT_EQ(value.columns.size(), 2u);
  ASSERT_TRUE(value.columns[0].has_value());
  EXPECT_EQ(*value.columns[0], "gtid_mode");
  ASSERT_TRUE(value.columns[1].has_value());
  EXPECT_EQ(*value.columns[1], "ON");
}

TEST(TextRowCodecTest, ParsesNullColumn) {
  std::vector<std::uint8_t> payload;
  LengthEncodedString::Encode("name", payload);
  payload.push_back(0xFB);

  TextRow value;
  std::string error;
  ASSERT_TRUE(TextRowCodec::Parse(payload, 2, value, error)) << error;
  EXPECT_TRUE(value.columns[0].has_value());
  EXPECT_FALSE(value.columns[1].has_value());
}

TEST(TextRowCodecTest, RejectsColumnValueShorterThanDeclared) {
  const std::vector<std::uint8_t> payload{5, 'a', 'b'};
  TextRow value;
  std::string error;
  EXPECT_FALSE(TextRowCodec::Parse(payload, 1, value, error));
}

TEST(TextRowCodecTest, EncodesExactBytesWithNullAndEmptyValue) {
  TextRow value;
  value.columns = {std::string("ab"), std::nullopt, std::string()};
  std::vector<std::uint8_t> out = {0xEE};
  TextRowCodec::Encode(value, out);
  ASSERT_EQ(out, (std::vector<std::uint8_t>{0xEE, 2, 'a', 'b', 0xFB, 0}));

  TextRow parsed;
  std::string error;
  ASSERT_TRUE(TextRowCodec::Parse(
      std::span<const std::uint8_t>(out.data() + 1, out.size() - 1), 3, parsed,
      error))
      << error;
  EXPECT_EQ(parsed.columns, value.columns);
}

TEST(TextRowCodecTest, EncodesLongValueWithMultiByteLength) {
  TextRow value;
  value.columns = {std::string(300, 'x')};
  std::vector<std::uint8_t> out;
  TextRowCodec::Encode(value, out);
  ASSERT_EQ(out.size(), 3u + 300u);
  EXPECT_EQ(out[0], 0xFC);
  EXPECT_EQ(out[1], 0x2C);
  EXPECT_EQ(out[2], 0x01);
}

}  // namespace
}  // namespace binlog_streamer
