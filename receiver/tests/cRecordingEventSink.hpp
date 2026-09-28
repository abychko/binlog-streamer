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

#include "receiver/iEventSink.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace binlog_streamer::test {

struct RecordedEvent {
  EventHeader header;
  StreamPosition positionBeforeEvent;
  std::vector<std::uint8_t>
      bytes;  // concatenation of every OnEventBytes() call for this event
  bool ended = false;
};

// Returning false from a scripted call simulates
// StreamEndReason::StoppedBySink.
class RecordingEventSink : public EventSink {
 public:
  std::vector<RecordedEvent> events;

  // 1-based call index (across the whole run, not per event) at which to
  // return false; nullopt means never.
  std::optional<unsigned> stopAtEventBeginCall;
  std::optional<unsigned> stopAtEventBytesCall;
  std::optional<unsigned> stopAtEventEndCall;

  unsigned eventBeginCalls = 0;
  unsigned eventBytesCalls = 0;
  unsigned eventEndCalls = 0;

  bool OnEventBegin(const EventHeader &header,
                    const StreamPosition &position) override {
    ++eventBeginCalls;
    events.push_back(RecordedEvent{header, position, {}, false});
    return stopAtEventBeginCall != eventBeginCalls;
  }

  bool OnEventBytes(std::span<const std::uint8_t> bytes) override {
    ++eventBytesCalls;
    events.back().bytes.insert(events.back().bytes.end(), bytes.begin(),
                               bytes.end());
    return stopAtEventBytesCall != eventBytesCalls;
  }

  bool OnEventEnd() override {
    ++eventEndCalls;
    events.back().ended = true;
    return stopAtEventEndCall != eventEndCalls;
  }
};

}  // namespace binlog_streamer::test
