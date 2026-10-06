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

#include "protocol/cLengthEncodedString.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(LengthEncodedStringTest, EncodesShortStringWithOneBytePrefix) {
  std::vector<std::uint8_t> out;
  LengthEncodedString::Encode("abc", out);
  EXPECT_EQ(out, (std::vector<std::uint8_t>{3, 'a', 'b', 'c'}));
}

TEST(LengthEncodedStringTest, EncodesEmptyStringAsJustThePrefix) {
  std::vector<std::uint8_t> out;
  LengthEncodedString::Encode("", out);
  EXPECT_EQ(out, (std::vector<std::uint8_t>{0}));
}

TEST(LengthEncodedStringTest, RoundTripsThroughEncodeAndDecode) {
  std::vector<std::uint8_t> out;
  LengthEncodedString::Encode("caching_sha2_password", out);
  std::string decoded;
  const std::size_t consumed = LengthEncodedString::Decode(out, decoded);
  EXPECT_EQ(consumed, out.size());
  EXPECT_EQ(decoded, "caching_sha2_password");
}

TEST(LengthEncodedStringTest,
     DecodeReturnsZeroWhenDataShorterThanDeclaredLength) {
  const std::vector<std::uint8_t> data{5, 'a', 'b'};
  std::string decoded;
  EXPECT_EQ(LengthEncodedString::Decode(data, decoded), 0u);
}

}  // namespace
}  // namespace binlog_streamer
