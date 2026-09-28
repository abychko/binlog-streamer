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

#include "protocol/cEofPacketCodec.hpp"

namespace binlog_streamer {

bool EofPacketCodec::IsEofPacket(std::span<const std::uint8_t> payload) {
  return !payload.empty() && payload[0] == 0xFE && payload.size() < 9;
}

bool EofPacketCodec::Parse(std::span<const std::uint8_t> payload,
                           EofPacket &value, std::string &error) {
  if (!IsEofPacket(payload)) {
    error = "not an EOF packet";
    return false;
  }
  if (payload.size() < 5) {
    error = "EOF packet too short for warnings and status flags";
    return false;
  }
  value.warnings = static_cast<std::uint16_t>(
      payload[1] | (static_cast<std::uint32_t>(payload[2]) << 8));
  value.statusFlags = static_cast<std::uint16_t>(
      payload[3] | (static_cast<std::uint32_t>(payload[4]) << 8));
  error.clear();
  return true;
}

void EofPacketCodec::Encode(const EofPacket &value,
                            std::vector<std::uint8_t> &out) {
  out.push_back(0xFE);
  out.push_back(static_cast<std::uint8_t>(value.warnings));
  out.push_back(static_cast<std::uint8_t>(value.warnings >> 8));
  out.push_back(static_cast<std::uint8_t>(value.statusFlags));
  out.push_back(static_cast<std::uint8_t>(value.statusFlags >> 8));
}

}  // namespace binlog_streamer
