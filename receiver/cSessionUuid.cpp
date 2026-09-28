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

#include "receiver/cSessionUuid.hpp"

#include <array>
#include <cstdint>
#include <random>

namespace binlog_streamer {
namespace {
char HexDigit(std::uint8_t nibble) {
  return nibble < 10 ? static_cast<char>('0' + nibble)
                     : static_cast<char>('a' + (nibble - 10));
}
}  // namespace

std::string SessionUuid::Generate() {
  std::array<std::uint8_t, 16> bytes{};
  std::random_device randomDevice;
  std::uniform_int_distribution<int> byteDistribution(0, 255);
  for (auto &byteValue : bytes)
    byteValue = static_cast<std::uint8_t>(byteDistribution(randomDevice));

  // RFC 4122 v4/variant 1: fix the reserved bits so this is a
  // well-formed random UUID, not 16 arbitrary bytes.
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
    text += HexDigit(static_cast<std::uint8_t>(bytes[i] >> 4));
    text += HexDigit(static_cast<std::uint8_t>(bytes[i] & 0x0F));
  }
  return text;
}

}  // namespace binlog_streamer
