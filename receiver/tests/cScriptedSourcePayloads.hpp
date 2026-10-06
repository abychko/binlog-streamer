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

#pragma once

#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cLengthEncodedString.hpp"
#include "protocol/hCapabilityFlags.hpp"
#include "protocol/hProtocolLimits.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer::test {

class ScriptedSourcePayloads {
 public:
  static std::array<std::uint8_t, SCRAMBLE_LENGTH> Scramble() {
    std::array<std::uint8_t, SCRAMBLE_LENGTH> scramble{};
    for (std::size_t i = 0; i < scramble.size(); ++i)
      scramble[i] = static_cast<std::uint8_t>(i + 1);
    return scramble;
  }

  // auth_plugin_data_len is fixed at 21 (8 + 13), as a real 8.4.11 server sends
  // it: a trailing zero byte past the scramble.
  static std::vector<std::uint8_t> Greeting(
      const std::string &serverVersion, const std::string &authPluginName,
      std::span<const std::uint8_t, SCRAMBLE_LENGTH> scramble,
      std::uint32_t extraCapabilities = 0) {
    std::vector<std::uint8_t> payload;
    payload.push_back(PROTOCOL_VERSION);
    payload.insert(payload.end(), serverVersion.begin(), serverVersion.end());
    payload.push_back(0);
    payload.insert(payload.end(), {0x01, 0x00, 0x00, 0x00});
    payload.insert(payload.end(), scramble.begin(),
                   scramble.begin() + AUTH_PLUGIN_DATA_PART_1_LENGTH);
    payload.push_back(0);

    const std::uint32_t capabilities =
        CLIENT_PROTOCOL_41 | CLIENT_PLUGIN_AUTH |
        CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | CLIENT_SESSION_TRACK |
        CLIENT_DEPRECATE_EOF | CLIENT_CONNECT_ATTRS | extraCapabilities;
    payload.push_back(static_cast<std::uint8_t>(capabilities));
    payload.push_back(static_cast<std::uint8_t>(capabilities >> 8));
    payload.push_back(0x21);
    payload.insert(payload.end(), {0, 0});
    payload.push_back(static_cast<std::uint8_t>(capabilities >> 16));
    payload.push_back(static_cast<std::uint8_t>(capabilities >> 24));
    payload.push_back(21);
    payload.insert(payload.end(), 10, std::uint8_t{0});

    payload.insert(payload.end(),
                   scramble.begin() + AUTH_PLUGIN_DATA_PART_1_LENGTH,
                   scramble.end());
    payload.push_back(0);

    payload.insert(payload.end(), authPluginName.begin(), authPluginName.end());
    payload.push_back(0);
    return payload;
  }

  static std::vector<std::uint8_t> Ok(bool eofHeader = false) {
    std::vector<std::uint8_t> payload;
    payload.push_back(eofHeader ? std::uint8_t{0xFE} : std::uint8_t{0x00});
    LengthEncodedInteger::Encode(0, payload);
    LengthEncodedInteger::Encode(0, payload);
    payload.insert(payload.end(), {0x02, 0x00});
    payload.insert(payload.end(), {0, 0});
    return payload;
  }

  static std::vector<std::uint8_t> Err(std::uint16_t code,
                                       const std::string &message,
                                       const std::string &sqlState = "HY000") {
    std::vector<std::uint8_t> payload;
    payload.push_back(0xFF);
    payload.push_back(static_cast<std::uint8_t>(code));
    payload.push_back(static_cast<std::uint8_t>(code >> 8));
    payload.push_back('#');
    payload.insert(payload.end(), sqlState.begin(), sqlState.end());
    payload.insert(payload.end(), message.begin(), message.end());
    return payload;
  }

  static std::vector<std::uint8_t> AuthMoreData(std::uint8_t signalByte) {
    return {0x01, signalByte};
  }

  static std::vector<std::uint8_t> AuthMoreData(
      std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> payload{0x01};
    payload.insert(payload.end(), data.begin(), data.end());
    return payload;
  }

  static std::vector<std::uint8_t> AuthSwitchRequest(
      const std::string &pluginName, std::span<const std::uint8_t> pluginData) {
    std::vector<std::uint8_t> payload;
    payload.push_back(0xFE);
    payload.insert(payload.end(), pluginName.begin(), pluginName.end());
    payload.push_back(0);
    payload.insert(payload.end(), pluginData.begin(), pluginData.end());
    return payload;
  }

  static std::vector<std::uint8_t> ColumnCount(std::uint64_t count) {
    std::vector<std::uint8_t> payload;
    LengthEncodedInteger::Encode(count, payload);
    return payload;
  }

  static std::vector<std::uint8_t> ColumnDefinition(const std::string &name) {
    std::vector<std::uint8_t> payload;
    LengthEncodedString::Encode("def", payload);
    LengthEncodedString::Encode("", payload);
    LengthEncodedString::Encode("", payload);
    LengthEncodedString::Encode("", payload);
    LengthEncodedString::Encode(name, payload);
    LengthEncodedString::Encode(name, payload);
    LengthEncodedInteger::Encode(0x0C, payload);
    payload.insert(payload.end(), {0x21, 0x00});
    payload.insert(payload.end(), {0, 0, 0, 0});
    payload.push_back(0xFD);  // VAR_STRING
    payload.insert(payload.end(), {0, 0});
    payload.push_back(0);
    return payload;
  }

  static std::vector<std::uint8_t> TextRow(std::optional<std::string> value) {
    std::vector<std::uint8_t> payload;
    if (!value) {
      payload.push_back(0xFB);  // SQL NULL marker
    } else {
      LengthEncodedString::Encode(*value, payload);
    }
    return payload;
  }
};

}  // namespace binlog_streamer::test
