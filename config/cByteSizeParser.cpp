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

#include "cByteSizeParser.hpp"

#include <charconv>
#include <limits>
#include <system_error>

namespace binlog_streamer {
bool ByteSizeParser::Parse(std::string_view text, std::uint64_t &value,
                           std::string &error) {
  error = "expected an integer byte size with suffix K, M, G, T, P or E";
  if (text.size() < 2) return false;
  const auto suffix = std::string_view("kKmMgGtTpPeE").find(text.back());
  if (suffix == std::string_view::npos) return false;
  const auto number = text.substr(0, text.size() - 1);
  if (number.find_first_not_of("0123456789") != std::string_view::npos)
    return false;
  std::uint64_t parsed = 0;
  const auto [end, status] =
      std::from_chars(number.data(), number.data() + number.size(), parsed);
  const std::uint64_t multiplier = std::uint64_t{1} << (10 * (suffix / 2 + 1));
  if (status != std::errc{} || end != number.data() + number.size() ||
      parsed > std::numeric_limits<std::uint64_t>::max() / multiplier)
    return false;
  value = parsed * multiplier;
  error.clear();
  return true;
}
}  // namespace binlog_streamer
