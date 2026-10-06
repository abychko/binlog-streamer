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

#include "protocol/cHandshakeV10Codec.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include "protocol/hCapabilityFlags.hpp"

namespace binlog_streamer {
namespace {

std::vector<std::uint8_t> HexToBytes(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return bytes;
}

std::vector<std::uint8_t> RealHandshakeV10Payload() {
  return HexToBytes(
      "0a382e342e313100a0cf00006d6a4f4b4b463a3c00ffffff0200ffdf15000000"
      "000000000000004426363073032132767b114f0063616368696e675f736861"
      "325f70617373776f726400");
}

TEST(HandshakeV10CodecTest, ParsesRealServerGreeting) {
  HandshakeV10 value;
  std::string error;
  ASSERT_TRUE(HandshakeV10Codec::Parse(RealHandshakeV10Payload(), value, error))
      << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(value.protocolVersion, 10);
  EXPECT_EQ(value.serverVersion, "8.4.11");
  EXPECT_EQ(value.threadId, 53152u);
  ASSERT_EQ(value.authPluginData.size(), 21u);
  EXPECT_EQ(value.authPluginData[20], 0);
  EXPECT_TRUE((value.capabilities & CLIENT_PLUGIN_AUTH) != 0);
  EXPECT_TRUE((value.capabilities & CLIENT_PROTOCOL_41) != 0);
  EXPECT_EQ(value.characterSet, 255);
  EXPECT_EQ(value.authPluginName, "caching_sha2_password");
}

TEST(HandshakeV10CodecTest, ScramblePart1MatchesWireBytes) {
  HandshakeV10 value;
  std::string error;
  ASSERT_TRUE(
      HandshakeV10Codec::Parse(RealHandshakeV10Payload(), value, error));
  const std::vector<std::uint8_t> expectedPart1 =
      HexToBytes("6d6a4f4b4b463a3c");
  ASSERT_GE(value.authPluginData.size(), 8u);
  EXPECT_TRUE(std::equal(expectedPart1.begin(), expectedPart1.end(),
                         value.authPluginData.begin()));
}

TEST(HandshakeV10CodecTest, RejectsWrongProtocolVersion) {
  std::vector<std::uint8_t> payload = RealHandshakeV10Payload();
  payload[0] = 9;
  HandshakeV10 value;
  std::string error;
  EXPECT_FALSE(HandshakeV10Codec::Parse(payload, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(HandshakeV10CodecTest, RejectsTruncatedPacket) {
  std::vector<std::uint8_t> payload = RealHandshakeV10Payload();
  payload.resize(payload.size() - 5);
  HandshakeV10 value;
  std::string error;
  EXPECT_FALSE(HandshakeV10Codec::Parse(payload, value, error));
}

TEST(HandshakeV10CodecTest, RejectsMissingServerVersionTerminator) {
  std::vector<std::uint8_t> payload = RealHandshakeV10Payload();
  for (std::size_t i = 1; i < payload.size(); ++i) {
    if (payload[i] == 0) payload[i] = 'x';
  }
  HandshakeV10 value;
  std::string error;
  EXPECT_FALSE(HandshakeV10Codec::Parse(payload, value, error));
}

TEST(HandshakeV10CodecTest, EncodeReproducesRealServerGreeting) {
  HandshakeV10 value;
  std::string error;
  ASSERT_TRUE(HandshakeV10Codec::Parse(RealHandshakeV10Payload(), value, error))
      << error;

  std::vector<std::uint8_t> out;
  error = "stale";
  ASSERT_TRUE(HandshakeV10Codec::Encode(value, out, error)) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(out, RealHandshakeV10Payload());
}

TEST(HandshakeV10CodecTest, EncodeAppendsToOutput) {
  HandshakeV10 value;
  std::string error;
  ASSERT_TRUE(HandshakeV10Codec::Parse(RealHandshakeV10Payload(), value, error))
      << error;

  std::vector<std::uint8_t> out = {0xEE};
  ASSERT_TRUE(HandshakeV10Codec::Encode(value, out, error)) << error;
  std::vector<std::uint8_t> expected = {0xEE};
  const std::vector<std::uint8_t> greeting = RealHandshakeV10Payload();
  expected.insert(expected.end(), greeting.begin(), greeting.end());
  EXPECT_EQ(out, expected);
}

TEST(HandshakeV10CodecTest, EncodeRejectsWhatItCannotRepresent) {
  HandshakeV10 valid;
  std::string error;
  ASSERT_TRUE(HandshakeV10Codec::Parse(RealHandshakeV10Payload(), valid, error))
      << error;

  struct Case {
    const char *name;
    HandshakeV10 value;
    const char *expectedError;
  };
  std::vector<Case> cases;
  cases.push_back({"no plugin auth", valid,
                   "handshake without CLIENT_PLUGIN_AUTH is not supported"});
  cases.back().value.capabilities &= ~CLIENT_PLUGIN_AUTH;
  cases.push_back({"scramble of 7 bytes", valid,
                   "handshake auth plugin data length is out of range"});
  cases.back().value.authPluginData.assign(7, 1);
  cases.push_back({"scramble of 256 bytes", valid,
                   "handshake auth plugin data length is out of range"});
  cases.back().value.authPluginData.assign(256, 1);
  cases.push_back({"NUL in server version", valid,
                   "handshake string field contains a NUL"});
  cases.back().value.serverVersion = std::string("8.4\0.11", 7);
  cases.push_back(
      {"NUL in plugin name", valid, "handshake string field contains a NUL"});
  cases.back().value.authPluginName = std::string("a\0b", 3);

  for (const Case &testCase : cases) {
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(HandshakeV10Codec::Encode(testCase.value, out, error))
        << testCase.name;
    EXPECT_EQ(error, testCase.expectedError) << testCase.name;
    EXPECT_TRUE(out.empty()) << testCase.name;
  }
}

TEST(HandshakeV10CodecTest, EncodeAcceptsScrambleLengthBounds) {
  HandshakeV10 value;
  std::string error;
  ASSERT_TRUE(HandshakeV10Codec::Parse(RealHandshakeV10Payload(), value, error))
      << error;
  for (const std::size_t length : {std::size_t{8}, std::size_t{255}}) {
    value.authPluginData.assign(length, 7);
    std::vector<std::uint8_t> out;
    ASSERT_TRUE(HandshakeV10Codec::Encode(value, out, error))
        << length << ": " << error;
    HandshakeV10 parsed;
    ASSERT_TRUE(HandshakeV10Codec::Parse(out, parsed, error))
        << length << ": " << error;
    EXPECT_EQ(parsed.authPluginData, value.authPluginData) << length;
  }
}

}  // namespace
}  // namespace binlog_streamer
