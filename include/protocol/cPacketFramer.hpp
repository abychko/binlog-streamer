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
#include <span>
#include <vector>
#include "protocol/sPacketDecodeResult.hpp"

namespace binlog_streamer {

// Not thread-safe: sequenceId is caller-owned.
class PacketFramer {
 public:
  // An exact MAX_PAYLOAD_PER_PACKET chunk is followed by another, possibly
  // empty one: Decode()'s only end-of-packet signal.
  static void Encode(std::span<const std::uint8_t> payload,
                     std::uint8_t &sequenceId, std::vector<std::uint8_t> &out);
  static void Encode(std::span<const std::uint8_t> head,
                     std::span<const std::uint8_t> body,
                     std::uint8_t &sequenceId, std::vector<std::uint8_t> &out);

  // NeedMoreBytes: bytesNeeded is a lower bound. verifySequence false is for
  // streams under protocol compression, where inner sequence ids are not
  // checked (sql-common/net_serv.cc).
  static PacketDecodeResult Decode(std::span<const std::uint8_t> data,
                                   std::uint8_t &sequenceId,
                                   std::vector<std::uint8_t> &payload,
                                   bool verifySequence = true);

  static PacketDecodeResult Measure(std::span<const std::uint8_t> data,
                                    std::uint8_t sequenceId,
                                    bool verifySequence = true);
};

}  // namespace binlog_streamer
