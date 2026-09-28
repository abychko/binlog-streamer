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

#include "protocol/cErrPacketCodec.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(ErrPacketCodecTest, ParsesFixture) {
  // header 0xFF, error_code=1236 (0x04D4 LE), '#', sqlstate "HY000", message
  // "boom"
  std::vector<std::uint8_t> payload{0xFF, 0xD4, 0x04, '#', 'H',
                                    'Y',  '0',  '0',  '0'};
  const std::string message = "boom";
  payload.insert(payload.end(), message.begin(), message.end());

  EXPECT_TRUE(ErrPacketCodec::IsErrPacket(payload));
  ErrPacket value;
  std::string error;
  ASSERT_TRUE(ErrPacketCodec::Parse(payload, value, error)) << error;
  EXPECT_EQ(value.errorCode, 1236);
  EXPECT_EQ(value.sqlState, "HY000");
  EXPECT_EQ(value.message, "boom");
}

TEST(ErrPacketCodecTest, RejectsWrongHeaderByte) {
  const std::vector<std::uint8_t> payload{0x00, 0xD4, 0x04, '#', 'H',
                                          'Y',  '0',  '0',  '0'};
  EXPECT_FALSE(ErrPacketCodec::IsErrPacket(payload));
}

TEST(ErrPacketCodecTest, EncodesExactBytes) {
  ErrPacket value;
  value.errorCode = 1045;
  value.sqlState = "28000";
  value.message = "No";
  std::vector<std::uint8_t> out = {0xEE};
  ErrPacketCodec::Encode(value, out);
  EXPECT_EQ(out, (std::vector<std::uint8_t>{0xEE, 0xFF, 0x15, 0x04, '#', '2',
                                            '8', '0', '0', '0', 'N', 'o'}));
}

TEST(ErrPacketCodecTest, ParseReadsBackWhatEncodeWrote) {
  ErrPacket value;
  value.errorCode = 1236;
  value.sqlState = "HY000";
  value.message =
      "Cannot replicate because the source purged required binary logs.";
  std::vector<std::uint8_t> wire;
  ErrPacketCodec::Encode(value, wire);

  ErrPacket parsed;
  std::string error;
  ASSERT_TRUE(ErrPacketCodec::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.errorCode, value.errorCode);
  EXPECT_EQ(parsed.sqlState, value.sqlState);
  EXPECT_EQ(parsed.message, value.message);
}

TEST(ErrPacketCodecTest, EncodeWritesGeneralStateForSqlStateOfWrongLength) {
  for (const char *sqlState : {"2800", "280000"}) {
    ErrPacket value;
    value.errorCode = 1;
    value.sqlState = sqlState;
    value.message = "m";
    std::vector<std::uint8_t> wire;
    ErrPacketCodec::Encode(value, wire);
    EXPECT_EQ(wire, (std::vector<std::uint8_t>{0xFF, 0x01, 0x00, '#', 'H', 'Y',
                                               '0', '0', '0', 'm'}))
        << sqlState;
  }
}

// A server refusing a connection before its greeting has no
// CLIENT_PROTOCOL_41 to go by and sends the message right after the code.
TEST(ErrPacketCodecTest, EncodeWritesNoSqlStateMarkerForEmptySqlState) {
  ErrPacket value;
  value.errorCode = 1130;
  value.message = "Host";
  std::vector<std::uint8_t> wire;
  ErrPacketCodec::Encode(value, wire);
  EXPECT_EQ(wire,
            (std::vector<std::uint8_t>{0xFF, 0x6A, 0x04, 'H', 'o', 's', 't'}));
}

TEST(ErrPacketCodecTest, ParseReadsAnErrPacketWithoutSqlStateMarker) {
  const std::vector<std::uint8_t> wire = {0xFF, 0x10, 0x04, 'T', 'o', 'o',
                                          ' ',  'm',  'a',  'n', 'y'};
  ErrPacket parsed;
  parsed.sqlState = "stale";
  std::string error;
  ASSERT_TRUE(ErrPacketCodec::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.errorCode, 1040);
  EXPECT_EQ(parsed.sqlState, "");
  EXPECT_EQ(parsed.message, "Too many");
}

TEST(ErrPacketCodecTest, ParseKeepsAShortMessageWithoutMarkerWhole) {
  const std::vector<std::uint8_t> wire = {0xFF, 0x01, 0x00, 'm'};
  ErrPacket parsed;
  std::string error;
  ASSERT_TRUE(ErrPacketCodec::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.sqlState, "");
  EXPECT_EQ(parsed.message, "m");
}

TEST(ErrPacketCodecTest, ParseRejectsAPacketShorterThanTheErrorCode) {
  const std::vector<std::uint8_t> wire = {0xFF, 0x01};
  ErrPacket parsed;
  std::string error;
  EXPECT_FALSE(ErrPacketCodec::Parse(wire, parsed, error));
}

}  // namespace
}  // namespace binlog_streamer
