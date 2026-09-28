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
#include <ostream>
#include <string>
#include "config/sServerSettings.hpp"
#include "config/sSourceSettings.hpp"
#include "receiver/eStreamEndReason.hpp"

namespace binlog_streamer::test {

struct DriverOptions {
  SourceSettings source;
  ServerSettings server;
  std::string startGtidSetText;  // empty means an empty GTID set (dump from the
                                 // earliest the source still has)
  std::uint64_t stopAfterGtidEvents =
      0;                             // 0 = no target (see cFileEventSink.hpp)
  std::uint64_t waitHeartbeats = 0;  // 0 = no target
  std::chrono::seconds heartbeatPeriod{30};
};

struct DriverResult {
  // FileEventSink's own stop condition was reached (StoppedBySink with
  // ReachedTargets() true), not the stream ending for any other reason.
  bool ok = false;
  std::string message;
  StreamEndReason reason = StreamEndReason::MalformedStream;

  bool hasWrittenAnyEvent = false;
  std::string firstWrittenFileName;
  std::uint64_t firstWrittenOffset = 0;
  std::string lastWrittenFileName;
  std::uint64_t lastWrittenEndOffset = 0;
  std::uint64_t gtidEventCount = 0;
  std::uint64_t heartbeatCount = 0;
  std::uint64_t events = 0;
  std::uint64_t largestEventLength = 0;
  std::uint64_t largestEventSubPackets = 0;
};

// Connects as an unregistered dump connection, like mysqlbinlog: registering
// as a replica could collide with a real one's server_id.
class StreamDriver {
 public:
  // stopRequested is polled like TcpTransport polls it on EINTR - it is
  // the caller's job (main.cpp) to make a signal actually arrive (e.g.
  // an alarm()) to interrupt an in-progress read.
  StreamDriver(DriverOptions options, const std::atomic<bool> *stopRequested);

  DriverResult Run(std::ostream &sinkStream);

 private:
  DriverOptions m_options;
  const std::atomic<bool> *m_stopRequested;
};

}  // namespace binlog_streamer::test
