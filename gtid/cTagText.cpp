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

#include "cTagText.hpp"

#include <cctype>
#include "gtid/hGtidLimits.hpp"

namespace binlog_streamer {

bool TagText::Parse(std::string_view text, std::string &value,
                    std::string &error) {
  error = "expected a tag matching [a-zA-Z_][a-zA-Z0-9_]{0,31}";
  if (text.empty() || text.size() > GTID_TAG_MAX_LENGTH) return false;
  const unsigned char first = static_cast<unsigned char>(text[0]);
  if (std::isalpha(first) == 0 && first != '_') return false;
  for (std::size_t i = 1; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (std::isalnum(c) == 0 && c != '_') return false;
  }
  std::string normalized(text);
  for (char &c : normalized)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  value = std::move(normalized);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
