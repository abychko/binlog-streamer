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
  // Run once, before connecting; catalog must be Load()ed. A trimmed tail is
  // truncated only if the file is still "in use"; a closed one is refused as
  // corruption.
  static bool Recover(const std::filesystem::path &dataDir,
                      StorageCatalog &catalog, StorageStartState &state,
                      std::string &error);

  // The same boundary for a running relay that lost its stream. Nothing is
  // truncated. Every byte must be on disk first
  // (StorageWriter::DrainAndSync()): the answer is read from the file, not the
  // cache.
  static bool ResumePoint(const std::filesystem::path &dataDir,
                          const StorageCatalog &catalog,
                          StorageStartState &state, std::string &error);
};

}  // namespace binlog_streamer
