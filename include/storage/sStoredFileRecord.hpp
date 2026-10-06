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
#include "gtid/cGtidSet.hpp"

namespace binlog_streamer {

struct StoredFileRecord {
  std::string name;
  std::string basename;
  std::uint64_t number = 0;

  std::uint64_t size = 0;

  std::uint64_t headerLength = 0;

  // The FDE's Common-Header timestamp, not its body's "created" field, which is
  // zeroed on every rotated-into file.
  std::uint32_t createdAt = 0;

  std::uint32_t serverId = 0;

  std::string serverVersion;

  std::string checksumAlgorithm;

  bool inUse = false;

  bool onDisk = true;

  GtidSet previousGtids;
};

}  // namespace binlog_streamer
