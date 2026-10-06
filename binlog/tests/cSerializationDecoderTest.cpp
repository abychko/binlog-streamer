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

#include "binlog/cSerializationDecoder.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

// Examples from libs/mysql/serialization/readme.md, which shows bytes
// big-endian: 65535 is FB FF 07 on the wire.
TEST(SerializationDecoderTest, DecodesTheReadmesUnsignedExample) {
  const std::vector<std::uint8_t> bytes{0xFB, 0xFF, 0x07};
  SerializationDecoder decoder(bytes);
  std::uint64_t value = 0;
  ASSERT_TRUE(decoder.ReadUnsigned(value));
  EXPECT_EQ(value, 65535u);
  EXPECT_TRUE(decoder.AtEnd());
}

TEST(SerializationDecoderTest, DecodesTheReadmesSignedExamples) {
  struct Case {
    std::vector<std::uint8_t> bytes;
    std::int64_t expected;
  };
  const Case cases[] = {
      {{0xF3, 0xFF, 0x0F}, 65535},
      {{0xEB, 0xFF, 0x0F}, -65535},
      {{0xFB, 0xFF, 0x0F}, -65536},
  };
  for (const auto &c : cases) {
    SerializationDecoder decoder(c.bytes);
    std::int64_t value = 0;
    ASSERT_TRUE(decoder.ReadSigned(value));
    EXPECT_EQ(value, c.expected);
    EXPECT_TRUE(decoder.AtEnd());
  }
}

// write_varlen_bytes: first byte = (2^(n-1) - 1) | (value << n), the rest =
// value >> (8 - n), little-endian.
TEST(SerializationDecoderTest, DecodesOneAndTwoByteUnsignedForms) {
  struct Case {
    std::vector<std::uint8_t> bytes;
    std::uint64_t expected;
  };
  const Case cases[] = {
      {{0x00}, 0},         {{0x02}, 1},         {{0xFE}, 127},
      {{0x01, 0x02}, 128}, {{0xFD, 0x03}, 255},
  };
  for (const auto &c : cases) {
    SerializationDecoder decoder(c.bytes);
    std::uint64_t value = 0;
    ASSERT_TRUE(decoder.ReadUnsigned(value));
    EXPECT_EQ(value, c.expected);
  }
}

TEST(SerializationDecoderTest, DecodesTheNineByteForm) {
  const std::vector<std::uint8_t> bytes{0xFF, 0x01, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x80};
  SerializationDecoder decoder(bytes);
  std::uint64_t value = 0;
  ASSERT_TRUE(decoder.ReadUnsigned(value));
  EXPECT_EQ(value, 0x8000000000000001ULL);
  EXPECT_TRUE(decoder.AtEnd());
}

TEST(SerializationDecoderTest, RefusesATruncatedIntegerAndKeepsItsPosition) {
  const std::vector<std::uint8_t> bytes{
      0xFB, 0xFF};  // announces three bytes, has two
  SerializationDecoder decoder(bytes);
  std::uint64_t value = 0;
  EXPECT_FALSE(decoder.ReadUnsigned(value));
  EXPECT_EQ(decoder.Position(), 0u);

  SerializationDecoder empty(std::span<const std::uint8_t>{});
  EXPECT_FALSE(empty.ReadUnsigned(value));
}

TEST(SerializationDecoderTest, ReadsALengthPrefixedStringWithinItsBound) {
  const std::vector<std::uint8_t> bytes{0x06 /*length 3*/, 'a', 'b', 'c', 0x02};
  SerializationDecoder decoder(bytes);
  std::string value;
  ASSERT_TRUE(decoder.ReadString(32, value));
  EXPECT_EQ(value, "abc");
  std::uint64_t next = 0;
  ASSERT_TRUE(decoder.ReadUnsigned(next));
  EXPECT_EQ(next, 1u);
}

TEST(SerializationDecoderTest, RefusesAStringLongerThanItsBoundOrItsBytes) {
  const std::vector<std::uint8_t> tooLong{0x06, 'a', 'b', 'c'};
  SerializationDecoder bounded(tooLong);
  std::string value;
  EXPECT_FALSE(bounded.ReadString(2, value));
  EXPECT_EQ(bounded.Position(), 0u);

  const std::vector<std::uint8_t> cutShort{0x06, 'a', 'b'};
  SerializationDecoder truncated(cutShort);
  EXPECT_FALSE(truncated.ReadString(32, value));
  EXPECT_EQ(truncated.Position(), 0u);

  const std::vector<std::uint8_t> emptyString{0x00};
  SerializationDecoder empty(emptyString);
  ASSERT_TRUE(empty.ReadString(32, value));
  EXPECT_TRUE(value.empty());
}

// Each array element is its own varint: {0x7F, 0x80} is FE 01 02.
TEST(SerializationDecoderTest, ReadsAByteArrayOfPerElementVarints) {
  const std::vector<std::uint8_t> bytes{0xFE, 0x01, 0x02};
  SerializationDecoder decoder(bytes);
  std::uint8_t out[2] = {0, 0};
  ASSERT_TRUE(decoder.ReadByteArray(out));
  EXPECT_EQ(out[0], 0x7F);
  EXPECT_EQ(out[1], 0x80);
  EXPECT_TRUE(decoder.AtEnd());
}

TEST(SerializationDecoderTest, RefusesAByteArrayElementAboveOneByteOrCutShort) {
  const std::vector<std::uint8_t> tooBig{0x01, 0x04};  // varint 256
  SerializationDecoder big(tooBig);
  std::uint8_t out[1] = {0};
  EXPECT_FALSE(big.ReadByteArray(out));
  EXPECT_EQ(big.Position(), 0u);

  const std::vector<std::uint8_t> short1{0x02};
  SerializationDecoder cut(short1);
  std::uint8_t two[2] = {0, 0};
  EXPECT_FALSE(cut.ReadByteArray(two));
  EXPECT_EQ(cut.Position(), 0u);
}

TEST(SerializationDecoderTest, ReadsTheMessageHeaderOnlyForFormatVersionOne) {
  const std::vector<std::uint8_t> bytes{0x02 /*version 1*/, 0x4C /*size 38*/,
                                        0x12 /*last non-ignorable 9*/, 0x00};
  SerializationDecoder decoder(bytes);
  std::uint64_t size = 0;
  std::uint64_t last = 0;
  ASSERT_TRUE(decoder.ReadMessageHeader(size, last));
  EXPECT_EQ(size, 38u);
  EXPECT_EQ(last, 9u);
  EXPECT_EQ(decoder.Position(), 3u);

  const std::vector<std::uint8_t> otherVersion{0x04, 0x4C, 0x12};
  SerializationDecoder rejected(otherVersion);
  EXPECT_FALSE(rejected.ReadMessageHeader(size, last));
  EXPECT_EQ(rejected.Position(), 0u);
}

TEST(SerializationDecoderTest, PeeksAFieldIdWithoutConsumingIt) {
  const std::vector<std::uint8_t> bytes{0x10 /*id 8*/, 0x02};
  SerializationDecoder decoder(bytes);
  std::uint64_t id = 0;
  ASSERT_TRUE(decoder.PeekFieldId(id));
  EXPECT_EQ(id, 8u);
  EXPECT_EQ(decoder.Position(), 0u);
  ASSERT_TRUE(decoder.SkipFieldId());
  EXPECT_EQ(decoder.Position(), 1u);
  ASSERT_TRUE(decoder.PeekFieldId(id));
  EXPECT_EQ(id, 1u);
  ASSERT_TRUE(decoder.SkipFieldId());
  EXPECT_FALSE(decoder.PeekFieldId(id));
}

}  // namespace
}  // namespace binlog_streamer
