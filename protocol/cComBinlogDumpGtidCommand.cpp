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

#include "protocol/cComBinlogDumpGtidCommand.hpp"

#include <utility>
#include "protocol/eCommand.hpp"

namespace binlog_streamer {

namespace {

std::uint32_t ReadUint32LE(std::span<const std::uint8_t> payload,
                           std::size_t pos) {
  return static_cast<std::uint32_t>(payload[pos]) |
         (static_cast<std::uint32_t>(payload[pos + 1]) << 8) |
         (static_cast<std::uint32_t>(payload[pos + 2]) << 16) |
         (static_cast<std::uint32_t>(payload[pos + 3]) << 24);
}

void WriteUint32LE(std::uint32_t value, std::vector<std::uint8_t> &out) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 24));
}

}  // namespace

std::vector<std::uint8_t> ComBinlogDumpGtidCommand::Encode(
    const BinlogDumpGtidCommand &value) {
  std::vector<std::uint8_t> payload;
  payload.reserve(1 + 2 + 4 + 4 + value.fileName.size() + 8 + 4 +
                  value.gtidSetEncoded.size());
  payload.push_back(static_cast<std::uint8_t>(Command::BinlogDumpGtid));

  payload.push_back(static_cast<std::uint8_t>(value.flags));
  payload.push_back(static_cast<std::uint8_t>(value.flags >> 8));

  WriteUint32LE(value.serverId, payload);

  WriteUint32LE(static_cast<std::uint32_t>(value.fileName.size()), payload);
  payload.insert(payload.end(), value.fileName.begin(), value.fileName.end());

  for (int i = 0; i < 8; ++i)
    payload.push_back(static_cast<std::uint8_t>(value.position >> (8 * i)));

  WriteUint32LE(static_cast<std::uint32_t>(value.gtidSetEncoded.size()),
                payload);
  payload.insert(payload.end(), value.gtidSetEncoded.begin(),
                 value.gtidSetEncoded.end());

  return payload;
}

bool ComBinlogDumpGtidCommand::Parse(std::span<const std::uint8_t> payload,
                                     BinlogDumpGtidCommand &value,
                                     std::string &error) {
  if (payload.empty() ||
      payload[0] != static_cast<std::uint8_t>(Command::BinlogDumpGtid)) {
    error = "not a COM_BINLOG_DUMP_GTID packet";
    return false;
  }
  constexpr std::size_t FIXED_HEAD_SIZE =
      1 + 2 + 4 + 4;  // command + flags + server_id + filename_length
  if (payload.size() < FIXED_HEAD_SIZE) {
    error = "COM_BINLOG_DUMP_GTID too short for its fixed fields";
    return false;
  }
  BinlogDumpGtidCommand parsed;
  parsed.flags = static_cast<std::uint16_t>(
      payload[1] | (static_cast<std::uint32_t>(payload[2]) << 8));
  parsed.serverId = ReadUint32LE(payload, 3);
  const std::size_t fileNameLength = ReadUint32LE(payload, 7);
  std::size_t pos = FIXED_HEAD_SIZE;
  if (payload.size() - pos < fileNameLength) {
    error = "COM_BINLOG_DUMP_GTID too short for its file name";
    return false;
  }
  parsed.fileName.assign(reinterpret_cast<const char *>(payload.data() + pos),
                         fileNameLength);
  pos += fileNameLength;

  if (payload.size() - pos < 8 + 4) {
    error =
        "COM_BINLOG_DUMP_GTID too short for the position and GTID set length";
    return false;
  }
  parsed.position = 0;
  for (std::size_t i = 0; i < 8; ++i)
    parsed.position |= static_cast<std::uint64_t>(payload[pos + i]) << (8 * i);
  pos += 8;

  const std::size_t gtidSetLength = ReadUint32LE(payload, pos);
  pos += 4;
  if (payload.size() - pos < gtidSetLength) {
    error = "COM_BINLOG_DUMP_GTID too short for its GTID set";
    return false;
  }
  const auto gtidSetBegin = payload.begin() + static_cast<std::ptrdiff_t>(pos);
  parsed.gtidSetEncoded.assign(
      gtidSetBegin, gtidSetBegin + static_cast<std::ptrdiff_t>(gtidSetLength));

  value = std::move(parsed);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
