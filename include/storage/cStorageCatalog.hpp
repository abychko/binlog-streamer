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
  // Repairs leftover files from an interrupted create/purge before
  // returning. indexExisted distinguishes "no index" from "empty index".
  bool Load(const std::filesystem::path &dataDir, bool &indexExisted,
            std::string &error);

  // onDisk may remain false until the writer has created its header
  // and appended the durable index entry.
  void Add(StoredFileRecord record);
  // Called after the header and index entry are durable.
  bool MarkOnDisk(const std::string &name, std::string &error);

  // Fails if there is no last record to close - a caller error (Add()
  // was skipped), not a condition the sink triggers itself.
  bool Close(std::uint64_t finalSize, std::string &error);

  // Fails if there is nothing to remove, or if the oldest record is
  // pinned - a caller pruning forward stops here rather than pruning
  // past a file a reader still holds.
  bool Remove(std::string &error);

  // Does not close the gap between FindStartFile() and Pin() - nullopt
  // here means "the file is gone", not "retry".
  std::optional<FilePin> Pin(const std::string &fileName, std::string &error);

  // Leaves inUse untouched, unlike Close(). Used only by StorageRecovery
  // after truncating a resumed file to its last complete transaction
  // boundary.
  bool UpdateSize(std::uint64_t size, std::string &error);

  // The newest file whose Previous_gtids is a subset of replicaSet - a
  // newer one would skip groups the replica lacks, an older one would
  // resend groups it already has.
  std::optional<std::string> FindStartFile(const GtidSet &replicaSet) const;

  std::optional<std::uint64_t> Boundary(const std::string &name) const;
  // Copies the read metadata in one lookup, without copying the GTID sets.
  bool ReadInfo(const std::string &name, bool &onDisk,
                std::uint64_t &headerLength) const;
  // Under one shared lock.
  std::optional<StoredFileRecord> Find(const std::string &name) const;
  std::size_t Size() const;
  // Sums every record size under one shared lock, without copying records.
  std::uint64_t TotalSize() const;
  // Under one shared lock; an absent name includes all records.
  std::uint64_t TotalSizeBefore(std::string_view name) const;
  // Bytes from one stored point to a later one, both looked up under one
  // shared lock, so a purge or a new file cannot land between them;
  // nullopt when either file is absent or `to` comes before `from`.
  std::optional<std::uint64_t> BytesBetween(std::string_view fromName,
                                            std::uint64_t fromPosition,
                                            std::string_view toName,
                                            std::uint64_t toPosition) const;
  // A copy, not a reference: callers must not hold one across a second
  // call into this class while another thread could be writing.
  StoredFileRecord At(std::size_t index) const;
  // The oldest and the newest record, each under one shared lock: an index
  // taken from Size() can name a different record, or none at all, by the
  // time At() reads it, because the purger removes from the front.
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
  // Called only from a FilePin's destructor/move-assignment, on a name
  // just handed back by Pin().
  void Unpin(const std::string &fileName);

  mutable std::shared_mutex m_mutex;
  std::vector<StoredFileRecord>
      m_records;  // ascending index order - oldest first, current file last
  std::unordered_map<std::string, int>
      m_pinCounts;  // guarded by m_mutex, same as m_records; absent means zero
};

}  // namespace binlog_streamer
