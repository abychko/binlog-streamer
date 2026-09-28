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

#include "binlog/cRotateEventCodec.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace binlog_streamer {
namespace {

std::vector<std::uint8_t> BuildBody(
    std::uint64_t position, const std::string &fileName,
    const std::vector<std::uint8_t> &checksum = {}) {
  std::vector<std::uint8_t> body;
  for (int i = 0; i < 8; ++i)
    body.push_back(static_cast<std::uint8_t>(position >> (8 * i)));
  body.insert(body.end(), fileName.begin(), fileName.end());
  body.insert(body.end(), checksum.begin(), checksum.end());
  return body;
}

TEST(RotateEventCodecTest, ParsesPositionAndFileNameWithoutChecksum) {
  const auto body = BuildBody(4, "binlog.000042");
  RotateEvent value;
  std::string error;
  ASSERT_TRUE(RotateEventCodec::Parse(body, 0, value, error)) << error;
  EXPECT_EQ(value.position, 4u);
  EXPECT_EQ(value.fileName, "binlog.000042");
}

TEST(RotateEventCodecTest, ExcludesTrailingChecksumFromFileName) {
  const auto body = BuildBody(4, "binlog.000042", {0xDE, 0xAD, 0xBE, 0xEF});
  RotateEvent value;
  std::string error;
  ASSERT_TRUE(RotateEventCodec::Parse(body, 4, value, error)) << error;
  EXPECT_EQ(value.fileName,
            "binlog.000042");  // the 4 checksum bytes are not part of the name
}

TEST(RotateEventCodecTest, RejectsBodyShorterThanThePositionField) {
  const std::vector<std::uint8_t> body{1, 2, 3};  // fewer than 8 bytes
  RotateEvent value;
  std::string error;
  EXPECT_FALSE(RotateEventCodec::Parse(body, 0, value, error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace binlog_streamer
