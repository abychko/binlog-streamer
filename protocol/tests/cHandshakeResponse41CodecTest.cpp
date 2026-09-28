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

#include "protocol/cHandshakeResponse41Codec.hpp"

#include <gtest/gtest.h>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "protocol/cCachingSha2Scramble.hpp"
#include "protocol/hCapabilityFlags.hpp"
#include "protocol/hProtocolLimits.hpp"

namespace binlog_streamer {
namespace {

std::uint32_t ReadUint32LE(const std::vector<std::uint8_t> &bytes,
                           std::size_t pos) {
  return static_cast<std::uint32_t>(bytes[pos]) |
         (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
}

TEST(HandshakeResponse41CodecTest, EncodesFixedHeaderAndUsername) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH;
  value.maxPacketSize = 0x01000000;
  value.characterSet = 45;
  value.username = "binlog_streamer";
  value.authResponse = {1, 2, 3, 4};
  value.authPluginName = "caching_sha2_password";

  std::vector<std::uint8_t> out;
  HandshakeResponse41Codec::Encode(value, out);

  ASSERT_GE(out.size(), 32u);
  EXPECT_EQ(ReadUint32LE(out, 0), value.capabilities);
  EXPECT_EQ(ReadUint32LE(out, 4), value.maxPacketSize);
  EXPECT_EQ(out[8], 45);
  for (std::size_t i = 9; i < 32; ++i)
    EXPECT_EQ(out[i], 0) << "reserved byte " << i;

  std::size_t pos = 32;
  const std::string username(reinterpret_cast<const char *>(&out[pos]),
                             value.username.size());
  EXPECT_EQ(username, "binlog_streamer");
  pos += value.username.size();
  EXPECT_EQ(out[pos], 0);
  ++pos;

  EXPECT_EQ(out[pos], 4);  // auth response length prefix (< 251, single byte)
  ++pos;
  EXPECT_EQ((std::vector<std::uint8_t>(
                out.begin() + static_cast<std::ptrdiff_t>(pos),
                out.begin() + static_cast<std::ptrdiff_t>(pos) + 4)),
            (std::vector<std::uint8_t>{1, 2, 3, 4}));
  pos += 4;

  const std::string pluginName(reinterpret_cast<const char *>(&out[pos]),
                               value.authPluginName.size());
  EXPECT_EQ(pluginName, "caching_sha2_password");
  pos += value.authPluginName.size();
  EXPECT_EQ(out[pos], 0);
  ++pos;
  EXPECT_EQ(
      pos, out.size());  // no connection attrs: nothing follows the plugin name
}

TEST(HandshakeResponse41CodecTest, OmitsPluginNameWithoutCapability) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41;  // no CLIENT_PLUGIN_AUTH
  value.username = "u";
  value.authResponse = {};
  value.authPluginName = "should_not_appear";

  std::vector<std::uint8_t> out;
  HandshakeResponse41Codec::Encode(value, out);

  // 32 (header) + "u"+NUL (2) + auth response length prefix 0 (1 byte, empty)
  ASSERT_EQ(out.size(), 32u + 2u + 1u);
  EXPECT_EQ(out.back(),
            0);  // the auth response length prefix, not a plugin name byte
}

TEST(HandshakeResponse41CodecTest,
     EncodesConnectionAttributesWithTotalLengthPrefix) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_CONNECT_ATTRS;
  value.username = "u";
  value.connectionAttributes = {{"k", "v"}};

  std::vector<std::uint8_t> out;
  HandshakeResponse41Codec::Encode(value, out);

  // 32 (header) + "u"+NUL (2) + auth response prefix 0 (1) + attrs total-length
  // prefix (1) + "k"(2) + "v"(2)
  ASSERT_EQ(out.size(), 32u + 2u + 1u + 1u + 2u + 2u);
  const std::size_t attrsStart = 32 + 2 + 1;
  EXPECT_EQ(out[attrsStart], 4);  // total length: 1(len)+1('k') + 1(len)+1('v')
  EXPECT_EQ(out[attrsStart + 1], 1);
  EXPECT_EQ(out[attrsStart + 2], 'k');
  EXPECT_EQ(out[attrsStart + 3], 1);
  EXPECT_EQ(out[attrsStart + 4], 'v');
}

std::vector<std::uint8_t> HexToBytes(std::string_view hex) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return bytes;
}

// os_user replaced with the equally long "testuser".
std::vector<std::uint8_t> RealHandshakeResponse41Payload() {
  return HexToBytes(
      "85a2bf1900000001080000000000000000000000000000000000000000000000"
      "636170747572655f75736572002085de6faebba52a63c8ec5c161ef675e35722"
      "5c1f906887fc2654cfd414a33e0863616368696e675f736861325f7061737377"
      "6f72640079035f6f73096d61636f7332362e36095f706c6174666f726d056172"
      "6d36340f5f636c69656e745f76657273696f6e06382e342e31310c5f636c6965"
      "6e745f6e616d65086c69626d7973716c045f7069640432333931076f735f7573"
      "65720874657374757365720c70726f6772616d5f6e616d65056d7973716c");
}

// The same client and credentials; differences: the CLIENT_CONNECT_WITH_DB
// bit, the database name between the auth response and the plugin name,
// and the client's pid.
std::vector<std::uint8_t> RealHandshakeResponse41PayloadWithDatabase() {
  return HexToBytes(
      "8da2bf1900000001080000000000000000000000000000000000000000000000"
      "636170747572655f75736572002085de6faebba52a63c8ec5c161ef675e35722"
      "5c1f906887fc2654cfd414a33e08636170747572655f64620063616368696e67"
      "5f736861325f70617373776f72640079035f6f73096d61636f7332362e36095f"
      "706c6174666f726d0561726d36340f5f636c69656e745f76657273696f6e0638"
      "2e342e31310c5f636c69656e745f6e616d65086c69626d7973716c045f706964"
      "0439313437076f735f757365720874657374757365720c70726f6772616d5f6e"
      "616d65056d7973716c");
}

// The nonce of the replayed greeting: its auth-plugin-data without the
// trailing 0x00.
constexpr std::array<std::uint8_t, SCRAMBLE_LENGTH> REPLAYED_GREETING_NONCE = {
    0x6d, 0x6a, 0x4f, 0x4b, 0x4b, 0x46, 0x3a, 0x3c, 0x44, 0x26,
    0x36, 0x30, 0x73, 0x03, 0x21, 0x32, 0x76, 0x7b, 0x11, 0x4f};

TEST(HandshakeResponse41CodecTest, ParsesRealClientReply) {
  HandshakeResponse41 value;
  std::string error = "stale";
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(RealHandshakeResponse41Payload(),
                                              value, error))
      << error;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(value.capabilities, 0x19bfa285u);
  EXPECT_EQ(value.capabilities & CLIENT_CONNECT_WITH_DB, 0u);
  EXPECT_EQ(value.maxPacketSize, 0x01000000u);
  EXPECT_EQ(value.characterSet, 8);
  EXPECT_EQ(value.username, "capture_user");
  EXPECT_TRUE(value.database.empty());
  EXPECT_EQ(value.authPluginName, "caching_sha2_password");

  const std::vector<std::pair<std::string, std::string>> expectedAttributes = {
      {"_os", "macos26.6"},
      {"_platform", "arm64"},
      {"_client_version", "8.4.11"},
      {"_client_name", "libmysql"},
      {"_pid", "2391"},
      {"os_user", "testuser"},
      {"program_name", "mysql"}};
  EXPECT_EQ(value.connectionAttributes, expectedAttributes);
}

TEST(HandshakeResponse41CodecTest,
     RealClientAuthResponseIsTheScrambleOfItsPassword) {
  // Ties the parsed auth response to the algorithm the server checks it
  // with: a wrong offset/length would still produce 32 bytes, just not these.
  HandshakeResponse41 value;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(RealHandshakeResponse41Payload(),
                                              value, error))
      << error;
  const auto expected =
      CachingSha2Scramble::Compute("capture-secret", REPLAYED_GREETING_NONCE);
  EXPECT_EQ(value.authResponse,
            std::vector<std::uint8_t>(expected.begin(), expected.end()));
}

TEST(HandshakeResponse41CodecTest, ParsesRealClientReplyWithDatabase) {
  HandshakeResponse41 value;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(
      RealHandshakeResponse41PayloadWithDatabase(), value, error))
      << error;
  EXPECT_NE(value.capabilities & CLIENT_CONNECT_WITH_DB, 0u);
  EXPECT_EQ(value.username, "capture_user");
  EXPECT_EQ(value.authResponse.size(), CachingSha2Scramble::LENGTH);
  EXPECT_EQ(value.database, "capture_db");
  EXPECT_EQ(value.authPluginName, "caching_sha2_password");
  ASSERT_EQ(value.connectionAttributes.size(), 7u);
  EXPECT_EQ(value.connectionAttributes[6].second, "mysql");
}

TEST(HandshakeResponse41CodecTest, ParseReadsBackWhatEncodeWrote) {
  HandshakeResponse41 original;
  original.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH |
                          CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA |
                          CLIENT_CONNECT_ATTRS;
  original.maxPacketSize = 0x40000000;
  original.characterSet = 255;
  original.username = "replica";
  original.authResponse.assign(32, 0xAB);
  original.authPluginName = "caching_sha2_password";
  original.connectionAttributes = {{"_client_name", "libmysql"}, {"empty", ""}};

  std::vector<std::uint8_t> wire;
  HandshakeResponse41Codec::Encode(original, wire);
  HandshakeResponse41 parsed;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(wire, parsed, error)) << error;
  EXPECT_EQ(parsed.capabilities, original.capabilities);
  EXPECT_EQ(parsed.maxPacketSize, original.maxPacketSize);
  EXPECT_EQ(parsed.characterSet, original.characterSet);
  EXPECT_EQ(parsed.username, original.username);
  EXPECT_EQ(parsed.authResponse, original.authResponse);
  EXPECT_EQ(parsed.authPluginName, original.authPluginName);
  EXPECT_EQ(parsed.connectionAttributes, original.connectionAttributes);
}

std::vector<std::uint8_t> ResponseWithBody(
    std::uint32_t capabilities, const std::vector<std::uint8_t> &body) {
  std::vector<std::uint8_t> payload(32, 0);
  payload[0] = static_cast<std::uint8_t>(capabilities);
  payload[1] = static_cast<std::uint8_t>(capabilities >> 8);
  payload[2] = static_cast<std::uint8_t>(capabilities >> 16);
  payload[3] = static_cast<std::uint8_t>(capabilities >> 24);
  payload.insert(payload.end(), body.begin(), body.end());
  return payload;
}

TEST(HandshakeResponse41CodecTest,
     AuthResponseLengthIsOneRawByteWithoutLenencCapability) {
  // 0xFC is a plain length of 252 here; read as a LengthEncodedInteger it
  // would be the prefix of a 2-byte value and swallow the first two
  // response bytes.
  std::vector<std::uint8_t> body = {'u', 0, 0xFC};
  body.insert(body.end(), 252, 0x5A);
  HandshakeResponse41 value;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(
      ResponseWithBody(CLIENT_PROTOCOL_41, body), value, error))
      << error;
  EXPECT_EQ(value.authResponse, std::vector<std::uint8_t>(252, 0x5A));
}

TEST(HandshakeResponse41CodecTest,
     AuthResponseLengthIsLengthEncodedWithLenencCapability) {
  std::vector<std::uint8_t> body = {'u', 0, 0xFC, 0xFC,
                                    0x00};  // 0xFC prefix + 2-byte value 252
  body.insert(body.end(), 252, 0x5A);
  HandshakeResponse41 value;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(
      ResponseWithBody(
          CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA, body),
      value, error))
      << error;
  EXPECT_EQ(value.authResponse, std::vector<std::uint8_t>(252, 0x5A));
}

struct MalformedResponseCase {
  const char *name;
  std::uint32_t capabilities;
  std::vector<std::uint8_t> body;
  const char *expectedError;
};

TEST(HandshakeResponse41CodecTest, RejectsMalformedPayloads) {
  constexpr std::uint32_t BASE = CLIENT_PROTOCOL_41;
  const std::vector<MalformedResponseCase> cases = {
      {"no protocol 41",
       0,
       {'u', 0, 0},
       "handshake response is not in the CLIENT_PROTOCOL_41 form"},
      {"user without NUL",
       BASE,
       {'u', 's', 'r'},
       "handshake response user name is not NUL-terminated"},
      {"no auth length",
       BASE,
       {'u', 0},
       "handshake response too short for the auth response length"},
      {"lenenc auth length cut",
       BASE | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA,
       {'u', 0, 0xFC, 0x01},
       "handshake response: malformed auth response length"},
      {"lenenc auth length NULL",
       BASE | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA,
       {'u', 0, 0xFB},
       "handshake response: malformed auth response length"},
      {"auth response cut",
       BASE,
       {'u', 0, 4, 1, 2, 3},
       "handshake response too short for its auth response"},
      {"database without NUL",
       BASE | CLIENT_CONNECT_WITH_DB,
       {'u', 0, 0, 'd', 'b'},
       "handshake response database name is not NUL-terminated"},
      {"plugin without NUL",
       BASE | CLIENT_PLUGIN_AUTH,
       {'u', 0, 0, 'p'},
       "handshake response auth plugin name is not NUL-terminated"},
      {"attributes length missing",
       BASE | CLIENT_CONNECT_ATTRS,
       {'u', 0, 0},
       "handshake response: malformed connection attributes length"},
      {"attributes cut",
       BASE | CLIENT_CONNECT_ATTRS,
       {'u', 0, 0, 5, 1, 'k'},
       "handshake response too short for its connection attributes"},
      {"attribute key cut",
       BASE | CLIENT_CONNECT_ATTRS,
       {'u', 0, 0, 2, 5, 'k'},
       "handshake response: malformed connection attribute key"},
      {"attribute without value",
       BASE | CLIENT_CONNECT_ATTRS,
       {'u', 0, 0, 2, 1, 'k'},
       "handshake response: malformed connection attribute value"},
  };
  for (const MalformedResponseCase &testCase : cases) {
    HandshakeResponse41 value;
    value.username = "untouched";
    std::string error;
    EXPECT_FALSE(HandshakeResponse41Codec::Parse(
        ResponseWithBody(testCase.capabilities, testCase.body), value, error))
        << testCase.name;
    EXPECT_EQ(error, testCase.expectedError) << testCase.name;
    EXPECT_EQ(value.username, "untouched") << testCase.name;
  }
}

TEST(HandshakeResponse41CodecTest, CarriesTheZstdLevelAfterTheAttributes) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH |
                       CLIENT_CONNECT_ATTRS | CLIENT_ZSTD_COMPRESSION_ALGORITHM;
  value.username = "repl";
  value.authPluginName = "caching_sha2_password";
  value.connectionAttributes = {{"_client_name", "libmysql"},
                                {"program_name", "mysqld"}};
  value.zstdCompressionLevel = 22;
  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::Encode(value, encoded);
  EXPECT_EQ(encoded.back(), 22);

  HandshakeResponse41 parsed;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(encoded, parsed, error)) << error;
  EXPECT_EQ(parsed.zstdCompressionLevel, 22);
  EXPECT_EQ(parsed.connectionAttributes, value.connectionAttributes);
}

TEST(HandshakeResponse41CodecTest, LeavesTheZstdLevelAtZeroWhenNotAskedFor) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH;
  value.username = "repl";
  value.authPluginName = "caching_sha2_password";
  value.zstdCompressionLevel = 9;  // ignored: the capability is not set
  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::Encode(value, encoded);

  HandshakeResponse41 parsed;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(encoded, parsed, error)) << error;
  EXPECT_EQ(parsed.zstdCompressionLevel, 0);
}

TEST(HandshakeResponse41CodecTest, LeavesTheZstdLevelAtZeroWhenItIsMissing) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH;
  value.username = "repl";
  value.authPluginName = "caching_sha2_password";
  std::vector<std::uint8_t> encoded;
  HandshakeResponse41Codec::Encode(value, encoded);
  // The bit set after encoding, so the level byte the peer promised is
  // simply not there.
  encoded[3] |=
      static_cast<std::uint8_t>(CLIENT_ZSTD_COMPRESSION_ALGORITHM >> 24);

  HandshakeResponse41 parsed;
  std::string error;
  ASSERT_TRUE(HandshakeResponse41Codec::Parse(encoded, parsed, error)) << error;
  EXPECT_EQ(parsed.zstdCompressionLevel, 0);
}

TEST(HandshakeResponse41CodecTest, RejectsPayloadShorterThanFixedHeader) {
  HandshakeResponse41 value;
  std::string error;
  EXPECT_FALSE(HandshakeResponse41Codec::Parse(std::vector<std::uint8_t>(31, 0),
                                               value, error));
  EXPECT_EQ(error, "handshake response too short for the fixed header");
}

TEST(HandshakeResponse41CodecTest, SslRequestIsTheFixedHeaderAlone) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_SSL | CLIENT_PLUGIN_AUTH;
  value.maxPacketSize = 0x01000000;
  value.characterSet = 45;
  value.username = "ignored";
  std::vector<std::uint8_t> out;
  HandshakeResponse41Codec::EncodeSslRequest(value, out);
  ASSERT_EQ(out.size(), 32u);
  EXPECT_EQ(ReadUint32LE(out, 0), value.capabilities);
  EXPECT_EQ(ReadUint32LE(out, 4), value.maxPacketSize);
  EXPECT_EQ(out[8], 45);
  EXPECT_TRUE(HandshakeResponse41Codec::IsSslRequest(out));
}

TEST(HandshakeResponse41CodecTest, OnlyTheSslBitMakesAnSslRequest) {
  HandshakeResponse41 value;
  value.capabilities = CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH;
  value.username = "root";
  std::vector<std::uint8_t> full;
  HandshakeResponse41Codec::Encode(value, full);
  EXPECT_FALSE(HandshakeResponse41Codec::IsSslRequest(full));

  value.capabilities |= CLIENT_SSL;
  std::vector<std::uint8_t> withBit;
  HandshakeResponse41Codec::Encode(value, withBit);
  EXPECT_TRUE(HandshakeResponse41Codec::IsSslRequest(withBit));

  const std::vector<std::uint8_t> tooShort(withBit.begin(),
                                           withBit.begin() + 31);
  EXPECT_FALSE(HandshakeResponse41Codec::IsSslRequest(tooShort));
}

}  // namespace
}  // namespace binlog_streamer
