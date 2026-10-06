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

#include "receiver/cDumpProbe.hpp"

#include "binlog/eEventType.hpp"
#include "binlog/hEventLimits.hpp"
#include "net/cTcpTransport.hpp"
#include "receiver/cEventStreamReader.hpp"
#include "receiver/cProbeStreamClassifier.hpp"
#include "receiver/cReplicaSession.hpp"

#include <utility>

namespace binlog_streamer {
namespace {

// The FDE header timestamp is the file's creation time.
class ProbeSink : public EventSink {
 public:
  std::optional<std::uint64_t> formatDescriptionTimestamp;

  bool OnEventBegin(const EventHeader &header,
                    const StreamPosition &) override {
    if (header.type == static_cast<std::uint8_t>(EventType::FormatDescription))
      formatDescriptionTimestamp = header.timestamp;
    return true;
  }
  bool OnEventBytes(std::span<const std::uint8_t>) override { return true; }
  bool OnEventEnd() override { return ++m_eventsSeen < 2; }

 private:
  int m_eventsSeen = 0;
};

SessionResult MakeFailure(std::string message) {
  SessionResult failure;
  failure.outcome = SessionOutcome::PermanentFailure;
  failure.message = std::move(message);
  return failure;
}

}  // namespace

DumpProbe::DumpProbe(const SourceSettings &source, const ServerSettings &server,
                     std::string replicaUuid, std::string relayName,
                     std::string relayVersion,
                     const std::atomic<bool> *stopRequested,
                     const WakeupPipe *wakeupPipe)
    : m_source(source),
      m_server(server),
      m_replicaUuid(std::move(replicaUuid)),
      m_relayName(std::move(relayName)),
      m_relayVersion(std::move(relayVersion)),
      m_stopRequested(stopRequested),
      m_wakeupPipe(wakeupPipe) {}

ProbeResult DumpProbe::Probe(const GtidSet &startSet) {
  ProbeResult result;

  TcpTransport transport(m_stopRequested, m_wakeupPipe);
  ReplicaSessionOptions sessionOptions;
  sessionOptions.registerAsReplica = false;
  ReplicaSession session(transport, m_source, m_server, m_replicaUuid,
                         m_relayName, m_relayVersion, sessionOptions);
  const SessionResult sessionResult = session.Run();
  if (sessionResult.outcome != SessionOutcome::Registered) {
    result.failure = sessionResult;
    return result;
  }

  if (const auto dumpFailure = session.StartDump(startSet)) {
    result.failure = *dumpFailure;
    return result;
  }

  ProbeSink sink;
  StreamReaderOptions readerOptions;
  readerOptions.sequenceId = session.NextSequenceId();
  readerOptions.checksumLength =
      sessionResult.identity.checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  readerOptions.verifySequence = !session.compressed();
  EventStreamReader reader(session.transport(), sink, StreamPosition{},
                           readerOptions);
  const StreamResult streamResult = reader.Run();

  if (streamResult.reason != StreamEndReason::StoppedBySink ||
      !sink.formatDescriptionTimestamp.has_value()) {
    result.failure = ProbeStreamClassifier::Classify(streamResult);
    return result;
  }

  result.ok = true;
  result.fileName = streamResult.lastPosition.fileName;
  result.position = streamResult.lastPosition.position;
  result.createdAt = *sink.formatDescriptionTimestamp;
  return result;
}

PreviousGtidsResult DumpProbe::PreviousGtidsText(std::string_view fileName) {
  PreviousGtidsResult result;

  TcpTransport transport(m_stopRequested, m_wakeupPipe);
  ReplicaSessionOptions sessionOptions;
  sessionOptions.registerAsReplica = false;
  ReplicaSession session(transport, m_source, m_server, m_replicaUuid,
                         m_relayName, m_relayVersion, sessionOptions);
  const SessionResult sessionResult = session.Run();
  if (sessionResult.outcome != SessionOutcome::Registered) {
    result.failure = sessionResult;
    return result;
  }

  // fileName comes from the source's own ROTATE events, never external input,
  // so interpolating it into SQL is injection-safe.
  const std::string sql =
      "SHOW BINLOG EVENTS IN '" + std::string(fileName) + "' LIMIT 1,1";
  const auto rowResult = session.QueryRow(sql, 6);
  if (!rowResult.ok) {
    result.failure = rowResult.failure;
    return result;
  }
  constexpr std::size_t INFO_COLUMN = 5;
  if (rowResult.columns.size() != 6 ||
      !rowResult.columns[INFO_COLUMN].has_value()) {
    // Not retryable: the file has no Previous_gtids row (it predates GTIDs or
    // is damaged).
    result.failure =
        MakeFailure("file '" + std::string(fileName) +
                    "' has no second event (no Previous_gtids row)");
    return result;
  }
  result.ok = true;
  result.text = *rowResult.columns[INFO_COLUMN];
  return result;
}

}  // namespace binlog_streamer
