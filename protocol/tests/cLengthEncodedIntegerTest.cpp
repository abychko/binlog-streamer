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

#include "protocol/cLengthEncodedInteger.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

void ExpectEncodesAs(std::uint64_t value, std::vector<std::uint8_t> expected) {
  std::vector<std::uint8_t> out;
  LengthEncodedInteger::Encode(value, out);
  EXPECT_EQ(out, expected) << value;

  std::uint64_t decoded = 0;
  bool isNull = false;
  const std::size_t consumed =
      LengthEncodedInteger::Decode(out, decoded, isNull);
  EXPECT_EQ(consumed, expected.size()) << value;
  EXPECT_FALSE(isNull);
  EXPECT_EQ(decoded, value) << value;
}

TEST(LengthEncodedIntegerTest, OneByteFormBelow251) {
  ExpectEncodesAs(0, {0x00});
  ExpectEncodesAs(250, {250});
}

TEST(LengthEncodedIntegerTest, TwoByteFormAtLowerBoundary) {
  ExpectEncodesAs(251, {0xFC, 251, 0});
  ExpectEncodesAs(65535, {0xFC, 0xFF, 0xFF});
}

TEST(LengthEncodedIntegerTest, ThreeByteFormAtLowerBoundary) {
  ExpectEncodesAs(65536, {0xFD, 0x00, 0x00, 0x01});
  ExpectEncodesAs(16777215, {0xFD, 0xFF, 0xFF, 0xFF});
}

TEST(LengthEncodedIntegerTest, EightByteFormAtLowerBoundary) {
  ExpectEncodesAs(16777216,
                  {0xFE, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00});
}

TEST(LengthEncodedIntegerTest, DecodeReportsNullMarker) {
  const std::vector<std::uint8_t> data{0xFB};
  std::uint64_t value = 123;
  bool isNull = false;
  const std::size_t consumed =
      LengthEncodedInteger::Decode(data, value, isNull);
  EXPECT_EQ(consumed, 1u);
  EXPECT_TRUE(isNull);
}

TEST(LengthEncodedIntegerTest, DecodeReturnsZeroWhenTruncated) {
  const std::vector<std::uint8_t> data{0xFD, 0x01};
  std::uint64_t value = 0;
  bool isNull = false;
  EXPECT_EQ(LengthEncodedInteger::Decode(data, value, isNull), 0u);
}

}  // namespace
}  // namespace binlog_streamer
