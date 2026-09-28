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
#include "protocol/sRegisterSlaveCommand.hpp"

namespace binlog_streamer {

class ComRegisterSlaveCommand {
 public:
  static std::vector<std::uint8_t> Encode(const RegisterSlaveCommand &value);
  // Includes the command byte. The three strings carry a single raw
  // length byte (<251), not a full length-encoded integer; the two
  // trailing 4-byte fields must be present but are discarded.
  static bool Parse(std::span<const std::uint8_t> payload,
                    RegisterSlaveCommand &value, std::string &error);
};

}  // namespace binlog_streamer
