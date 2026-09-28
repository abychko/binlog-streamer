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

#include "protocol/cComQueryCommand.hpp"

#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace binlog_streamer {
namespace {

TEST(ComQueryCommandTest, PrependsCommandByteToSql) {
  const auto payload = ComQueryCommand::Encode("SELECT 1");
  ASSERT_EQ(payload.size(), 1u + 8u);
  EXPECT_EQ(payload[0], 0x03);
  const std::string sql(reinterpret_cast<const char *>(&payload[1]), 8);
  EXPECT_EQ(sql, "SELECT 1");
}

TEST(ComQueryCommandTest, HandlesEmptySql) {
  const auto payload = ComQueryCommand::Encode("");
  EXPECT_EQ(payload, (std::vector<std::uint8_t>{0x03}));
}

TEST(ComQueryCommandTest, ParseReadsBackWhatEncodeWrote) {
  const std::vector<std::uint8_t> wire =
      ComQueryCommand::Encode("SELECT @@GLOBAL.SERVER_ID");
  std::string_view sql;
  std::string error = "stale";
  ASSERT_TRUE(ComQueryCommand::Parse(wire, sql, error)) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(sql, "SELECT @@GLOBAL.SERVER_ID");
}

TEST(ComQueryCommandTest, ParsesEmptySqlAndKeepsEmbeddedNul) {
  // sql views into the payload, so both payloads outlive the checks.
  const std::vector<std::uint8_t> commandByteOnly = {0x03};
  const std::vector<std::uint8_t> withEmbeddedNul = {0x03, 'a', 0x00, 'b'};
  std::string_view sql = "stale";
  std::string error;
  ASSERT_TRUE(ComQueryCommand::Parse(commandByteOnly, sql, error)) << error;
  EXPECT_TRUE(sql.empty());

  ASSERT_TRUE(ComQueryCommand::Parse(withEmbeddedNul, sql, error)) << error;
  EXPECT_EQ(sql, std::string_view("a\0b", 3));
}

TEST(ComQueryCommandTest, ParseRejectsOtherCommandsAndEmptyPayload) {
  std::string_view sql;
  std::string error;
  EXPECT_FALSE(
      ComQueryCommand::Parse(std::vector<std::uint8_t>{0x01}, sql, error));
  EXPECT_EQ(error, "not a COM_QUERY packet");
  error.clear();
  EXPECT_FALSE(ComQueryCommand::Parse(std::vector<std::uint8_t>{}, sql, error));
  EXPECT_EQ(error, "not a COM_QUERY packet");
}

}  // namespace
}  // namespace binlog_streamer
