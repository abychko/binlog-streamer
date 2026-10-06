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
#include <cstddef>
#include <cstdint>
#include "protocol/hCapabilityFlags.hpp"

namespace binlog_streamer {

inline constexpr std::uint32_t SERVER_CAPABILITIES =
    CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
    CLIENT_TRANSACTIONS | CLIENT_RESERVED2 | CLIENT_MULTI_RESULTS |
    CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_ATTRS |
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | CLIENT_DEPRECATE_EOF;

// The only auth plugin the login accepts; the account's password is known here
// in cleartext, so full authentication is unneeded.
inline constexpr char CACHING_SHA2_PASSWORD_PLUGIN_NAME[] =
    "caching_sha2_password";

// connect_timeout's default (sql/sys_vars.cc); applied to each login-phase
// operation, not as one deadline for the whole exchange.
inline constexpr std::chrono::milliseconds LOGIN_TIMEOUT{10'000};

// wait_timeout's default (NET_WAIT_TIMEOUT in include/mysql_com.h); bounds only
// waiting for a new command, COMMAND_READ_TIMEOUT bounds finishing one already
// arriving.
inline constexpr std::chrono::milliseconds COMMAND_WAIT_TIMEOUT{8 * 60 * 60 *
                                                                1000};

// net_read_timeout's default (NET_READ_TIMEOUT in include/mysql_com.h).
inline constexpr std::chrono::milliseconds COMMAND_READ_TIMEOUT{30'000};

// net_write_timeout's default (NET_WRITE_TIMEOUT in include/mysql_com.h).
inline constexpr std::chrono::milliseconds COMMAND_WRITE_TIMEOUT{60'000};

inline constexpr std::size_t INCOMING_PACKET_LIMIT = 16UL * 1024UL * 1024UL;

inline constexpr std::uint16_t SERVER_STATUS_AUTOCOMMIT = 2;

}  // namespace binlog_streamer
