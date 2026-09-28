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

#include "protocol/cTextRowCodec.hpp"

#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cLengthEncodedString.hpp"

namespace binlog_streamer {

bool TextRowCodec::Parse(std::span<const std::uint8_t> payload,
                         std::size_t columnCount, TextRow &value,
                         std::string &error) {
  TextRow parsed;
  parsed.columns.reserve(columnCount);
  std::size_t pos = 0;
  for (std::size_t i = 0; i < columnCount; ++i) {
    std::uint64_t length = 0;
    bool isNull = false;
    const std::size_t prefixBytes =
        LengthEncodedInteger::Decode(payload.subspan(pos), length, isNull);
    if (prefixBytes == 0) {
      error = "text row: malformed column length";
      return false;
    }
    pos += prefixBytes;
    if (isNull) {
      parsed.columns.emplace_back(std::nullopt);
      continue;
    }
    if (payload.size() - pos < length) {
      error = "text row: column value shorter than its declared length";
      return false;
    }
    parsed.columns.emplace_back(std::string(
        reinterpret_cast<const char *>(payload.data() + pos), length));
    pos += length;
  }
  value = std::move(parsed);
  error.clear();
  return true;
}

void TextRowCodec::Encode(const TextRow &value,
                          std::vector<std::uint8_t> &out) {
  constexpr std::uint8_t NULL_MARKER = 0xFB;
  for (const std::optional<std::string> &column : value.columns) {
    if (column.has_value()) {
      LengthEncodedString::Encode(*column, out);
    } else {
      out.push_back(NULL_MARKER);
    }
  }
}

}  // namespace binlog_streamer
