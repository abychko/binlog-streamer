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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include "cache/cEventCache.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/eStorageOpenFailure.hpp"
#include "storage/sStorageStartState.hpp"

namespace binlog_streamer {

// Exclusive per-dataDir owner: a second instance pointed at the same
// directory refuses immediately.
class BinlogStorage {
 public:
  BinlogStorage() = default;
  ~BinlogStorage();
  BinlogStorage(const BinlogStorage &) = delete;
  BinlogStorage &operator=(const BinlogStorage &) = delete;

  // flock()s dataDir itself, not binlog.index. Creates an empty index
  // only if none existed, so "no index" and "empty index" stay
  // distinguishable.
  bool Open(const std::filesystem::path &dataDir, StorageOpenFailure &failure,
            std::string &error);

  // Makes disk state already found by recovery visible through
  // Published() before this run writes anything of its own; a no-op on
  // a fresh data_dir.
  void SeedPublished(const StorageStartState &state);

  // failure passes through Open()'s own value on Open() failure; a
  // Recover() failure always becomes StorageProblem.
  bool OpenResumed(const std::filesystem::path &dataDir,
                   StorageStartState &state, StorageOpenFailure &failure,
                   std::string &error);

  bool ReserveCache(std::uint64_t maxSize, std::chrono::seconds window,
                    const std::atomic<bool> &stop, StorageOpenFailure &failure,
                    std::string &error);
  EventCache *Cache() { return m_cache ? &*m_cache : nullptr; }

  StorageCatalog &Catalog() { return m_catalog; }
  PublishedPositionTracker &Published() { return m_published; }

 private:
  std::optional<EventCache> m_cache;
  StorageCatalog m_catalog;
  PublishedPositionTracker m_published;
  int m_lockFd = -1;
};

}  // namespace binlog_streamer
