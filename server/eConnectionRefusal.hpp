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

namespace binlog_streamer {

// Why a connection is over before its greeting. The listener decides it,
// but the connection still needs a channel and a peer address to send the
// ERR packet the way a server sends it.
enum class ConnectionRefusal {
  None,
  // ERR 1040, as a server already at max_connections answers.
  TooManyConnections,
  // ERR 3168: nothing is stored yet, so the relay knows neither the
  // source's version nor a single event to serve. A replica retries on
  // its own connect-retry timer, which is what waiting looks like here.
  NotReady,
};

}  // namespace binlog_streamer
