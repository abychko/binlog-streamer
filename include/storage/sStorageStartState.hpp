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

struct StorageStartState {
  // True only for a data_dir with no files yet - startSet, lastFileName
  // and lastFileLength are all meaningless then.
  bool empty = true;

  // Previous_gtids of the last indexed file plus every group found
  // fully written past it - never gtid_executed, since this must be
  // exactly the boundary of a file the relay actually holds.
  GtidSet startSet;
  std::string lastFileName;
  // Post-truncation length if the file was still "in use"; used by
  // BinlogStorage::SeedPublished() to make history visible early. The
  // writer re-derives its own on resume.
  std::uint64_t lastFileLength = 0;

  // Travels out through this struct because library code here never
  // writes to stdout/stderr itself; only main.cpp prints, once, before
  // connecting.
  bool truncated = false;
  std::uint64_t truncatedBytes = 0;
};

}  // namespace binlog_streamer
