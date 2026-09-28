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
#include <string>
#include "receiver/sHeartbeatEvent.hpp"

namespace binlog_streamer {

// body excludes the Common-Header but still carries the trailing
// checksum; checksumLength says how many trailing bytes to drop before
// parsing.
class HeartbeatEventCodec {
 public:
  // v1: the whole body (after trimming checksum) is the file name text;
  // cannot fail structurally, so no error out-parameter.
  static void ParseV1(std::span<const std::uint8_t> body,
                      std::size_t checksumLength, HeartbeatEvent &value);

  // Unknown TLV types are skipped by length rather than rejected, for
  // forward compatibility.
  static bool ParseV2(std::span<const std::uint8_t> body,
                      std::size_t checksumLength, HeartbeatEvent &value,
                      std::string &error);
};

}  // namespace binlog_streamer
