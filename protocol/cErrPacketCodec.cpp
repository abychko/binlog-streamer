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

#include "protocol/cErrPacketCodec.hpp"

#include <string_view>

namespace binlog_streamer {

bool ErrPacketCodec::IsErrPacket(std::span<const std::uint8_t> payload) {
  return !payload.empty() && payload[0] == 0xFF;
}

// error_code(2), then '#' + sqlstate(5) if CLIENT_PROTOCOL_41 was negotiated,
// then the message. A server refusing the connection before its greeting sends
// no marker (sql-common/client.cc, cli_safe_read).
bool ErrPacketCodec::Parse(std::span<const std::uint8_t> payload,
                           ErrPacket &value, std::string &error) {
  if (!IsErrPacket(payload)) {
    error = "not an ERR packet";
    return false;
  }
  constexpr std::size_t SQL_STATE_LENGTH = 5;
  constexpr std::size_t CODE_END = 1 + 2;
  constexpr std::size_t STATE_END = CODE_END + 1 + SQL_STATE_LENGTH;
  if (payload.size() < CODE_END) {
    error = "ERR packet too short";
    return false;
  }
  value.errorCode = static_cast<std::uint16_t>(
      payload[1] | (static_cast<std::uint32_t>(payload[2]) << 8));
  std::size_t messageStart = CODE_END;
  if (payload.size() >= STATE_END && payload[CODE_END] == '#') {
    value.sqlState.assign(
        reinterpret_cast<const char *>(payload.data() + CODE_END + 1),
        SQL_STATE_LENGTH);
    messageStart = STATE_END;
  } else {
    value.sqlState.clear();
  }
  value.message.assign(
      reinterpret_cast<const char *>(payload.data() + messageStart),
      payload.size() - messageStart);
  error.clear();
  return true;
}

void ErrPacketCodec::Encode(const ErrPacket &value,
                            std::vector<std::uint8_t> &out) {
  out.push_back(0xFF);
  out.push_back(static_cast<std::uint8_t>(value.errorCode));
  out.push_back(static_cast<std::uint8_t>(value.errorCode >> 8));
  if (!value.sqlState.empty()) {
    constexpr std::size_t SQL_STATE_LENGTH = 5;
    const std::string_view sqlState = value.sqlState.size() == SQL_STATE_LENGTH
                                          ? std::string_view(value.sqlState)
                                          : std::string_view("HY000");
    out.push_back('#');
    out.insert(out.end(), sqlState.begin(), sqlState.end());
  }
  out.insert(out.end(), value.message.begin(), value.message.end());
}

}  // namespace binlog_streamer
