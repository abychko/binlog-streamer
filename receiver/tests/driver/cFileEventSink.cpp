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

#include "cFileEventSink.hpp"

#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"

namespace binlog_streamer::test {
namespace {
bool IsGtidEvent(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(EventType::Gtid) ||
         type == static_cast<std::uint8_t>(EventType::AnonymousGtid) ||
         type == static_cast<std::uint8_t>(EventType::GtidTagged);
}

bool IsHeartbeat(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
         type == static_cast<std::uint8_t>(EventType::HeartbeatV2);
}

// Both sit at an offset that never aligns with a resumed dump's own range;
// FDE also never matches the source file's copy (IN_USE cleared on the
// wire, set on disk, sql/rpl_binlog_sender.cc).
bool IsAlwaysExcludedPreamble(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(EventType::FormatDescription) ||
         type == static_cast<std::uint8_t>(EventType::PreviousGtids);
}
}  // namespace

FileEventSink::FileEventSink(std::ostream &sinkStream,
                             std::uint64_t stopAfterGtidEvents,
                             std::uint64_t waitHeartbeats)
    : m_sinkStream(sinkStream),
      m_stopAfterGtidEvents(stopAfterGtidEvents),
      m_waitHeartbeats(waitHeartbeats) {}

bool FileEventSink::OnEventBegin(const EventHeader &header,
                                 const StreamPosition &position) {
  // Checked before this event is processed, not after: this lets the
  // driver stop exactly at a transaction boundary, not mid-write.
  const bool gtidTargetSet = m_stopAfterGtidEvents > 0;
  const bool heartbeatTargetSet = m_waitHeartbeats > 0;
  if ((gtidTargetSet || heartbeatTargetSet) &&
      (!gtidTargetSet || m_gtidEventCount >= m_stopAfterGtidEvents) &&
      (!heartbeatTargetSet || m_heartbeatCount >= m_waitHeartbeats)) {
    m_reachedTargets = true;
    return false;
  }

  if (IsGtidEvent(header.type)) ++m_gtidEventCount;
  if (IsHeartbeat(header.type)) ++m_heartbeatCount;

  m_writeCurrentEvent = (header.flags & EVENT_FLAG_ARTIFICIAL) == 0 &&
                        !IsHeartbeat(header.type) &&
                        !IsAlwaysExcludedPreamble(header.type);
  m_currentFileName = position.fileName;
  m_currentNextPosition = header.nextPosition;
  m_currentEventLength = header.eventLength;

  if (m_writeCurrentEvent && !m_hasFirstWritten) {
    m_firstWrittenFileName = position.fileName;
    // From the header's own fields, not this reader's position-tracking.
    m_firstWrittenOffset = header.nextPosition - header.eventLength;
    m_hasFirstWritten = true;
  }
  return true;
}

bool FileEventSink::OnEventBytes(std::span<const std::uint8_t> bytes) {
  if (!m_writeCurrentEvent) return true;
  m_sinkStream.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(
      m_sinkStream);  // false (stop) if the write itself failed, e.g. disk full
}

bool FileEventSink::OnEventEnd() {
  if (m_writeCurrentEvent) {
    m_lastWrittenFileName = m_currentFileName;
    m_lastWrittenEndOffset = m_currentNextPosition;
  }
  return true;
}

}  // namespace binlog_streamer::test
