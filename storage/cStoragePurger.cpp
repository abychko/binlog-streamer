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

#include "storage/cStoragePurger.hpp"
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/hStorageDefaults.hpp"

namespace binlog_streamer {

StoragePurger::StoragePurger(std::filesystem::path dataDir,
                             StorageCatalog &catalog, StoragePurgerHooks hooks)
    : m_dataDir(std::move(dataDir)),
      m_catalog(catalog),
      m_hooks(std::move(hooks)) {}

std::uint64_t StoragePurger::Cutoff(std::uint64_t sourceNow,
                                    std::chrono::seconds period) {
  if (period.count() <= 0) return sourceNow;
  const auto seconds = static_cast<std::uint64_t>(period.count());
  return sourceNow <= seconds ? 0 : sourceNow - seconds;
}

bool StoragePurger::Purge(const PurgeLimits &limits, std::string_view openFile,
                          PurgeResult &result, std::string &error) {
  result = {};
  error.clear();
  auto used = limits.space ? m_catalog.TotalSize() : 0;
  auto available = limits.space ? limits.space->availableBytes : 0;
  const bool spaceActive =
      limits.space && (used > limits.space->highWatermark ||
                       available < limits.space->minFreeSpace);
  while (m_catalog.Size() >= 2) {
    const auto first = m_catalog.At(0);
    const auto next = m_catalog.At(1);
    if (first.inUse || !first.onDisk || first.name == openFile) break;
    const bool expired =
        limits.cutoff && next.createdAt != 0 &&
        static_cast<std::uint64_t>(next.createdAt) < *limits.cutoff;
    const bool needSpace =
        spaceActive && (used > limits.space->lowWatermark ||
                        available < limits.space->minFreeSpace);
    if (!expired && !needSpace) break;
    std::string removeError;
    if (!m_catalog.Remove(removeError)) break;
    result.purged.push_back(first.name);
    used -= std::min(used, first.size);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    available += std::min(first.size, maximum - available);
  }
  if (limits.space) {
    result.usedBytes = used;
    result.availableBytes = available;
    result.spaceShort = used > limits.space->highWatermark ||
                        available < limits.space->minFreeSpace;
  }
  if (result.purged.empty()) return true;

  const auto indexPath = (m_dataDir / INDEX_FILE_NAME).string();
  std::vector<std::string> names;
  if (!BinlogIndexFile::Load(indexPath, names, error)) return false;
  if (names.size() < result.purged.size() ||
      !std::equal(result.purged.begin(), result.purged.end(), names.begin())) {
    std::ostringstream message;
    message << "purge prefix differs from index; catalog:";
    for (const auto &name : result.purged) message << ' ' << name;
    message << "; index:";
    for (const auto &name : names) message << ' ' << name;
    error = message.str();
    return false;
  }
  names.erase(names.begin(), names.begin() + result.purged.size());
  if (!BinlogIndexFile::Replace(indexPath, names, error)) return false;

  for (const auto &name : result.purged) {
    if (m_hooks.beforeUnlink) m_hooks.beforeUnlink(name);
    if (::unlink((m_dataDir / name).c_str()) != 0) {
      const int failure = errno;
      if (failure == ENOENT) continue;
      if (!result.warning.empty()) result.warning += '\n';
      result.warning += name + ": " + std::strerror(failure);
    }
  }
  return true;
}
}  // namespace binlog_streamer
