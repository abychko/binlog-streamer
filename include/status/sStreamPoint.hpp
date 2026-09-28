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

namespace binlog_streamer {

// Where a stream of events stands: the source's as received, a replica's
// as sent. timestamp is the header timestamp of the last event, on the
// source's clock; 0 until an event passed.
struct StreamPoint {
  std::string file;
  std::uint64_t position = 0;
  std::uint32_t timestamp = 0;
  // Nothing more to pass at the moment: a heartbeat came from the source,
  // or the sender waits for new events.
  bool idle = false;
  bool operator==(const StreamPoint &) const = default;
};

}  // namespace binlog_streamer
