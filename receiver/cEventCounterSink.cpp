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

#include "receiver/cEventCounterSink.hpp"

#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"

#include <algorithm>

namespace binlog_streamer {

bool EventCounterSink::OnEventBegin(const EventHeader &header,
                                    const StreamPosition &) {
  ++events;
  if (header.type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
      header.type == static_cast<std::uint8_t>(EventType::HeartbeatV2))
    ++heartbeats;
  if ((header.flags & EVENT_FLAG_ARTIFICIAL) != 0) ++artificial;
  bytes += header.eventLength;
  largestEventLength =
      std::max<std::uint64_t>(largestEventLength, header.eventLength);
  return true;
}

bool EventCounterSink::OnEventBytes(std::span<const std::uint8_t> /*bytes*/) {
  return true;
}

bool EventCounterSink::OnEventEnd() { return true; }

}  // namespace binlog_streamer
