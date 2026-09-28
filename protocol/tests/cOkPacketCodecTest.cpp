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

#include "protocol/cOkPacketCodec.hpp"

#include <gtest/gtest.h>
#include "protocol/cLengthEncodedString.hpp"

namespace binlog_streamer {
namespace {

TEST(OkPacketCodecTest, IsOkPacketAcceptsZeroHeaderAlways) {
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(0x00, false));
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(0x00, true));
}

TEST(OkPacketCodecTest, IsOkPacketAccepts0xFEOnlyWithDeprecateEof) {
  EXPECT_FALSE(OkPacketCodec::IsOkPacket(0xFE, false));
  EXPECT_TRUE(OkPacketCodec::IsOkPacket(0xFE, true));
}

TEST(OkPacketCodecTest, ParsesFixtureWithNoMessage) {
  const std::vector<std::uint8_t> payload{0x00, 5, 0, 0x02, 0x00, 0x00, 0x00};
  OkPacket value;
  std::string error;
  ASSERT_TRUE(
      OkPacketCodec::Parse(payload, /*clientSessionTrack=*/false, value, error))
      << error;
  EXPECT_EQ(value.affectedRows, 5u);
  EXPECT_EQ(value.lastInsertId, 0u);
  EXPECT_EQ(value.statusFlags, 0x0002);
  EXPECT_EQ(value.warnings, 0u);
  EXPECT_TRUE(value.info.empty());
}

TEST(OkPacketCodecTest, ParsesTrailingInfoMessage) {
  std::vector<std::uint8_t> payload{0x00, 0, 0, 0x00, 0x00, 0x00, 0x00};
  const std::string message = "some message";
  payload.insert(payload.end(), message.begin(), message.end());
  OkPacket value;
  std::string error;
  ASSERT_TRUE(OkPacketCodec::Parse(payload, /*clientSessionTrack=*/false, value,
                                   error));
  EXPECT_EQ(value.info, message);
}

TEST(OkPacketCodecTest, RejectsTooShortPacket) {
  const std::vector<std::uint8_t> payload{0x00, 0, 0, 0x00, 0x00};
  OkPacket value;
  std::string error;
  EXPECT_FALSE(OkPacketCodec::Parse(payload, /*clientSessionTrack=*/false,
                                    value, error));
}

TEST(OkPacketCodecTest, SessionTrackEmptyRemainderIsEmptyInfo) {
  const std::vector<std::uint8_t> payload{0x00, 0, 0, 0x00, 0x00, 0x00, 0x00};
  OkPacket value;
  std::string error;
  ASSERT_TRUE(
      OkPacketCodec::Parse(payload, /*clientSessionTrack=*/true, value, error))
      << error;
  EXPECT_TRUE(value.info.empty());
}

TEST(OkPacketCodecTest,
     SessionTrackDecodesLenencInfoAndHidesTrailingSessionState) {
  std::vector<std::uint8_t> payload{0x00, 0, 0, 0x00, 0x00, 0x00, 0x00};
  LengthEncodedString::Encode("some message", payload);
  // Trailing bytes representing SessionStateInfo (opaque here - real
  // content is not this codec's concern): must not leak into info.
  const std::vector<std::uint8_t> sessionState{0x01, 0x02, 0x03};
  payload.insert(payload.end(), sessionState.begin(), sessionState.end());

  OkPacket value;
  std::string error;
  ASSERT_TRUE(
      OkPacketCodec::Parse(payload, /*clientSessionTrack=*/true, value, error))
      << error;
  EXPECT_EQ(value.info, "some message");
}

TEST(OkPacketCodecTest, EncodesExactBytesWithoutInfo) {
  OkPacket value;
  value.statusFlags = 0x0002;  // SERVER_STATUS_AUTOCOMMIT
  for (const bool clientSessionTrack : {false, true}) {
    std::vector<std::uint8_t> out;
    OkPacketCodec::Encode(value, clientSessionTrack, out);
    EXPECT_EQ(out, (std::vector<std::uint8_t>{0x00, 0x00, 0x00, 0x02, 0x00,
                                              0x00, 0x00}))
        << clientSessionTrack;
  }
}

TEST(OkPacketCodecTest, EncodesInfoInTheFormParseReads) {
  OkPacket value;
  value.affectedRows = 300;  // needs the 0xFC LengthEncodedInteger form
  value.lastInsertId = 7;
  value.statusFlags = 0x0002;
  value.warnings = 0x0102;
  value.info = "Rows matched: 300";

  std::vector<std::uint8_t> plain;
  OkPacketCodec::Encode(value, false, plain);
  std::vector<std::uint8_t> tracked;
  OkPacketCodec::Encode(value, true, tracked);
  EXPECT_EQ(tracked.size(), plain.size() + 1);  // the info length prefix

  for (const bool clientSessionTrack : {false, true}) {
    OkPacket parsed;
    std::string error;
    ASSERT_TRUE(OkPacketCodec::Parse(clientSessionTrack ? tracked : plain,
                                     clientSessionTrack, parsed, error))
        << error;
    EXPECT_EQ(parsed.affectedRows, value.affectedRows);
    EXPECT_EQ(parsed.lastInsertId, value.lastInsertId);
    EXPECT_EQ(parsed.statusFlags, value.statusFlags);
    EXPECT_EQ(parsed.warnings, value.warnings);
    EXPECT_EQ(parsed.info, value.info);
  }
}

}  // namespace
}  // namespace binlog_streamer
