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
#include <string>
#include <vector>
#include "protocol/sErrPacket.hpp"

namespace binlog_streamer {

// Header byte 0xFF is unambiguous: unlike OK/EOF, it's never reused for
// anything else in this protocol.
class ErrPacketCodec {
 public:
  static bool IsErrPacket(std::span<const std::uint8_t> payload);
  static bool Parse(std::span<const std::uint8_t> payload, ErrPacket &value,
                    std::string &error);
  // An empty sqlState writes no '#' + SQLSTATE, as a server does before
  // CLIENT_PROTOCOL_41 is negotiated; any other length than 5 is written
  // as "HY000", the server's own default for an error that has none.
  static void Encode(const ErrPacket &value, std::vector<std::uint8_t> &out);
};

}  // namespace binlog_streamer
