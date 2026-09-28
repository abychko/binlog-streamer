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

#include "binlog/cCrc32.hpp"

#include <array>

namespace binlog_streamer {

namespace {

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit)
      value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
    table[i] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> TABLE = MakeTable();

}  // namespace

std::uint32_t Crc32::Compute(std::span<const std::uint8_t> data) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::uint8_t byte : data)
    crc = TABLE[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace binlog_streamer
