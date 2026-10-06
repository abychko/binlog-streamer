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

#include "binlog/cEventHeaderCodec.hpp"

#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <string>
#include "binlog/hEventFlags.hpp"

namespace binlog_streamer {
namespace {

TEST(EventHeaderCodecTest, ParsesFixedLittleEndianFields) {
  const std::array<std::uint8_t, EVENT_HEADER_LENGTH> data{
      0x01, 0x02, 0x03, 0x04,  // timestamp = 0x04030201
      33,                      // type = GTID_EVENT
      0x0A, 0x00, 0x00, 0x00,  // server_id = 10
      0x40, 0x00, 0x00, 0x00,  // event_length = 64
      0x50, 0x01, 0x00, 0x00,  // next_position = 0x150
      0x20, 0x00,              // flags = ARTIFICIAL
  };
  EventHeader header;
  std::string error;
  ASSERT_TRUE(EventHeaderCodec::Parse(data, header, error));
  EXPECT_EQ(header.timestamp, 0x04030201u);
  EXPECT_EQ(header.type, 33);
  EXPECT_EQ(header.serverId, 10u);
  EXPECT_EQ(header.eventLength, 64u);
  EXPECT_EQ(header.nextPosition, 0x150u);
  EXPECT_EQ(header.flags, 0x20u);
}

TEST(EventHeaderCodecTest, ParsesAllZeroArtificialHeaderShape) {
  std::array<std::uint8_t, EVENT_HEADER_LENGTH> data{};
  data[4] = 4;  // ROTATE_EVENT
  data[9] = 25;
  data[10] = 0;
  data[11] = 0;
  data[12] = 0;     // event_length = 25
  data[17] = 0x20;  // ARTIFICIAL

  EventHeader header;
  std::string error;
  ASSERT_TRUE(EventHeaderCodec::Parse(data, header, error));
  EXPECT_EQ(header.timestamp, 0u);
  EXPECT_EQ(header.type, 4);
  EXPECT_EQ(header.nextPosition, 0u);
  EXPECT_EQ(header.flags, EVENT_FLAG_ARTIFICIAL);
}

}  // namespace
}  // namespace binlog_streamer
