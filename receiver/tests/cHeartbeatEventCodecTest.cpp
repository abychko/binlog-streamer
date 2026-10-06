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

#include "receiver/cHeartbeatEventCodec.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

std::vector<std::uint8_t> Lenenc(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  if (value < 251) {
    out.push_back(static_cast<std::uint8_t>(value));
  } else {
    out.push_back(0xFE);
    for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  return out;
}

TEST(HeartbeatEventCodecTest, ParsesV1BodyAsPlainFileName) {
  const std::vector<std::uint8_t> body{'b', 'i', 'n', 'l', 'o', 'g', '.',
                                       '0', '0', '0', '0', '0', '1'};
  HeartbeatEvent value;
  HeartbeatEventCodec::ParseV1(body, 0, value);
  EXPECT_EQ(value.fileName, "binlog.000001");
  EXPECT_FALSE(value.position.has_value());
}

TEST(HeartbeatEventCodecTest, V1StripsTrailingChecksum) {
  std::vector<std::uint8_t> body{'b', 'i', 'n', 0xDE, 0xAD, 0xBE, 0xEF};
  HeartbeatEvent value;
  HeartbeatEventCodec::ParseV1(body, 4, value);
  EXPECT_EQ(value.fileName, "bin");
}

TEST(HeartbeatEventCodecTest, ParsesV2FileNameAndPositionFields) {
  std::vector<std::uint8_t> body;
  body.push_back(1);  // OTW_HB_LOG_FILENAME_FIELD
  const std::string name = "binlog.000041";
  body.push_back(static_cast<std::uint8_t>(name.size()));
  body.insert(body.end(), name.begin(), name.end());
  body.push_back(2);  // OTW_HB_LOG_POSITION_FIELD
  const auto encodedPosition = Lenenc(123456789);
  body.push_back(static_cast<std::uint8_t>(encodedPosition.size()));
  body.insert(body.end(), encodedPosition.begin(), encodedPosition.end());
  body.push_back(0);  // OTW_HB_HEADER_END_MARK

  HeartbeatEvent value;
  std::string error;
  ASSERT_TRUE(HeartbeatEventCodec::ParseV2(body, 0, value, error)) << error;
  EXPECT_EQ(value.fileName, "binlog.000041");
  ASSERT_TRUE(value.position.has_value());
  EXPECT_EQ(*value.position, 123456789u);
}

TEST(HeartbeatEventCodecTest, ParsesV2WithOnlyTheEndMarker) {
  const std::vector<std::uint8_t> body{0};
  HeartbeatEvent value;
  std::string error;
  ASSERT_TRUE(HeartbeatEventCodec::ParseV2(body, 0, value, error)) << error;
  EXPECT_TRUE(value.fileName.empty());
  EXPECT_FALSE(value.position.has_value());
}

TEST(HeartbeatEventCodecTest, V2SkipsAnUnrecognizedFieldByItsDeclaredLength) {
  std::vector<std::uint8_t> body;
  body.push_back(99);
  body.push_back(3);
  body.insert(body.end(), {'x', 'y', 'z'});
  body.push_back(1);
  const std::string name = "binlog.000005";
  body.push_back(static_cast<std::uint8_t>(name.size()));
  body.insert(body.end(), name.begin(), name.end());
  body.push_back(0);

  HeartbeatEvent value;
  std::string error;
  ASSERT_TRUE(HeartbeatEventCodec::ParseV2(body, 0, value, error)) << error;
  EXPECT_EQ(value.fileName, "binlog.000005");
}

TEST(HeartbeatEventCodecTest, V2RejectsAFieldLengthRunningPastTheBody) {
  std::vector<std::uint8_t> body{1, 200, 'a', 'b'};
  HeartbeatEvent value;
  std::string error;
  EXPECT_FALSE(HeartbeatEventCodec::ParseV2(body, 0, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(HeartbeatEventCodecTest, V2StripsTrailingChecksumBeforeParsingFields) {
  std::vector<std::uint8_t> body{0, 0xDE, 0xAD, 0xBE, 0xEF};
  HeartbeatEvent value;
  std::string error;
  ASSERT_TRUE(HeartbeatEventCodec::ParseV2(body, 4, value, error)) << error;
}

}  // namespace
}  // namespace binlog_streamer
