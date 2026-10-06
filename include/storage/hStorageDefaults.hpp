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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace binlog_streamer {

inline constexpr std::array<std::uint8_t, 4> BINLOG_MAGIC{0xfe, 0x62, 0x69,
                                                          0x6e};

inline constexpr std::size_t WRITE_BUFFER_SIZE = 64 * 1024;

// Not read by cBinlogFileWriter; the driving code counts events and calls
// Sync().
inline constexpr std::uint64_t SYNC_EVENT_PERIOD = 10000;

inline constexpr std::chrono::milliseconds WRITE_COALESCE{5};

// Fixed, unlike MySQL's <log-bin-basename>.index: it only holds plain file
// names.
inline constexpr std::string_view INDEX_FILE_NAME = "binlog.index";

inline constexpr std::string_view SERVER_UUID_FILE_NAME = "auto.cnf";
inline constexpr std::array<std::string_view, 4> TLS_FILE_NAMES{
    "ca.pem", "ca-key.pem", "server-cert.pem", "server-key.pem"};

// 4-byte magic plus 17 bytes: the Common-Header flags field, where
// LOG_EVENT_BINLOG_IN_USE_F lives.
inline constexpr std::uint64_t IN_USE_FLAG_OFFSET = 21;

inline constexpr std::size_t MAX_BUFFERED_EVENT_SIZE = 16UL * 1024UL * 1024UL;

}  // namespace binlog_streamer
