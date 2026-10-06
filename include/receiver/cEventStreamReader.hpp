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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "net/iTransport.hpp"
#include "receiver/iEventSink.hpp"
#include "receiver/sStreamReaderOptions.hpp"
#include "receiver/sStreamResult.hpp"

namespace binlog_streamer {

class EventStreamReader {
 public:
  EventStreamReader(Transport &transport, EventSink &sink,
                    StreamPosition startPosition,
                    StreamReaderOptions options = {});

  StreamResult Run();

 private:
  enum class FillOutcome { Ready, TimedOut, Interrupted, Closed, Failed };

  FillOutcome EnsureBytes(std::size_t count, std::string &error);
  FillOutcome FillMore(std::string &error);
  void Compact();
  StreamResult MakeResult(StreamEndReason reason, std::string message) const;
  StreamResult TerminalFromFill(FillOutcome outcome,
                                const std::string &error) const;

  bool ReadSubPacketHeader(std::size_t &subPacketLength,
                           StreamResult &terminalResult);

  bool ConsumeEvent(std::size_t firstSubPacketPayloadLength,
                    bool firstSubPacketWasFull, StreamResult &terminalResult);

  // Always ends the stream: terminalResult is set unconditionally.
  void ConsumeErrPacket(std::size_t firstSubPacketPayloadLength,
                        bool firstSubPacketWasFull,
                        StreamResult &terminalResult);

  Transport &m_transport;
  EventSink &m_sink;
  StreamReaderOptions m_options;

  std::vector<std::uint8_t> m_buffer;
  std::size_t m_dataStart = 0;
  std::size_t m_dataEnd = 0;
  std::uint8_t m_sequenceId = 0;

  StreamPosition m_position;
  StreamPosition m_firstPosition;
  bool m_hasFirstPosition = false;
  std::uint64_t m_events = 0;
  std::uint64_t m_heartbeats = 0;
  std::uint64_t m_artificial = 0;
  std::uint64_t m_bytes = 0;
  std::uint64_t m_largestEventLength = 0;
  std::uint64_t m_largestEventSubPackets = 0;
};

}  // namespace binlog_streamer
