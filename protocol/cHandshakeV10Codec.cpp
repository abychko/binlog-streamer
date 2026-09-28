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

#include "protocol/hCapabilityFlags.hpp"
#include "protocol/hProtocolLimits.hpp"

namespace binlog_streamer {

// Field layout and the two-part scramble reassembly mirror the reference
// client parser (sql-common/client.cc, csm_parse_handshake()). Only the
// modern, always-extended form MySQL 8.0+/Percona Server sends is accepted.
bool HandshakeV10Codec::Parse(std::span<const std::uint8_t> payload,
                              HandshakeV10 &value, std::string &error) {
  if (payload.size() < 1 + 1 + 4 + AUTH_PLUGIN_DATA_PART_1_LENGTH + 1) {
    error = "handshake packet too short";
    return false;
  }
  if (payload[0] != PROTOCOL_VERSION) {
    error = "unsupported handshake protocol version";
    return false;
  }

  std::size_t versionEnd = 1;
  while (versionEnd < payload.size() && payload[versionEnd] != 0) ++versionEnd;
  if (versionEnd == payload.size()) {
    error = "handshake server version is not NUL-terminated";
    return false;
  }
  const std::string serverVersion(
      reinterpret_cast<const char *>(payload.data() + 1), versionEnd - 1);

  std::size_t pos = versionEnd + 1;
  if (payload.size() - pos < 4) {
    error = "handshake packet too short for thread id";
    return false;
  }
  const std::uint32_t threadId =
      static_cast<std::uint32_t>(payload[pos]) |
      (static_cast<std::uint32_t>(payload[pos + 1]) << 8) |
      (static_cast<std::uint32_t>(payload[pos + 2]) << 16) |
      (static_cast<std::uint32_t>(payload[pos + 3]) << 24);
  pos += 4;

  if (payload.size() - pos < AUTH_PLUGIN_DATA_PART_1_LENGTH + 1) {
    error = "handshake packet too short for the first scramble part";
    return false;
  }
  const auto scramblePart1Begin =
      payload.begin() + static_cast<std::ptrdiff_t>(pos);
  std::vector<std::uint8_t> scramblePart1(
      scramblePart1Begin,
      scramblePart1Begin +
          static_cast<std::ptrdiff_t>(AUTH_PLUGIN_DATA_PART_1_LENGTH));
  pos += AUTH_PLUGIN_DATA_PART_1_LENGTH +
         1;  // + the 1-byte filler, always 0 and otherwise unused

  constexpr std::size_t EXTENDED_BLOCK_SIZE =
      18;  // capabilities_lower(2)+charset(1)+status(2)+capabilities_upper(2)+auth_data_len(1)+reserved(10)
  if (payload.size() - pos < EXTENDED_BLOCK_SIZE) {
    error = "handshake packet too short for the extended fields";
    return false;
  }
  const std::uint32_t capabilitiesLower =
      static_cast<std::uint32_t>(payload[pos]) |
      (static_cast<std::uint32_t>(payload[pos + 1]) << 8);
  const std::uint8_t characterSet = payload[pos + 2];
  const std::uint16_t statusFlags = static_cast<std::uint16_t>(
      payload[pos + 3] | (static_cast<std::uint32_t>(payload[pos + 4]) << 8));
  const std::uint32_t capabilitiesUpper =
      static_cast<std::uint32_t>(payload[pos + 5]) |
      (static_cast<std::uint32_t>(payload[pos + 6]) << 8);
  const std::uint8_t authPluginDataLength = payload[pos + 7];
  pos += EXTENDED_BLOCK_SIZE;

  const std::uint32_t capabilities =
      capabilitiesLower | (capabilitiesUpper << 16);

  if (authPluginDataLength < AUTH_PLUGIN_DATA_PART_1_LENGTH) {
    error = "handshake declares a scramble shorter than its own first part";
    return false;
  }
  const std::size_t part2Length =
      static_cast<std::size_t>(authPluginDataLength) -
      AUTH_PLUGIN_DATA_PART_1_LENGTH;
  if (payload.size() - pos < part2Length) {
    error = "handshake packet too short for the second scramble part";
    return false;
  }
  std::vector<std::uint8_t> authPluginData = std::move(scramblePart1);
  authPluginData.insert(
      authPluginData.end(), payload.begin() + static_cast<std::ptrdiff_t>(pos),
      payload.begin() + static_cast<std::ptrdiff_t>(pos + part2Length));
  pos += part2Length;

  std::string authPluginName;
  if ((capabilities & CLIENT_PLUGIN_AUTH) != 0) {
    std::size_t nameEnd = pos;
    while (nameEnd < payload.size() && payload[nameEnd] != 0) ++nameEnd;
    if (nameEnd == payload.size()) {
      error = "handshake auth plugin name is not NUL-terminated";
      return false;
    }
    authPluginName.assign(reinterpret_cast<const char *>(payload.data() + pos),
                          nameEnd - pos);
  }

  value.protocolVersion = payload[0];
  value.serverVersion = serverVersion;
  value.threadId = threadId;
  value.authPluginData = std::move(authPluginData);
  value.capabilities = capabilities;
  value.characterSet = characterSet;
  value.statusFlags = statusFlags;
  value.authPluginName = std::move(authPluginName);
  error.clear();
  return true;
}

// Field layout mirrors the server (sql/auth/sql_authentication.cc,
// send_server_handshake_packet()). Only the CLIENT_PLUGIN_AUTH form is
// built - the one every supported server sends.
bool HandshakeV10Codec::Encode(const HandshakeV10 &value,
                               std::vector<std::uint8_t> &out,
                               std::string &error) {
  if ((value.capabilities & CLIENT_PLUGIN_AUTH) == 0) {
    error = "handshake without CLIENT_PLUGIN_AUTH is not supported";
    return false;
  }
  if (value.authPluginData.size() < AUTH_PLUGIN_DATA_PART_1_LENGTH ||
      value.authPluginData.size() > 0xFF) {
    error = "handshake auth plugin data length is out of range";
    return false;
  }
  if (value.serverVersion.find('\0') != std::string::npos ||
      value.authPluginName.find('\0') != std::string::npos) {
    error = "handshake string field contains a NUL";
    return false;
  }

  out.push_back(value.protocolVersion);
  out.insert(out.end(), value.serverVersion.begin(), value.serverVersion.end());
  out.push_back(0);
  out.push_back(static_cast<std::uint8_t>(value.threadId));
  out.push_back(static_cast<std::uint8_t>(value.threadId >> 8));
  out.push_back(static_cast<std::uint8_t>(value.threadId >> 16));
  out.push_back(static_cast<std::uint8_t>(value.threadId >> 24));

  const auto part2Begin =
      value.authPluginData.begin() +
      static_cast<std::ptrdiff_t>(AUTH_PLUGIN_DATA_PART_1_LENGTH);
  out.insert(out.end(), value.authPluginData.begin(), part2Begin);
  out.push_back(0);  // filler

  out.push_back(static_cast<std::uint8_t>(value.capabilities));
  out.push_back(static_cast<std::uint8_t>(value.capabilities >> 8));
  out.push_back(value.characterSet);
  out.push_back(static_cast<std::uint8_t>(value.statusFlags));
  out.push_back(static_cast<std::uint8_t>(value.statusFlags >> 8));
  out.push_back(static_cast<std::uint8_t>(value.capabilities >> 16));
  out.push_back(static_cast<std::uint8_t>(value.capabilities >> 24));
  out.push_back(static_cast<std::uint8_t>(value.authPluginData.size()));
  out.insert(out.end(), 10, std::uint8_t{0});  // reserved

  out.insert(out.end(), part2Begin, value.authPluginData.end());
  out.insert(out.end(), value.authPluginName.begin(),
             value.authPluginName.end());
  out.push_back(0);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
