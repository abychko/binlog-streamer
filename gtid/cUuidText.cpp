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

#include "cUuidText.hpp"

#include <array>
#include "gtid/hGtidLimits.hpp"

namespace binlog_streamer {
namespace {

constexpr std::array<int, 5> SECTION_BYTES{4, 2, 2, 2, 6};

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool UuidText::Parse(std::string_view text, Uuid &value, std::string &error) {
  error = "expected a UUID of the form XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX";
  if (text.size() != UUID_TEXT_LENGTH) return false;
  Uuid parsed;
  std::size_t pos = 0;
  std::size_t byteIndex = 0;
  for (std::size_t section = 0; section < SECTION_BYTES.size(); ++section) {
    if (section > 0) {
      if (text[pos] != '-') return false;
      ++pos;
    }
    for (int i = 0; i < SECTION_BYTES[section]; ++i) {
      const int hi = HexDigit(text[pos]);
      const int lo = HexDigit(text[pos + 1]);
      if (hi < 0 || lo < 0) return false;
      parsed.bytes[byteIndex++] = static_cast<std::uint8_t>((hi << 4) | lo);
      pos += 2;
    }
  }
  value = parsed;
  error.clear();
  return true;
}

std::string UuidText::ToString(const Uuid &value) {
  static constexpr char HEX[] = "0123456789abcdef";
  std::string text;
  text.reserve(UUID_TEXT_LENGTH);
  std::size_t byteIndex = 0;
  for (std::size_t section = 0; section < SECTION_BYTES.size(); ++section) {
    if (section > 0) text.push_back('-');
    for (int i = 0; i < SECTION_BYTES[section]; ++i) {
      const std::uint8_t b = value.bytes[byteIndex++];
      text.push_back(HEX[b >> 4]);
      text.push_back(HEX[b & 0xF]);
    }
  }
  return text;
}

}  // namespace binlog_streamer
