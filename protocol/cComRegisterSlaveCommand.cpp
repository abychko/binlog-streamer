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

#include <utility>
#include "protocol/cLengthEncodedString.hpp"
#include "protocol/eCommand.hpp"

namespace binlog_streamer {

std::vector<std::uint8_t> ComRegisterSlaveCommand::Encode(
    const RegisterSlaveCommand &value) {
  std::vector<std::uint8_t> payload;
  payload.push_back(static_cast<std::uint8_t>(Command::RegisterSlave));
  payload.push_back(static_cast<std::uint8_t>(value.serverId));
  payload.push_back(static_cast<std::uint8_t>(value.serverId >> 8));
  payload.push_back(static_cast<std::uint8_t>(value.serverId >> 16));
  payload.push_back(static_cast<std::uint8_t>(value.serverId >> 24));
  LengthEncodedString::Encode(value.reportHost, payload);
  LengthEncodedString::Encode(value.reportUser, payload);
  LengthEncodedString::Encode(value.reportPassword, payload);
  payload.push_back(static_cast<std::uint8_t>(value.reportPort));
  payload.push_back(static_cast<std::uint8_t>(value.reportPort >> 8));
  payload.insert(
      payload.end(), 4,
      std::uint8_t{0});  // rpl_recovery_rank: removed field, always 0
  payload.insert(
      payload.end(), 4,
      std::uint8_t{0});  // master_id: the source fills this in, we send 0
  return payload;
}

namespace {

// One raw length byte plus that many bytes; false if payload ends first.
bool ReadShortString(std::span<const std::uint8_t> payload, std::size_t &pos,
                     std::string &value) {
  if (pos == payload.size()) return false;
  const std::size_t length = payload[pos];
  if (payload.size() - pos - 1 < length) return false;
  value.assign(reinterpret_cast<const char *>(payload.data() + pos + 1),
               length);
  pos += 1 + length;
  return true;
}

}  // namespace

bool ComRegisterSlaveCommand::Parse(std::span<const std::uint8_t> payload,
                                    RegisterSlaveCommand &value,
                                    std::string &error) {
  if (payload.empty() ||
      payload[0] != static_cast<std::uint8_t>(Command::RegisterSlave)) {
    error = "not a COM_REGISTER_SLAVE packet";
    return false;
  }
  if (payload.size() < 1 + 4) {
    error = "COM_REGISTER_SLAVE too short for the server id";
    return false;
  }
  RegisterSlaveCommand parsed;
  parsed.serverId = static_cast<std::uint32_t>(payload[1]) |
                    (static_cast<std::uint32_t>(payload[2]) << 8) |
                    (static_cast<std::uint32_t>(payload[3]) << 16) |
                    (static_cast<std::uint32_t>(payload[4]) << 24);
  std::size_t pos = 1 + 4;
  if (!ReadShortString(payload, pos, parsed.reportHost) ||
      !ReadShortString(payload, pos, parsed.reportUser) ||
      !ReadShortString(payload, pos, parsed.reportPassword)) {
    error = "COM_REGISTER_SLAVE too short for its report strings";
    return false;
  }
  constexpr std::size_t TAIL_SIZE =
      2 + 4 + 4;  // port + rpl_recovery_rank + master_id
  if (payload.size() - pos < TAIL_SIZE) {
    error = "COM_REGISTER_SLAVE too short for the port and trailing fields";
    return false;
  }
  parsed.reportPort = static_cast<std::uint16_t>(
      payload[pos] | (static_cast<std::uint32_t>(payload[pos + 1]) << 8));

  value = std::move(parsed);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
