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

#include <filesystem>
#include <string>
#include "storage/cStorageCatalog.hpp"
#include "storage/sStorageStartState.hpp"

namespace binlog_streamer {

class StorageRecovery {
 public:
  // Run once, before main() ever connects to a source. catalog must
  // already be Load()ed. A nonzero truncatedBytes is acted on only if
  // the file's "in use" bit is true - a closed file with a trimmed tail
  // is refused as corruption, not truncated.
  static bool Recover(const std::filesystem::path &dataDir,
                      StorageCatalog &catalog, StorageStartState &state,
                      std::string &error);

  // The same boundary for a relay that is already running and has just
  // lost its stream: where the next dump has to start from. Nothing is
  // truncated - a stream cut mid-transaction, or mid-event, left only
  // bytes the source sends again from the same position, and storage
  // compares them instead of writing them twice. Every byte must be on
  // disk before this is called (StorageWriter::DrainAndSync()): the
  // answer is read from the file, not from the cache.
  static bool ResumePoint(const std::filesystem::path &dataDir,
                          const StorageCatalog &catalog,
                          StorageStartState &state, std::string &error);
};

}  // namespace binlog_streamer
