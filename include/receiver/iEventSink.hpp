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

#include <cstdint>
#include <span>
#include "binlog/sEventHeader.hpp"
#include "receiver/sStreamPosition.hpp"

namespace binlog_streamer {

// Called for every event, including heartbeats and artificial ones - no
// pre-filtering; a sink filters by EventHeader::type/flags itself.
class EventSink {
 public:
  virtual ~EventSink() = default;

  // Treat position, not header.nextPosition (32 bits only), as the
  // source of truth. False stops the stream; no further calls follow.
  virtual bool OnEventBegin(const EventHeader &header,
                            const StreamPosition &position) = 0;

  // Span sizes across all calls sum to header.eventLength; bytes stream
  // in as they arrive off the wire, not buffered whole first.
  virtual bool OnEventBytes(std::span<const std::uint8_t> bytes) = 0;

  // Not called if an earlier callback already returned false for this
  // event.
  virtual bool OnEventEnd() = 0;
};

}  // namespace binlog_streamer
