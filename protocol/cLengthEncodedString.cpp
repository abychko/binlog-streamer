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

#include "protocol/cLengthEncodedString.hpp"

#include "protocol/cLengthEncodedInteger.hpp"

namespace binlog_streamer {

void LengthEncodedString::Encode(const std::string &value,
                                 std::vector<std::uint8_t> &out) {
  LengthEncodedInteger::Encode(value.size(), out);
  out.insert(out.end(), value.begin(), value.end());
}

std::size_t LengthEncodedString::Decode(std::span<const std::uint8_t> data,
                                        std::string &value) {
  std::uint64_t length = 0;
  bool isNull = false;
  const std::size_t prefixBytes =
      LengthEncodedInteger::Decode(data, length, isNull);
  if (prefixBytes == 0 || isNull) return 0;
  if (data.size() - prefixBytes < length) return 0;
  value.assign(reinterpret_cast<const char *>(data.data() + prefixBytes),
               length);
  return prefixBytes + length;
}

}  // namespace binlog_streamer
