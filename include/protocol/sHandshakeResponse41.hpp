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

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace binlog_streamer {

// capabilities must include CLIENT_PROTOCOL_41 - only the 32-byte
// fixed-header form is handled.
struct HandshakeResponse41 {
  std::uint32_t capabilities = 0;
  std::uint32_t maxPacketSize = 0;
  std::uint8_t characterSet = 0;
  std::string username;
  std::vector<std::uint8_t> authResponse;
  std::string database;
  std::string authPluginName;
  std::vector<std::pair<std::string, std::string>> connectionAttributes;
  std::uint8_t zstdCompressionLevel =
      0;  // present only if capabilities has
          // CLIENT_ZSTD_COMPRESSION_ALGORITHM; 0 when absent, which is
          // not a level a peer may ask for
};

}  // namespace binlog_streamer
