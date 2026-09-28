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

#include "cDurationParser.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>

namespace binlog_streamer {
bool DurationParser::Parse(std::string_view text, std::chrono::seconds &value,
                           std::string &error) {
  error = "expected a positive integer duration with suffix s, m, h or d";
  if (text.size() < 2) return false;
  const auto suffix = std::string_view("smhd").find(text.back());
  if (suffix == std::string_view::npos) return false;
  constexpr std::uint64_t multipliers[] = {1, 60, 3600, 86400};
  const auto number = text.substr(0, text.size() - 1);
  if (number.find_first_not_of("0123456789") != std::string_view::npos)
    return false;
  std::uint64_t parsed = 0;
  const auto [end, status] =
      std::from_chars(number.data(), number.data() + number.size(), parsed);
  const auto maximum = static_cast<std::uint64_t>(
      std::numeric_limits<std::chrono::seconds::rep>::max());
  if (status != std::errc{} || end != number.data() + number.size() ||
      parsed == 0 || parsed > maximum / multipliers[suffix])
    return false;
  value = std::chrono::seconds(
      static_cast<std::chrono::seconds::rep>(parsed * multipliers[suffix]));
  error.clear();
  return true;
}
bool DurationParser::ParseDelay(std::string_view text,
                                std::chrono::microseconds &value,
                                std::string &error) {
  error = "expected 0 or a non-negative integer with suffix us or ms";
  std::uint64_t multiplier = 1;
  std::string_view number = text;
  if (text != "0") {
    if (text.ends_with("ms"))
      multiplier = 1000;
    else if (!text.ends_with("us"))
      return false;
    number = text.substr(0, text.size() - 2);
  }
  if (number.find_first_not_of("0123456789") != std::string_view::npos)
    return false;
  std::uint64_t parsed = 0;
  const auto [end, status] =
      std::from_chars(number.data(), number.data() + number.size(), parsed);
  const auto maximum = static_cast<std::uint64_t>(
      std::numeric_limits<std::chrono::microseconds::rep>::max());
  if (status != std::errc{} || end != number.data() + number.size() ||
      parsed > maximum / multiplier)
    return false;
  value = std::chrono::microseconds(
      static_cast<std::chrono::microseconds::rep>(parsed * multiplier));
  error.clear();
  return true;
}
}  // namespace binlog_streamer
