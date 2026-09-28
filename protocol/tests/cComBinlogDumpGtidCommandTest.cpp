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

#include "protocol/cComBinlogDumpGtidCommand.hpp"

#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>
#include "protocol/hBinlogDumpFlags.hpp"

namespace binlog_streamer {
namespace {

TEST(ComBinlogDumpGtidCommandTest, EncodesFieldsInOrder) {
  BinlogDumpGtidCommand value;
  value.flags =
      BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2 | BINLOG_DUMP_SKIP_TAGGED_GTIDS;
  value.serverId = 0x0A0B0C0D;
  value.gtidSetEncoded = {0xAA, 0xBB, 0xCC};

  const auto payload = ComBinlogDumpGtidCommand::Encode(value);

  std::size_t pos = 0;
  EXPECT_EQ(payload[pos++], 30);  // command byte
  // flags, little-endian
  EXPECT_EQ(payload[pos++], 0x06);  // 0x02 | 0x04
  EXPECT_EQ(payload[pos++], 0x00);
  // server_id, little-endian
  EXPECT_EQ(payload[pos++], 0x0D);
  EXPECT_EQ(payload[pos++], 0x0C);
  EXPECT_EQ(payload[pos++], 0x0B);
  EXPECT_EQ(payload[pos++], 0x0A);
  // filename_length: always 0 in GTID mode
  for (int i = 0; i < 4; ++i) EXPECT_EQ(payload[pos++], 0);
  // start_position: fixed at 4
  EXPECT_EQ(payload[pos++], 4);
  for (int i = 0; i < 7; ++i) EXPECT_EQ(payload[pos++], 0);
  // gtid set length, little-endian
  EXPECT_EQ(payload[pos++], 3);
  for (int i = 0; i < 3; ++i) EXPECT_EQ(payload[pos++], 0);
  // gtid set bytes
  EXPECT_EQ(payload[pos++], 0xAA);
  EXPECT_EQ(payload[pos++], 0xBB);
  EXPECT_EQ(payload[pos++], 0xCC);
  EXPECT_EQ(pos, payload.size());
}

TEST(ComBinlogDumpGtidCommandTest, ParseReadsBackAReplicaRequest) {
  BinlogDumpGtidCommand original;
  original.flags = 0x0006;
  original.serverId = 0x01020304;
  original.gtidSetEncoded = {9, 8, 7, 6, 5};
  const std::vector<std::uint8_t> wire =
      ComBinlogDumpGtidCommand::Encode(original);

  BinlogDumpGtidCommand parsed;
  std::string error = "stale";
  ASSERT_TRUE(ComBinlogDumpGtidCommand::Parse(wire, parsed, error)) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(parsed.flags, original.flags);
  EXPECT_EQ(parsed.serverId, original.serverId);
  EXPECT_TRUE(parsed.fileName.empty());
  EXPECT_EQ(parsed.position, 4u);
  EXPECT_EQ(parsed.gtidSetEncoded, original.gtidSetEncoded);
}

TEST(ComBinlogDumpGtidCommandTest, EncodesAndParsesFileNameAndPosition) {
  BinlogDumpGtidCommand original;
  original.serverId = 1;
  original.fileName = "binlog.000042";
  original.position = 0x0102030405060708ULL;  // every byte distinct: a 32-bit
                                              // read would lose the upper half
  const std::vector<std::uint8_t> wire =
      ComBinlogDumpGtidCommand::Encode(original);

  // command(1) + flags(2) + server_id(4), then the length-prefixed name and the
  // position.
  ASSERT_GE(wire.size(), 7u + 4u + 13u + 8u);
  EXPECT_EQ(wire[7], 13);
  EXPECT_EQ(std::string(wire.begin() + 11, wire.begin() + 24), "binlog.000042");
  EXPECT_EQ(wire[24], 0x08);
  EXPECT_EQ(wire[31], 0x01);

  BinlogDumpGtidCommand parsed;
  std::string error;
  ASSERT_TRUE(ComBinlogDumpGtidCommand::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.fileName, original.fileName);
  EXPECT_EQ(parsed.position, original.position);
  EXPECT_TRUE(parsed.gtidSetEncoded.empty());
}

TEST(ComBinlogDumpGtidCommandTest, ParseIgnoresBytesAfterTheGtidSet) {
  BinlogDumpGtidCommand original;
  original.gtidSetEncoded = {1, 2, 3};
  std::vector<std::uint8_t> wire = ComBinlogDumpGtidCommand::Encode(original);
  wire.push_back(0xEE);
  BinlogDumpGtidCommand parsed;
  std::string error;
  ASSERT_TRUE(ComBinlogDumpGtidCommand::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.gtidSetEncoded, original.gtidSetEncoded);
}

TEST(ComBinlogDumpGtidCommandTest, ParseRejectsEveryTruncation) {
  BinlogDumpGtidCommand original;
  original.fileName = "f";
  original.gtidSetEncoded = {1, 2};
  const std::vector<std::uint8_t> wire =
      ComBinlogDumpGtidCommand::Encode(original);
  for (std::size_t length = 0; length < wire.size(); ++length) {
    BinlogDumpGtidCommand parsed;
    parsed.fileName = "untouched";
    std::string error;
    EXPECT_FALSE(ComBinlogDumpGtidCommand::Parse(
        std::span<const std::uint8_t>(wire.data(), length), parsed, error))
        << length;
    EXPECT_FALSE(error.empty()) << length;
    EXPECT_EQ(parsed.fileName, "untouched") << length;
  }
}

TEST(ComBinlogDumpGtidCommandTest, ParseRejectsOtherCommands) {
  std::vector<std::uint8_t> wire =
      ComBinlogDumpGtidCommand::Encode(BinlogDumpGtidCommand{});
  wire[0] = 0x12;  // COM_BINLOG_DUMP, the file/position form
  BinlogDumpGtidCommand parsed;
  std::string error;
  EXPECT_FALSE(ComBinlogDumpGtidCommand::Parse(wire, parsed, error));
  EXPECT_EQ(error, "not a COM_BINLOG_DUMP_GTID packet");
}

}  // namespace
}  // namespace binlog_streamer
