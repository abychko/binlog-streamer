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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace binlog_streamer {

class StreamProgress;

struct DumpSenderOptions {
  std::uint32_t serverId =
      0;  // the relay's own, carried by the events it synthesizes
  // Whether the replica negotiated CRC32 checksums: decides if a synthesized
  // event ends with one until the first Format_description is sent, after
  // which that file's algorithm decides, as on a source; stored events keep
  // what they have.
  bool checksum = true;
  // BINLOG_DUMP_NON_BLOCK, or a replica with server_id 0: end with EOF at
  // the end of the stored history instead of waiting for more.
  bool nonBlocking = false;
  // What the replica asked for with @source_heartbeat_period; zero means
  // no heartbeats, as it does on a source.
  std::chrono::nanoseconds heartbeatPeriod{0};
  bool heartbeatV2 =
      false;  // BINLOG_DUMP_USE_HEARTBEAT_EVENT_V2: positions past 4 GiB fit
  const std::atomic<bool> *stopRequested = nullptr;
  std::shared_ptr<std::atomic<bool>> superseded;
  // Told where the dump stands after every event; not owned, may be
  // null.
  StreamProgress *progress = nullptr;
  // Once caught up, what is queued waits up to this long for the events
  // that follow, so they go in one write; zero sends at once.
  std::chrono::microseconds sendLinger{0};
};

}  // namespace binlog_streamer
