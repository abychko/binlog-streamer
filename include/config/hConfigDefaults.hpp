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

#include <chrono>
#include <cstdint>

namespace binlog_streamer {

inline constexpr char DEFAULT_SETTINGS_PATH[] =
    "/etc/binlog-streamer/settings.yml";
inline constexpr char SOURCE_FILE_NAME[] = "source.yml";
inline constexpr char REPLICA_FILE_NAME[] = "replica.yml";
inline constexpr char DEFAULT_DATA_DIR[] = "/var/lib/binlog-streamer";
inline constexpr std::uint16_t DEFAULT_SOURCE_PORT = 3306;
inline constexpr char DEFAULT_LISTEN_ADDRESS[] = "0.0.0.0";
inline constexpr std::uint16_t DEFAULT_LISTEN_PORT = 3307;
inline constexpr std::uint16_t DEFAULT_HTTP_LISTEN_PORT = 8080;
inline constexpr char DEFAULT_HTTP_HTML_DIR[] = "/etc/binlog-streamer/html";
inline constexpr unsigned DEFAULT_MAX_CONNECTIONS = 64;
inline constexpr unsigned MAX_CONNECTIONS_LIMIT = 100000;
inline constexpr std::chrono::microseconds DEFAULT_SEND_LINGER{200};
inline constexpr std::chrono::microseconds MAX_SEND_LINGER =
    std::chrono::seconds(1);
inline constexpr char PROTECTED_FILE_OWNER[] = "root";
inline constexpr char PROTECTED_FILE_GROUP[] = "binlog-streamer";

}  // namespace binlog_streamer
