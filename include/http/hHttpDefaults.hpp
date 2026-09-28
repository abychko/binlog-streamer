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

namespace binlog_streamer {

// A request head longer than this is refused with 431: a status request
// has a request line and a few headers, nothing a proxy adds comes close.
inline constexpr std::size_t MAX_HTTP_REQUEST_HEAD = 8 * 1024;

// A client that connected and sends nothing holds one connection thread
// this long, then gets 408; a proxy or a monitoring poll sends its
// request at once.
inline constexpr std::chrono::milliseconds HTTP_READ_TIMEOUT{5'000};
inline constexpr std::chrono::milliseconds HTTP_WRITE_TIMEOUT{5'000};

}  // namespace binlog_streamer
