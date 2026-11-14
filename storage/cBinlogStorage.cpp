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

#include "storage/cBinlogStorage.hpp"

#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageRecovery.hpp"
#include "storage/hAccessErrno.hpp"
#include "storage/hStorageDefaults.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace binlog_streamer {

BinlogStorage::~BinlogStorage() = default;

bool BinlogStorage::Open(const std::filesystem::path &dataDir,
                         StorageOpenFailure &failure, std::string &error) {
  const std::string dirPath = dataDir.string();

  bool accessProblem = false;
  if (!m_lock.Acquire(dataDir, accessProblem, error)) {
    failure = accessProblem ? StorageOpenFailure::AccessProblem
                            : StorageOpenFailure::StorageProblem;
    return false;
  }

  if (access(dirPath.c_str(), W_OK) != 0) {
    const int savedErrno = errno;
    failure = IsAccessErrno(savedErrno) ? StorageOpenFailure::AccessProblem
                                        : StorageOpenFailure::StorageProblem;
    error = "writing to " + dirPath + ": " + std::strerror(savedErrno);
    return false;
  }

  bool indexExisted = false;
  if (!m_catalog.Load(dataDir, indexExisted, error)) {
    failure = StorageOpenFailure::StorageProblem;
    return false;
  }

  if (!indexExisted) {
    const std::string indexPath = (dataDir / INDEX_FILE_NAME).string();
    if (!BinlogIndexFile::Replace(indexPath, std::vector<std::string>{},
                                  error)) {
      failure = StorageOpenFailure::StorageProblem;
      return false;
    }
  }
  return true;
}

void BinlogStorage::SeedPublished(const StorageStartState &state) {
  if (state.empty) return;
  m_published.Advance(state.lastFileName, state.lastFileLength);
}

bool BinlogStorage::OpenResumed(const std::filesystem::path &dataDir,
                                StorageStartState &state,
                                StorageOpenFailure &failure,
                                std::string &error) {
  if (!Open(dataDir, failure, error)) return false;
  if (!StorageRecovery::Recover(dataDir, m_catalog, state, error)) {
    failure = StorageOpenFailure::StorageProblem;
    return false;
  }
  SeedPublished(state);
  return true;
}

bool BinlogStorage::ReserveCache(std::uint64_t maxSize,
                                 std::chrono::seconds window,
                                 const std::atomic<bool> &stop,
                                 StorageOpenFailure &failure,
                                 std::string &error) {
  auto cache = EventCache::Reserve(maxSize, window, stop, error);
  if (!cache) {
    failure = StorageOpenFailure::StorageProblem;
    return false;
  }
  m_cache = std::move(cache);
  return true;
}

}  // namespace binlog_streamer
