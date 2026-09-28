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

#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cLengthEncodedString.hpp"
#include "protocol/hCapabilityFlags.hpp"

namespace binlog_streamer {

namespace {

constexpr std::size_t FIXED_HEADER_SIZE =
    32;  // capabilities(4)+max_packet_size(4)+charset(1)+reserved(23)

void EncodeFixedHeader(const HandshakeResponse41 &value,
                       std::vector<std::uint8_t> &out) {
  out.push_back(static_cast<std::uint8_t>(value.capabilities));
  out.push_back(static_cast<std::uint8_t>(value.capabilities >> 8));
  out.push_back(static_cast<std::uint8_t>(value.capabilities >> 16));
  out.push_back(static_cast<std::uint8_t>(value.capabilities >> 24));
  out.push_back(static_cast<std::uint8_t>(value.maxPacketSize));
  out.push_back(static_cast<std::uint8_t>(value.maxPacketSize >> 8));
  out.push_back(static_cast<std::uint8_t>(value.maxPacketSize >> 16));
  out.push_back(static_cast<std::uint8_t>(value.maxPacketSize >> 24));
  out.push_back(value.characterSet);
  out.insert(out.end(), 23, std::uint8_t{0});  // reserved, must be zero
}

}  // namespace

void HandshakeResponse41Codec::EncodeSslRequest(
    const HandshakeResponse41 &value, std::vector<std::uint8_t> &out) {
  EncodeFixedHeader(value, out);
}

bool HandshakeResponse41Codec::IsSslRequest(
    std::span<const std::uint8_t> payload) {
  // The server reads the bit before anything else, whatever follows
  // (sql/auth/sql_authentication.cc, parse_client_handshake_packet).
  return payload.size() >= FIXED_HEADER_SIZE &&
         (static_cast<std::uint32_t>(payload[1]) & (CLIENT_SSL >> 8)) != 0;
}

// Field layout mirrors the reference client (sql-common/client.cc).
// auth response is always LengthEncodedString - for scrambles this relay
// sends (<= 32 bytes), byte-identical to lenenc and legacy forms.
void HandshakeResponse41Codec::Encode(const HandshakeResponse41 &value,
                                      std::vector<std::uint8_t> &out) {
  EncodeFixedHeader(value, out);

  out.insert(out.end(), value.username.begin(), value.username.end());
  out.push_back(0);

  LengthEncodedInteger::Encode(value.authResponse.size(), out);
  out.insert(out.end(), value.authResponse.begin(), value.authResponse.end());

  if ((value.capabilities & CLIENT_PLUGIN_AUTH) != 0) {
    out.insert(out.end(), value.authPluginName.begin(),
               value.authPluginName.end());
    out.push_back(0);
  }

  if ((value.capabilities & CLIENT_CONNECT_ATTRS) != 0) {
    std::vector<std::uint8_t> attrs;
    for (const auto &[key, attrValue] : value.connectionAttributes) {
      LengthEncodedString::Encode(key, attrs);
      LengthEncodedString::Encode(attrValue, attrs);
    }
    LengthEncodedInteger::Encode(attrs.size(), out);
    out.insert(out.end(), attrs.begin(), attrs.end());
  }

  // Last byte of the packet, after the attributes; a peer reads it only
  // when it advertised the bit itself (sql-common/client.cc).
  if ((value.capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM) != 0)
    out.push_back(value.zstdCompressionLevel);
}

namespace {

bool ReadNulTerminated(std::span<const std::uint8_t> payload, std::size_t &pos,
                       std::string &value) {
  std::size_t end = pos;
  while (end < payload.size() && payload[end] != 0) ++end;
  if (end == payload.size()) return false;
  value.assign(reinterpret_cast<const char *>(payload.data() + pos), end - pos);
  pos = end + 1;
  return true;
}

}  // namespace

// Field order and the two auth-response length forms mirror the server
// parser (sql/auth/sql_authentication.cc): without
// CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA it's one raw byte, otherwise a
// LengthEncodedInteger prefix; unread trailing bytes match the server too.
bool HandshakeResponse41Codec::Parse(std::span<const std::uint8_t> payload,
                                     HandshakeResponse41 &value,
                                     std::string &error) {
  if (payload.size() < FIXED_HEADER_SIZE) {
    error = "handshake response too short for the fixed header";
    return false;
  }
  HandshakeResponse41 parsed;
  parsed.capabilities = static_cast<std::uint32_t>(payload[0]) |
                        (static_cast<std::uint32_t>(payload[1]) << 8) |
                        (static_cast<std::uint32_t>(payload[2]) << 16) |
                        (static_cast<std::uint32_t>(payload[3]) << 24);
  parsed.maxPacketSize = static_cast<std::uint32_t>(payload[4]) |
                         (static_cast<std::uint32_t>(payload[5]) << 8) |
                         (static_cast<std::uint32_t>(payload[6]) << 16) |
                         (static_cast<std::uint32_t>(payload[7]) << 24);
  parsed.characterSet = payload[8];
  if ((parsed.capabilities & CLIENT_PROTOCOL_41) == 0) {
    error = "handshake response is not in the CLIENT_PROTOCOL_41 form";
    return false;
  }

  std::size_t pos = FIXED_HEADER_SIZE;
  if (!ReadNulTerminated(payload, pos, parsed.username)) {
    error = "handshake response user name is not NUL-terminated";
    return false;
  }

  std::uint64_t authResponseLength = 0;
  if ((parsed.capabilities & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) != 0) {
    bool isNull = false;
    const std::size_t consumed = LengthEncodedInteger::Decode(
        payload.subspan(pos), authResponseLength, isNull);
    if (consumed == 0 || isNull) {
      error = "handshake response: malformed auth response length";
      return false;
    }
    pos += consumed;
  } else {
    if (pos == payload.size()) {
      error = "handshake response too short for the auth response length";
      return false;
    }
    authResponseLength = payload[pos];
    ++pos;
  }
  if (payload.size() - pos < authResponseLength) {
    error = "handshake response too short for its auth response";
    return false;
  }
  const auto authResponseBegin =
      payload.begin() + static_cast<std::ptrdiff_t>(pos);
  parsed.authResponse.assign(
      authResponseBegin,
      authResponseBegin + static_cast<std::ptrdiff_t>(authResponseLength));
  pos += static_cast<std::size_t>(authResponseLength);

  if ((parsed.capabilities & CLIENT_CONNECT_WITH_DB) != 0 &&
      !ReadNulTerminated(payload, pos, parsed.database)) {
    error = "handshake response database name is not NUL-terminated";
    return false;
  }

  if ((parsed.capabilities & CLIENT_PLUGIN_AUTH) != 0 &&
      !ReadNulTerminated(payload, pos, parsed.authPluginName)) {
    error = "handshake response auth plugin name is not NUL-terminated";
    return false;
  }

  if ((parsed.capabilities & CLIENT_CONNECT_ATTRS) != 0) {
    std::uint64_t attrsLength = 0;
    bool isNull = false;
    const std::size_t consumed =
        LengthEncodedInteger::Decode(payload.subspan(pos), attrsLength, isNull);
    if (consumed == 0 || isNull) {
      error = "handshake response: malformed connection attributes length";
      return false;
    }
    pos += consumed;
    if (payload.size() - pos < attrsLength) {
      error = "handshake response too short for its connection attributes";
      return false;
    }
    std::span<const std::uint8_t> attrs =
        payload.subspan(pos, static_cast<std::size_t>(attrsLength));
    while (!attrs.empty()) {
      std::string key;
      const std::size_t keyConsumed = LengthEncodedString::Decode(attrs, key);
      if (keyConsumed == 0) {
        error = "handshake response: malformed connection attribute key";
        return false;
      }
      attrs = attrs.subspan(keyConsumed);
      std::string attrValue;
      const std::size_t valueConsumed =
          LengthEncodedString::Decode(attrs, attrValue);
      if (valueConsumed == 0) {
        error = "handshake response: malformed connection attribute value";
        return false;
      }
      attrs = attrs.subspan(valueConsumed);
      parsed.connectionAttributes.emplace_back(std::move(key),
                                               std::move(attrValue));
    }
    pos += static_cast<std::size_t>(attrsLength);
  }

  // Left at 0 when the client asked for zstd but sent no level: the
  // caller rejects the connection, as a server does for any level
  // outside 1...22 (sql/auth/sql_authentication.cc).
  if ((parsed.capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM) != 0 &&
      pos < payload.size())
    parsed.zstdCompressionLevel = payload[pos];

  value = std::move(parsed);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
