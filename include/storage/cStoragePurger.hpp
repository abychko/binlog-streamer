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
#include <filesystem>
#include <string>
#include <string_view>
#include "storage/sPurgeLimits.hpp"
#include "storage/sPurgeResult.hpp"
#include "storage/sStoragePurgerHooks.hpp"

namespace binlog_streamer {
class StorageCatalog;

// Writer thread only: that thread owns the index and disk. A reader
// pin keeps that file and everything after it, as in MySQL.
class StoragePurger {
 public:
  StoragePurger(std::filesystem::path dataDir, StorageCatalog &catalog,
                StoragePurgerHooks hooks = {});
  static std::uint64_t Cutoff(std::uint64_t sourceNow,
                              std::chrono::seconds period);
  // On index failure, removed records stay removed in the catalog; the
  // caller must stop the writer.
  bool Purge(const PurgeLimits &limits, std::string_view openFile,
             PurgeResult &result, std::string &error);

 private:
  std::filesystem::path m_dataDir;
  StorageCatalog &m_catalog;
  StoragePurgerHooks m_hooks;
};
}  // namespace binlog_streamer
