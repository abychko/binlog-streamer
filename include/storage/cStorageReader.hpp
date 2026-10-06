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

#include "cache/cEventCache.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/iBinlogStorageReader.hpp"
#include "storage/sReaderCounters.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace binlog_streamer {

class StorageReader : public BinlogStorageReader {
 public:
  StorageReader(std::filesystem::path dataDir, StorageCatalog &catalog,
                PublishedPositionTracker &published,
                EventCache *cache = nullptr);

  std::optional<std::string> FindStartFile(
      const GtidSet &replicaSet) const override;
  std::unique_ptr<FileCursor> Open(const std::string &fileName,
                                   std::string &error) override;
  std::size_t Read(const FileCursor &cursor, std::uint64_t offset,
                   std::span<std::uint8_t> out, std::string &error) override;
  NextFileOutcome Next(const FileCursor &current, std::uint64_t offset,
                       std::unique_ptr<FileCursor> &next,
                       std::string &error) override;
  WaitOutcome WaitForNewEvents(const PublishedPosition &target,
                               std::chrono::milliseconds timeout,
                               WaitStyle style) override;
  PublishedPosition Published() const override;
  ReaderCounters Counters() const;

 private:
  bool Boundary(const FileCursor &cursor, std::uint64_t &boundary,
                std::string &error) const;

  std::filesystem::path m_dataDir;
  StorageCatalog &m_catalog;
  PublishedPositionTracker &m_published;
  EventCache *m_cache;
  std::atomic<std::uint64_t> m_readFromCache{0};
  std::atomic<std::uint64_t> m_readFromDisk{0};
  std::atomic<std::uint64_t> m_readFromDiskHeader{0};
  std::atomic<std::uint64_t> m_seamCrossings{0};
};

}  // namespace binlog_streamer
