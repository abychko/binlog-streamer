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

// Excludes artificial events, heartbeats and each dump's FDE/PREVIOUS_GTIDS
// preamble, so the output compares byte for byte with the source's binlog.

#include "receiver/iEventSink.hpp"

#include <cstdint>
#include <ostream>
#include <string>

namespace binlog_streamer::test {

class FileEventSink : public EventSink {
 public:
  // 0 means no target; a 0/0 pair never stops on its own, leaving --max-seconds
  // as the backstop.
  FileEventSink(std::ostream &sinkStream, std::uint64_t stopAfterGtidEvents,
                std::uint64_t waitHeartbeats);

  bool OnEventBegin(const EventHeader &header,
                    const StreamPosition &position) override;
  bool OnEventBytes(std::span<const std::uint8_t> bytes) override;
  bool OnEventEnd() override;

  bool HasWrittenAnyEvent() const { return m_hasFirstWritten; }
  const std::string &FirstWrittenFileName() const {
    return m_firstWrittenFileName;
  }
  std::uint64_t FirstWrittenOffset() const { return m_firstWrittenOffset; }
  const std::string &LastWrittenFileName() const {
    return m_lastWrittenFileName;
  }
  std::uint64_t LastWrittenEndOffset() const { return m_lastWrittenEndOffset; }
  std::uint64_t GtidEventCount() const { return m_gtidEventCount; }
  std::uint64_t HeartbeatCount() const { return m_heartbeatCount; }

  bool ReachedTargets() const { return m_reachedTargets; }

 private:
  std::ostream &m_sinkStream;
  std::uint64_t m_stopAfterGtidEvents;
  std::uint64_t m_waitHeartbeats;

  bool m_writeCurrentEvent = false;
  std::string m_currentFileName;
  std::uint32_t m_currentNextPosition = 0;
  std::uint32_t m_currentEventLength = 0;

  bool m_hasFirstWritten = false;
  std::string m_firstWrittenFileName;
  std::uint64_t m_firstWrittenOffset = 0;
  std::string m_lastWrittenFileName;
  std::uint64_t m_lastWrittenEndOffset = 0;

  std::uint64_t m_gtidEventCount = 0;
  std::uint64_t m_heartbeatCount = 0;
  bool m_reachedTargets = false;
};

}  // namespace binlog_streamer::test
