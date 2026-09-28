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

#include <chrono>
#include <cstdint>

#include "config/hConfigDefaults.hpp"

namespace binlog_streamer {

struct ServerSettings {
  std::uint32_t serverId = 0;
  // How many replicas may be connected at once; the one past it is
  // refused with ERR 1040, as a server refuses it.
  unsigned maxConnections = DEFAULT_MAX_CONNECTIONS;
  // How long a dump that caught up holds what it queued for the events
  // that follow, so they go in one write; zero sends at once.
  std::chrono::microseconds sendLinger = DEFAULT_SEND_LINGER;
  bool operator==(const ServerSettings &) const = default;
};

}  // namespace binlog_streamer
