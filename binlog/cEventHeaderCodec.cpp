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

#include "binlog/cEventHeaderCodec.hpp"

namespace binlog_streamer {

bool EventHeaderCodec::Parse(
    std::span<const std::uint8_t, EVENT_HEADER_LENGTH> data, EventHeader &value,
    std::string & /*error*/) {
  value.timestamp = static_cast<std::uint32_t>(data[0]) |
                    (static_cast<std::uint32_t>(data[1]) << 8) |
                    (static_cast<std::uint32_t>(data[2]) << 16) |
                    (static_cast<std::uint32_t>(data[3]) << 24);
  value.type = data[4];
  value.serverId = static_cast<std::uint32_t>(data[5]) |
                   (static_cast<std::uint32_t>(data[6]) << 8) |
                   (static_cast<std::uint32_t>(data[7]) << 16) |
                   (static_cast<std::uint32_t>(data[8]) << 24);
  value.eventLength = static_cast<std::uint32_t>(data[9]) |
                      (static_cast<std::uint32_t>(data[10]) << 8) |
                      (static_cast<std::uint32_t>(data[11]) << 16) |
                      (static_cast<std::uint32_t>(data[12]) << 24);
  value.nextPosition = static_cast<std::uint32_t>(data[13]) |
                       (static_cast<std::uint32_t>(data[14]) << 8) |
                       (static_cast<std::uint32_t>(data[15]) << 16) |
                       (static_cast<std::uint32_t>(data[16]) << 24);
  value.flags = static_cast<std::uint16_t>(data[17]) |
                static_cast<std::uint16_t>(data[18] << 8);
  return true;
}

}  // namespace binlog_streamer
