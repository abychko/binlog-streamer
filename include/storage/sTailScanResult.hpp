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
#include "gtid/cGtidSet.hpp"

namespace binlog_streamer {

struct TailScanResult {
  // Nothing past this point is handed to a reader or survives a
  // resumed file's truncation.
  std::uint64_t lastBoundary = 0;

  // Every transaction found fully written; never includes an anonymous
  // transaction (GNO 0), which has no identity a GtidSet can represent.
  GtidSet completedGroups;

  // The file's actual size minus lastBoundary. Zero if the file already
  // ends exactly on a boundary.
  std::uint64_t truncatedBytes = 0;
};

}  // namespace binlog_streamer
