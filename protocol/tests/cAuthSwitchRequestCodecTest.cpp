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

#include "protocol/cAuthSwitchRequestCodec.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(AuthSwitchRequestCodecTest, RecognizesAndParsesFixture) {
  // 0xFE + "caching_sha2_password\0" + 20 bytes of plugin data
  std::vector<std::uint8_t> payload{0xFE};
  const std::string plugin = "caching_sha2_password";
  payload.insert(payload.end(), plugin.begin(), plugin.end());
  payload.push_back(0);
  const std::vector<std::uint8_t> data(20, 0x5A);
  payload.insert(payload.end(), data.begin(), data.end());

  EXPECT_TRUE(AuthSwitchRequestCodec::IsAuthSwitchRequest(payload));

  AuthSwitchRequest value;
  std::string error;
  ASSERT_TRUE(AuthSwitchRequestCodec::Parse(payload, value, error)) << error;
  EXPECT_EQ(value.pluginName, plugin);
  EXPECT_EQ(value.pluginData, data);
}

TEST(AuthSwitchRequestCodecTest, RejectsWrongHeaderByte) {
  const std::vector<std::uint8_t> payload{0x00, 'x', 0};
  EXPECT_FALSE(AuthSwitchRequestCodec::IsAuthSwitchRequest(payload));
  AuthSwitchRequest value;
  std::string error;
  EXPECT_FALSE(AuthSwitchRequestCodec::Parse(payload, value, error));
}

TEST(AuthSwitchRequestCodecTest, RejectsMissingTerminator) {
  const std::vector<std::uint8_t> payload{0xFE, 'a', 'b',
                                          'c'};  // no NUL after the plugin name
  AuthSwitchRequest value;
  std::string error;
  EXPECT_FALSE(AuthSwitchRequestCodec::Parse(payload, value, error));
}

TEST(AuthSwitchRequestCodecTest, EncodesExactBytesThatParseReadsBack) {
  AuthSwitchRequest value;
  value.pluginName = "ab";
  value.pluginData = {
      0x01, 0x00, 0x02,
      0x00};  // NUL bytes inside and at the end of the data survive
  std::vector<std::uint8_t> out = {0xEE};
  AuthSwitchRequestCodec::Encode(value, out);
  ASSERT_EQ(out, (std::vector<std::uint8_t>{0xEE, 0xFE, 'a', 'b', 0x00, 0x01,
                                            0x00, 0x02, 0x00}));

  AuthSwitchRequest parsed;
  std::string error;
  ASSERT_TRUE(AuthSwitchRequestCodec::Parse(
      std::span<const std::uint8_t>(out.data() + 1, out.size() - 1), parsed,
      error))
      << error;
  EXPECT_EQ(parsed.pluginName, value.pluginName);
  EXPECT_EQ(parsed.pluginData, value.pluginData);
}

}  // namespace
}  // namespace binlog_streamer
