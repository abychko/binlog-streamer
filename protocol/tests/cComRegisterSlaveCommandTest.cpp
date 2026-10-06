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

#include "protocol/cComRegisterSlaveCommand.hpp"

#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

TEST(ComRegisterSlaveCommandTest, EncodesFieldsInOrder) {
  RegisterSlaveCommand value;
  value.serverId = 0x01020304;
  value.reportHost = "host";
  value.reportUser = "user";
  value.reportPassword = "pw";
  value.reportPort = 0x1516;

  const auto payload = ComRegisterSlaveCommand::Encode(value);

  std::size_t pos = 0;
  EXPECT_EQ(payload[pos++], 0x15);
  EXPECT_EQ(payload[pos++], 0x04);
  EXPECT_EQ(payload[pos++], 0x03);
  EXPECT_EQ(payload[pos++], 0x02);
  EXPECT_EQ(payload[pos++], 0x01);
  EXPECT_EQ(payload[pos++], 4);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(&payload[pos]), 4),
            "host");
  pos += 4;
  EXPECT_EQ(payload[pos++], 4);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(&payload[pos]), 4),
            "user");
  pos += 4;
  EXPECT_EQ(payload[pos++], 2);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(&payload[pos]), 2),
            "pw");
  pos += 2;
  EXPECT_EQ(payload[pos++], 0x16);
  EXPECT_EQ(payload[pos++], 0x15);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(payload[pos++], 0);
  EXPECT_EQ(pos, payload.size());
}

TEST(ComRegisterSlaveCommandTest, ParseReadsBackWhatEncodeWrote) {
  RegisterSlaveCommand original;
  original.serverId = 0x01020304;
  original.reportHost = "replica-host";
  original.reportUser = "";
  original.reportPassword = "pw";
  original.reportPort = 3306;
  const std::vector<std::uint8_t> wire =
      ComRegisterSlaveCommand::Encode(original);

  RegisterSlaveCommand parsed;
  std::string error = "stale";
  ASSERT_TRUE(ComRegisterSlaveCommand::Parse(wire, parsed, error)) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(parsed.serverId, original.serverId);
  EXPECT_EQ(parsed.reportHost, original.reportHost);
  EXPECT_EQ(parsed.reportUser, original.reportUser);
  EXPECT_EQ(parsed.reportPassword, original.reportPassword);
  EXPECT_EQ(parsed.reportPort, original.reportPort);
}

TEST(ComRegisterSlaveCommandTest, ParseReadsStringLengthAsOneRawByte) {
  // A 252-byte host: its length byte 0xFC is a plain length here, not a
  // LengthEncodedInteger prefix.
  std::vector<std::uint8_t> wire = {0x15, 7, 0, 0, 0, 0xFC};
  wire.insert(wire.end(), 252, 'h');
  wire.insert(wire.end(), {0, 0});
  wire.insert(wire.end(), {0xEA, 0x0C});
  wire.insert(wire.end(), 8, 0);
  RegisterSlaveCommand parsed;
  std::string error;
  ASSERT_TRUE(ComRegisterSlaveCommand::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.serverId, 7u);
  EXPECT_EQ(parsed.reportHost, std::string(252, 'h'));
  EXPECT_EQ(parsed.reportPort, 3306);
}

TEST(ComRegisterSlaveCommandTest, ParseRejectsEveryTruncation) {
  RegisterSlaveCommand original;
  original.serverId = 5;
  original.reportHost = "h";
  original.reportUser = "u";
  original.reportPassword = "p";
  original.reportPort = 1;
  const std::vector<std::uint8_t> wire =
      ComRegisterSlaveCommand::Encode(original);
  for (std::size_t length = 0; length < wire.size(); ++length) {
    RegisterSlaveCommand parsed;
    parsed.reportHost = "untouched";
    std::string error;
    EXPECT_FALSE(ComRegisterSlaveCommand::Parse(
        std::span<const std::uint8_t>(wire.data(), length), parsed, error))
        << length;
    EXPECT_FALSE(error.empty()) << length;
    EXPECT_EQ(parsed.reportHost, "untouched") << length;
  }
}

TEST(ComRegisterSlaveCommandTest, ParseRejectsOtherCommands) {
  std::vector<std::uint8_t> wire =
      ComRegisterSlaveCommand::Encode(RegisterSlaveCommand{});
  wire[0] = 0x03;
  RegisterSlaveCommand parsed;
  std::string error;
  EXPECT_FALSE(ComRegisterSlaveCommand::Parse(wire, parsed, error));
  EXPECT_EQ(error, "not a COM_REGISTER_SLAVE packet");
}

}  // namespace
}  // namespace binlog_streamer
