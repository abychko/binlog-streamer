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

#include "protocol/cEofPacketCodec.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(EofPacketCodecTest, ParsesFixture) {
  const std::vector<std::uint8_t> payload{0xFE, 0x02, 0x00, 0x22, 0x00};
  EXPECT_TRUE(EofPacketCodec::IsEofPacket(payload));
  EofPacket value;
  std::string error;
  ASSERT_TRUE(EofPacketCodec::Parse(payload, value, error)) << error;
  EXPECT_EQ(value.warnings, 2u);
  EXPECT_EQ(value.statusFlags, 0x0022);
}

TEST(EofPacketCodecTest, RejectsPacketOfNineBytesOrMore) {
  // A long packet starting with 0xFE is a row whose first length-encoded
  // integer uses the 0xFE prefix, not EOF.
  const std::vector<std::uint8_t> payload(9, 0xFE);
  EXPECT_FALSE(EofPacketCodec::IsEofPacket(payload));
}

TEST(EofPacketCodecTest, RejectsWrongHeaderByte) {
  const std::vector<std::uint8_t> payload{0x00, 0x02, 0x00, 0x22, 0x00};
  EXPECT_FALSE(EofPacketCodec::IsEofPacket(payload));
}

TEST(EofPacketCodecTest, EncodesExactBytesThatParseReadsBack) {
  EofPacket value;
  value.warnings = 0x0102;
  value.statusFlags = 0x0304;
  std::vector<std::uint8_t> out = {0xEE};
  EofPacketCodec::Encode(value, out);
  ASSERT_EQ(out,
            (std::vector<std::uint8_t>{0xEE, 0xFE, 0x02, 0x01, 0x04, 0x03}));

  const std::span<const std::uint8_t> wire(out.data() + 1, out.size() - 1);
  ASSERT_TRUE(EofPacketCodec::IsEofPacket(wire));
  EofPacket parsed;
  std::string error;
  ASSERT_TRUE(EofPacketCodec::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.warnings, value.warnings);
  EXPECT_EQ(parsed.statusFlags, value.statusFlags);
}

}  // namespace
}  // namespace binlog_streamer
