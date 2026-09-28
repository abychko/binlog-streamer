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

#include "binlog/cBinlogFileName.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace binlog_streamer {
namespace {
constexpr std::size_t MINIMUM_DIGITS = 6;
}  // namespace

bool BinlogFileName::Parse(std::string_view text, std::string &basename,
                           std::uint64_t &number, std::string &error) {
  const std::size_t dot = text.rfind('.');
  if (dot == std::string_view::npos) {
    error =
        "expected '<basename>.<digits>', no '.' in '" + std::string(text) + "'";
    return false;
  }
  const std::string_view suffix = text.substr(dot + 1);
  if (suffix.size() < MINIMUM_DIGITS ||
      !std::all_of(suffix.begin(), suffix.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    error = "expected at least " + std::to_string(MINIMUM_DIGITS) +
            " digits after '.' in '" + std::string(text) + "'";
    return false;
  }
  std::uint64_t parsed = 0;
  const auto [ptr, status] =
      std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
  if (status != std::errc{} || ptr != suffix.data() + suffix.size()) {
    error = "malformed sequence number in '" + std::string(text) + "'";
    return false;
  }
  basename = std::string(text.substr(0, dot));
  number = parsed;
  return true;
}

std::string BinlogFileName::Format(std::string_view basename,
                                   std::uint64_t number) {
  std::string digits = std::to_string(number);
  if (digits.size() < MINIMUM_DIGITS)
    digits.insert(0, MINIMUM_DIGITS - digits.size(), '0');
  return std::string(basename) + "." + digits;
}

}  // namespace binlog_streamer
