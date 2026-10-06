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

#include "protocol/cLengthEncodedInteger.hpp"

namespace binlog_streamer {

void LengthEncodedInteger::Encode(std::uint64_t value,
                                  std::vector<std::uint8_t> &out) {
  if (value < 251) {
    out.push_back(static_cast<std::uint8_t>(value));
    return;
  }
  if (value < 65536) {
    out.push_back(0xFC);
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    return;
  }
  if (value < 16777216) {
    out.push_back(0xFD);
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    return;
  }
  out.push_back(0xFE);
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::size_t LengthEncodedInteger::Decode(std::span<const std::uint8_t> data,
                                         std::uint64_t &value, bool &isNull) {
  isNull = false;
  if (data.empty()) return 0;
  const std::uint8_t first = data[0];
  if (first < 0xFB) {
    value = first;
    return 1;
  }
  if (first == 0xFB) {
    isNull = true;
    return 1;
  }
  if (first == 0xFC) {
    if (data.size() < 3) return 0;
    value = static_cast<std::uint64_t>(data[1]) |
            (static_cast<std::uint64_t>(data[2]) << 8);
    return 3;
  }
  if (first == 0xFD) {
    if (data.size() < 4) return 0;
    value = static_cast<std::uint64_t>(data[1]) |
            (static_cast<std::uint64_t>(data[2]) << 8) |
            (static_cast<std::uint64_t>(data[3]) << 16);
    return 4;
  }
  if (data.size() < 9) return 0;
  std::uint64_t decoded = 0;
  for (int i = 0; i < 8; ++i)
    decoded |= static_cast<std::uint64_t>(data[1 + i]) << (8 * i);
  value = decoded;
  return 9;
}

}  // namespace binlog_streamer
