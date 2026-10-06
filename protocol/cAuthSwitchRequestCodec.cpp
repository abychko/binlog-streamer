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

#include "protocol/cAuthSwitchRequestCodec.hpp"

namespace binlog_streamer {

bool AuthSwitchRequestCodec::IsAuthSwitchRequest(
    std::span<const std::uint8_t> payload) {
  // 0xFE is AuthSwitchRequest only during auth negotiation; in a result set it
  // means EOF (or OK with CLIENT_DEPRECATE_EOF).
  return !payload.empty() && payload[0] == 0xFE;
}

bool AuthSwitchRequestCodec::Parse(std::span<const std::uint8_t> payload,
                                   AuthSwitchRequest &value,
                                   std::string &error) {
  if (!IsAuthSwitchRequest(payload)) {
    error = "not an AuthSwitchRequest packet";
    return false;
  }
  std::size_t nameEnd = 1;
  while (nameEnd < payload.size() && payload[nameEnd] != 0) ++nameEnd;
  if (nameEnd == payload.size()) {
    error = "AuthSwitchRequest plugin name is not NUL-terminated";
    return false;
  }
  value.pluginName.assign(reinterpret_cast<const char *>(payload.data() + 1),
                          nameEnd - 1);
  value.pluginData.assign(
      payload.begin() + static_cast<std::ptrdiff_t>(nameEnd + 1),
      payload.end());
  error.clear();
  return true;
}

void AuthSwitchRequestCodec::Encode(const AuthSwitchRequest &value,
                                    std::vector<std::uint8_t> &out) {
  out.push_back(0xFE);
  out.insert(out.end(), value.pluginName.begin(), value.pluginName.end());
  out.push_back(0);
  out.insert(out.end(), value.pluginData.begin(), value.pluginData.end());
}

}  // namespace binlog_streamer
