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
#include <optional>
#include <string>
#include "status/sStreamPoint.hpp"

namespace binlog_streamer {

struct SourceStatus {
  std::string address;  // host:port from source.yml
  bool connected = false;
  // Unix seconds: when the current connection was made, or when the last
  // one was lost; nullopt before the first attempt finished.
  std::optional<std::uint64_t> since;
  unsigned attempt = 0;  // the connection attempt under way, 0 when connected
  std::uint32_t serverId = 0;
  std::string serverUuid;
  std::string version;
  bool tls = false;
  std::string compression;  // "none", "zstd" or "zlib"; empty before connecting
  StreamPoint seen;         // the last event or heartbeat received
  // The source's own clock, unix seconds; known while connected.
  std::optional<std::uint64_t> clock;
  // 0 when the source said it has nothing more (heartbeat); otherwise the
  // source's clock minus the last event's timestamp. Unknown until an
  // event came through, and while not connected.
  std::optional<std::uint64_t> behindSeconds;
};

}  // namespace binlog_streamer
