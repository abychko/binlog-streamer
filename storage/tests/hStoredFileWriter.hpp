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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "gtid/cGtidSet.hpp"

namespace binlog_streamer::test {

inline void WriteFreshFile(const std::filesystem::path &path,
                           std::uint32_t serverId, bool inUse,
                           const GtidSet &previous = GtidSet()) {
  const auto put32 = [](std::vector<std::uint8_t> &bytes, std::size_t at,
                        std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
      bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  };
  std::vector<std::uint8_t> fdeBody(57, 0x00);
  fdeBody[0] = 3;  // binlogVersion
  const std::string version = "5.5.62";
  for (std::size_t i = 0; i < version.size(); ++i)
    fdeBody[2 + i] = static_cast<std::uint8_t>(version[i]);
  fdeBody[56] = 19;  // commonHeaderLength

  const auto pgeBody = previous.Encode(/*skipTaggedGtids=*/false);

  std::vector<std::uint8_t> file{0xfe, 0x62, 0x69, 0x6e};

  const auto fdeEventLength = static_cast<std::uint32_t>(19 + fdeBody.size());
  std::vector<std::uint8_t> fdeHeader(19, 0x00);
  fdeHeader[4] = 15;  // FormatDescription
  put32(fdeHeader, 5, serverId);
  put32(fdeHeader, 9, fdeEventLength);
  put32(fdeHeader, 13, 4 + fdeEventLength);
  fdeHeader[17] = inUse ? 0x01 : 0x00;  // LOG_EVENT_BINLOG_IN_USE_F
  file.insert(file.end(), fdeHeader.begin(), fdeHeader.end());
  file.insert(file.end(), fdeBody.begin(), fdeBody.end());

  const auto pgeEventLength = static_cast<std::uint32_t>(19 + pgeBody.size());
  std::vector<std::uint8_t> pgeHeader(19, 0x00);
  pgeHeader[4] = 35;  // PreviousGtids
  put32(pgeHeader, 5, serverId);
  put32(pgeHeader, 9, pgeEventLength);
  put32(pgeHeader, 13, 4 + fdeEventLength + pgeEventLength);
  file.insert(file.end(), pgeHeader.begin(), pgeHeader.end());
  file.insert(file.end(), pgeBody.begin(), pgeBody.end());

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(file.data()),
            static_cast<std::streamsize>(file.size()));
}

}  // namespace binlog_streamer::test
