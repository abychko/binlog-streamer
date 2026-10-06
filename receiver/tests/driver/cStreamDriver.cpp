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

#include "cStreamDriver.hpp"

#include "cFileEventSink.hpp"

#include "binlog/hEventLimits.hpp"
#include "gtid/cGtidSet.hpp"
#include "net/cTcpTransport.hpp"
#include "receiver/cEventStreamReader.hpp"
#include "receiver/cReplicaSession.hpp"
#include "receiver/cSessionUuid.hpp"

#include <utility>

namespace binlog_streamer::test {

StreamDriver::StreamDriver(DriverOptions options,
                           const std::atomic<bool> *stopRequested)
    : m_options(std::move(options)), m_stopRequested(stopRequested) {}

DriverResult StreamDriver::Run(std::ostream &sinkStream) {
  DriverResult result;

  GtidSet startSet;
  if (!m_options.startGtidSetText.empty()) {
    std::string parseError;
    if (!startSet.AddFromText(m_options.startGtidSetText, parseError)) {
      result.message = "parsing --start-gtid-set: " + parseError;
      return result;
    }
  }

  TcpTransport transport(m_stopRequested);
  ReplicaSessionOptions sessionOptions;
  sessionOptions.registerAsReplica = false;
  sessionOptions.heartbeatPeriod = m_options.heartbeatPeriod;
  const std::string replicaUuid = SessionUuid::Generate();
  ReplicaSession session(transport, m_options.source, m_options.server,
                         replicaUuid, "bs-stream-driver",
                         BINLOG_STREAMER_STREAM_DRIVER_VERSION, sessionOptions);
  const SessionResult sessionResult = session.Run();
  if (sessionResult.outcome != SessionOutcome::Registered) {
    result.message = sessionResult.message;
    return result;
  }

  if (const auto dumpFailure = session.StartDump(startSet)) {
    result.message = dumpFailure->message;
    return result;
  }

  FileEventSink sink(sinkStream, m_options.stopAfterGtidEvents,
                     m_options.waitHeartbeats);
  StreamReaderOptions readerOptions;
  readerOptions.sequenceId = session.NextSequenceId();
  readerOptions.checksumLength =
      sessionResult.identity.checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  readerOptions.verifySequence = !session.compressed();
  EventStreamReader reader(session.transport(), sink, StreamPosition{},
                           readerOptions);
  const StreamResult streamResult = reader.Run();

  result.reason = streamResult.reason;
  result.hasWrittenAnyEvent = sink.HasWrittenAnyEvent();
  result.firstWrittenFileName = sink.FirstWrittenFileName();
  result.firstWrittenOffset = sink.FirstWrittenOffset();
  result.lastWrittenFileName = sink.LastWrittenFileName();
  result.lastWrittenEndOffset = sink.LastWrittenEndOffset();
  result.gtidEventCount = sink.GtidEventCount();
  result.heartbeatCount = sink.HeartbeatCount();
  result.events = streamResult.events;
  result.largestEventLength = streamResult.largestEventLength;
  result.largestEventSubPackets = streamResult.largestEventSubPackets;

  result.ok = sink.ReachedTargets();
  result.message = streamResult.message;
  return result;
}

}  // namespace binlog_streamer::test
