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
#include "receiver/hSessionDefaults.hpp"
#include "receiver/hStreamDefaults.hpp"

namespace binlog_streamer {

class StreamProgress;

struct StreamReaderOptions {
  // Continues the counter COM_BINLOG_DUMP_GTID left the connection on,
  // instead of resetting it - matches the server's per-connection
  // sequence for a running dump.
  std::uint8_t sequenceId = 0;

  // 0, or CHECKSUM_LENGTH for CRC32 - whichever
  // ReplicaSession::NegotiateChecksum found.
  std::size_t checksumLength = 0;

  // Idle-wait timeout, not a per-event budget - a heartbeat during a
  // quiet period resets it, matching a real replica.
  std::chrono::milliseconds readTimeout = REPLICA_NET_TIMEOUT;

  // False over a compressed connection: the ids of packets arriving
  // inside a frame are neither checked nor monotonic there
  // (sql-common/net_serv.cc rewrites them on flush), so the frame
  // counter under the transport is what stays verified.
  bool verifySequence = true;

  std::size_t bufferSize = STREAM_READ_BUFFER_SIZE;

  // Told where the stream stands after every event; not owned, may be
  // null.
  StreamProgress *progress = nullptr;
};

}  // namespace binlog_streamer
