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
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "gtid/cGtidSet.hpp"
#include "storage/cFilePin.hpp"
#include "storage/eSuccessorOutcome.hpp"
#include "storage/sStoredFileRecord.hpp"

namespace binlog_streamer {

class StorageCatalog {
 public:
  bool Load(const std::filesystem::path &dataDir, bool &indexExisted,
            std::string &error);

  void Add(StoredFileRecord record);
  bool MarkOnDisk(const std::string &name, std::string &error);

  bool Close(std::uint64_t finalSize, std::string &error);

  // Fails if the oldest record is pinned: pruning stops before a file a reader
  // holds.
  bool Remove(std::string &error);

  // Does not close the gap between FindStartFile() and Pin() - nullopt
  // here means "the file is gone", not "retry".
  std::optional<FilePin> Pin(const std::string &fileName, std::string &error);

  // Leaves inUse untouched, unlike Close(); only StorageRecovery uses it.
  bool UpdateSize(std::uint64_t size, std::string &error);

  // The newest file whose Previous_gtids is a subset of replicaSet - a
  // newer one would skip groups the replica lacks, an older one would
  // resend groups it already has.
  std::optional<std::string> FindStartFile(const GtidSet &replicaSet) const;

  std::optional<std::uint64_t> Boundary(const std::string &name) const;
  bool ReadInfo(const std::string &name, bool &onDisk,
                std::uint64_t &headerLength) const;
  std::optional<StoredFileRecord> Find(const std::string &name) const;
  std::size_t Size() const;
  std::uint64_t TotalSize() const;
  std::uint64_t TotalSizeBefore(std::string_view name) const;
  // Both files are looked up under one lock, so a purge cannot land between
  // them; nullopt if either is absent or `to` precedes `from`.
  std::optional<std::uint64_t> BytesBetween(std::string_view fromName,
                                            std::uint64_t fromPosition,
                                            std::string_view toName,
                                            std::uint64_t toPosition) const;
  // A copy, not a reference: callers must not hold one across a second
  // call into this class while another thread could be writing.
  StoredFileRecord At(std::size_t index) const;
  // An index taken from Size() can name another record by the time At() reads
  // it: the purger removes from the front.
  std::optional<StoredFileRecord> First() const;
  std::optional<StoredFileRecord> Last() const;

  // done means closed and offset at final size - a published boundary
  // alone is not proof nothing more will be appended.
  SuccessorOutcome FindSuccessor(const std::string &currentFileName,
                                 std::uint64_t offset,
                                 std::string &successorName,
                                 std::uint64_t &closedSize) const;

 private:
  friend class FilePin;
  void Unpin(const std::string &fileName);

  mutable std::shared_mutex m_mutex;
  std::vector<StoredFileRecord>
      m_records;  // ascending index order - oldest first, current file last
  std::unordered_map<std::string, int>
      m_pinCounts;  // guarded by m_mutex, same as m_records; absent means zero
};

}  // namespace binlog_streamer
