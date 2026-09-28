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

#include "protocol/cComQueryCommand.hpp"

#include "protocol/eCommand.hpp"

namespace binlog_streamer {

std::vector<std::uint8_t> ComQueryCommand::Encode(std::string_view sql) {
  std::vector<std::uint8_t> payload;
  payload.reserve(1 + sql.size());
  payload.push_back(static_cast<std::uint8_t>(Command::Query));
  payload.insert(payload.end(), sql.begin(), sql.end());
  return payload;
}

bool ComQueryCommand::Parse(std::span<const std::uint8_t> payload,
                            std::string_view &sql, std::string &error) {
  if (payload.empty() ||
      payload[0] != static_cast<std::uint8_t>(Command::Query)) {
    error = "not a COM_QUERY packet";
    return false;
  }
  sql = std::string_view(reinterpret_cast<const char *>(payload.data() + 1),
                         payload.size() - 1);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
