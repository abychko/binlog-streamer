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
#include <string>
#include "receiver/eStreamEndReason.hpp"
#include "receiver/sStreamPosition.hpp"

namespace binlog_streamer {

struct StreamResult {
  StreamEndReason reason = StreamEndReason::MalformedStream;
  std::string message;  // human-readable; never contains a secret (this tract
                        // has none of its own after dump starts)

  // Meaningful only when reason == SourceError; kept separate from
  // message so a caller can branch on errorCode without parsing text.
  std::uint16_t errorCode = 0;
  std::string errorText;

  StreamPosition firstPosition;
  StreamPosition lastPosition;  // the last event delivered, or the position the
                                // reader started from if none was

  std::uint64_t events = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t artificial = 0;
  std::uint64_t bytes = 0;
  std::uint64_t largestEventLength = 0;
  std::uint64_t largestEventSubPackets = 0;
};

}  // namespace binlog_streamer
