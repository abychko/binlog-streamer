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
  std::uint32_t serverId = 0;
  // Whether the replica negotiated CRC32: decides if a synthesized event ends
  // with a checksum until the first Format_description, after which that file's
  // algorithm decides, as on a source; stored events keep what they have.
  bool checksum = true;
  // BINLOG_DUMP_NON_BLOCK, or a replica with server_id 0: end with EOF at the
  // end of the stored history instead of waiting.
  bool nonBlocking = false;
  std::chrono::nanoseconds heartbeatPeriod{0};
  bool heartbeatV2 = false;
  const std::atomic<bool> *stopRequested = nullptr;
  std::shared_ptr<std::atomic<bool>> superseded;
  StreamProgress *progress = nullptr;
  std::chrono::microseconds sendLinger{0};
};

}  // namespace binlog_streamer
