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

#include "storage/cStorageCatalog.hpp"

#include "binlog/cBinlogFileName.hpp"
#include "storage/cBinlogFileHeaderReader.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/hStorageDefaults.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace binlog_streamer {
namespace {

std::string IndexPath(const std::filesystem::path &dataDir) {
  return (dataDir / INDEX_FILE_NAME).string();
}
std::string TmpIndexPath(const std::filesystem::path &dataDir) {
  return IndexPath(dataDir) + ".tmp";
}

struct ParsedName {
  std::string basename;
  std::uint64_t number = 0;
};

}  // namespace

bool StorageCatalog::Load(const std::filesystem::path &dataDir,
                          bool &indexExisted, std::string &error) {
  std::unique_lock lock(m_mutex);
  m_records.clear();

  const std::string indexPath = IndexPath(dataDir);
  std::error_code indexExistsError;
  indexExisted = std::filesystem::exists(indexPath, indexExistsError);
  if (indexExistsError) {
    error = "checking " + indexPath + ": " + indexExistsError.message();
    return false;
  }

  std::vector<std::string> indexNames;
  if (!BinlogIndexFile::Load(indexPath, indexNames, error)) return false;

  std::error_code removeTmpError;
  std::filesystem::remove(TmpIndexPath(dataDir), removeTmpError);

  // basename/number of the first and last entries only, not a min/max over
  // every entry: the source's own log-bin basename can legitimately change
  // mid-history, so a remnant is judged only against the edge it would extend.
  std::vector<ParsedName> parsedIndexNames;
  parsedIndexNames.reserve(indexNames.size());
  for (const auto &name : indexNames) {
    ParsedName parsed;
    std::string parseError;
    if (!BinlogFileName::Parse(name, parsed.basename, parsed.number,
                               parseError)) {
      error = "binlog.index holds a name storage cannot parse: " + name + " (" +
              parseError + ")";
      return false;
    }
    parsedIndexNames.push_back(std::move(parsed));
  }
  const ParsedName *firstEntry =
      parsedIndexNames.empty() ? nullptr : &parsedIndexNames.front();
  const ParsedName *lastEntry =
      parsedIndexNames.empty() ? nullptr : &parsedIndexNames.back();

  const std::set<std::string> indexed(indexNames.begin(), indexNames.end());
  std::vector<std::string> toDelete;
  std::vector<std::string> refusals;
  // At most one "create whose index append never happened" can be
  // legitimate; two or more are refused rather than guessed at.
  std::vector<std::string> newerThanLast;

  std::error_code listError;
  for (const auto &entry : std::filesystem::directory_iterator(
           dataDir, std::filesystem::directory_options::none, listError)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name == INDEX_FILE_NAME ||
        name == std::string(INDEX_FILE_NAME) + ".tmp")
      continue;
    if (name == SERVER_UUID_FILE_NAME) continue;
    if (std::find(TLS_FILE_NAMES.begin(), TLS_FILE_NAMES.end(), name) !=
        TLS_FILE_NAMES.end())
      continue;
    if (indexed.contains(name)) continue;

    std::string entryBasename;
    std::uint64_t number = 0;
    std::string parseError;
    if (!BinlogFileName::Parse(name, entryBasename, number, parseError)) {
      refusals.push_back(
          name + ": not in binlog.index and not a recognized remnant (" +
          parseError + ")");
      continue;
    }
    if (parsedIndexNames.empty()) {
      if (!indexExisted) {
        refusals.push_back(
            name +
            ": binlog.index is missing entirely, not merely empty - refusing "
            "rather "
            "than guessing whether this file is this project's own");
      } else {
        newerThanLast.push_back(name);
      }
      continue;
    }
    if (entryBasename == lastEntry->basename && number > lastEntry->number) {
      newerThanLast.push_back(name);
    } else if (entryBasename == firstEntry->basename &&
               number < firstEntry->number) {
      toDelete.push_back(name);
    } else if (entryBasename != lastEntry->basename &&
               entryBasename != firstEntry->basename) {
      refusals.push_back(name +
                         ": not in binlog.index and not a recognized remnant "
                         "(basename matches neither "
                         "the first nor the last entry in binlog.index)");
    } else {
      refusals.push_back(name +
                         ": not in binlog.index and not a recognized remnant "
                         "(falls inside the indexed range)");
    }
  }
  if (listError) {
    error = "listing " + dataDir.string() + ": " + listError.message();
    return false;
  }

  if (newerThanLast.size() > 1) {
    for (const auto &name : newerThanLast)
      refusals.push_back(name +
                         ": more than one file newer than the last indexed one "
                         "- not a safe remnant to "
                         "guess about, refusing all of them");
  } else {
    toDelete.insert(toDelete.end(), newerThanLast.begin(), newerThanLast.end());
  }

  for (const auto &name : indexNames) {
    std::error_code existsError;
    if (!std::filesystem::exists(dataDir / name, existsError))
      refusals.push_back(name + ": listed in binlog.index but missing on disk");
  }

  if (!refusals.empty()) {
    error = "data directory does not match binlog.index:";
    for (const auto &reason : refusals) error += "\n  " + reason;
    return false;
  }

  // Read every header and check the in-use bit before deleting anything.
  std::vector<StoredFileRecord> records;
  records.reserve(indexNames.size());
  for (std::size_t i = 0; i < indexNames.size(); ++i) {
    StoredFileRecord record;
    std::string readError;
    if (!BinlogFileHeaderReader::Read(dataDir / indexNames[i], indexNames[i],
                                      record, readError)) {
      error = "reading " + indexNames[i] + ": " + readError;
      return false;
    }
    if (record.inUse && i + 1 != indexNames.size()) {
      error = indexNames[i] +
              " has the \"file in use\" bit set, but is not the last file in "
              "binlog.index";
      return false;
    }
    records.push_back(std::move(record));
  }

  for (const auto &name : toDelete) {
    std::error_code deleteError;
    std::filesystem::remove(dataDir / name, deleteError);
    if (deleteError) {
      error = "removing leftover " + name + ": " + deleteError.message();
      return false;
    }
  }

  m_records = std::move(records);
  return true;
}

bool StorageCatalog::MarkOnDisk(const std::string &name, std::string &error) {
  std::unique_lock lock(m_mutex);
  for (auto &record : m_records) {
    if (record.name == name) {
      record.onDisk = true;
      return true;
    }
  }
  error = "created file is absent from the catalog";
  return false;
}

void StorageCatalog::Add(StoredFileRecord record) {
  std::unique_lock lock(m_mutex);
  m_records.push_back(std::move(record));
}

bool StorageCatalog::Close(std::uint64_t finalSize, std::string &error) {
  std::unique_lock lock(m_mutex);
  if (m_records.empty()) {
    error = "no open file to close";
    return false;
  }
  m_records.back().size = finalSize;
  m_records.back().inUse = false;
  return true;
}

bool StorageCatalog::Remove(std::string &error) {
  std::unique_lock lock(m_mutex);
  if (m_records.empty()) {
    error = "no file to remove";
    return false;
  }
  const std::string &name = m_records.front().name;
  const auto pinned = m_pinCounts.find(name);
  if (pinned != m_pinCounts.end() && pinned->second > 0) {
    error =
        name + " is pinned by " + std::to_string(pinned->second) + " reader(s)";
    return false;
  }
  m_records.erase(m_records.begin());
  return true;
}

std::optional<FilePin> StorageCatalog::Pin(const std::string &fileName,
                                           std::string &error) {
  std::unique_lock lock(m_mutex);
  const bool present = std::any_of(m_records.begin(), m_records.end(),
                                   [&fileName](const StoredFileRecord &record) {
                                     return record.name == fileName;
                                   });
  if (!present) {
    error = fileName + " is not in the storage catalog";
    return std::nullopt;
  }
  // Copied before the count is touched, then only moved (never throws)
  // into FilePin: a bad_alloc from a later copy would leave the count
  // incremented with nothing left alive to decrement it back down.
  std::string name = fileName;
  ++m_pinCounts[name];
  return FilePin(*this, std::move(name));
}

void StorageCatalog::Unpin(const std::string &fileName) {
  std::unique_lock lock(m_mutex);
  const auto it = m_pinCounts.find(fileName);
  if (it == m_pinCounts.end()) return;
  if (--it->second == 0) m_pinCounts.erase(it);
}

bool StorageCatalog::UpdateSize(std::uint64_t size, std::string &error) {
  std::unique_lock lock(m_mutex);
  if (m_records.empty()) {
    error = "no last record to update";
    return false;
  }
  m_records.back().size = size;
  return true;
}

std::optional<std::string> StorageCatalog::FindStartFile(
    const GtidSet &replicaSet) const {
  std::shared_lock lock(m_mutex);
  for (auto it = m_records.rbegin(); it != m_records.rend(); ++it) {
    if (it->previousGtids.IsSubsetOf(replicaSet)) return it->name;
  }
  return std::nullopt;
}

std::size_t StorageCatalog::Size() const {
  std::shared_lock lock(m_mutex);
  return m_records.size();
}

std::optional<StoredFileRecord> StorageCatalog::First() const {
  std::shared_lock lock(m_mutex);
  if (m_records.empty()) return std::nullopt;
  return m_records.front();
}

std::optional<StoredFileRecord> StorageCatalog::Last() const {
  std::shared_lock lock(m_mutex);
  if (m_records.empty()) return std::nullopt;
  return m_records.back();
}

std::uint64_t StorageCatalog::TotalSize() const {
  std::shared_lock lock(m_mutex);
  std::uint64_t total = 0;
  for (const auto &record : m_records) total += record.size;
  return total;
}

std::uint64_t StorageCatalog::TotalSizeBefore(std::string_view name) const {
  std::shared_lock lock(m_mutex);
  std::uint64_t total = 0;
  for (const auto &record : m_records) {
    if (record.name == name) break;
    total += record.size;
  }
  return total;
}

std::optional<std::uint64_t> StorageCatalog::BytesBetween(
    std::string_view fromName, std::uint64_t fromPosition,
    std::string_view toName, std::uint64_t toPosition) const {
  std::shared_lock lock(m_mutex);
  std::uint64_t from = 0;
  std::uint64_t total = 0;
  bool fromFound = false;
  for (const auto &record : m_records) {
    if (record.name == fromName) {
      from = total + fromPosition;
      fromFound = true;
    }
    if (record.name == toName) {
      const std::uint64_t to = total + toPosition;
      if (!fromFound || to < from) return std::nullopt;
      return to - from;
    }
    total += record.size;
  }
  return std::nullopt;
}

StoredFileRecord StorageCatalog::At(std::size_t index) const {
  std::shared_lock lock(m_mutex);
  return m_records.at(index);
}

SuccessorOutcome StorageCatalog::FindSuccessor(
    const std::string &currentFileName, std::uint64_t offset,
    std::string &successorName, std::uint64_t &closedSize) const {
  std::shared_lock lock(m_mutex);
  for (std::size_t i = 0; i < m_records.size(); ++i) {
    if (m_records[i].name != currentFileName) continue;
    if (m_records[i].inUse) return SuccessorOutcome::NotDone;
    if (offset > m_records[i].size) {
      closedSize = m_records[i].size;
      return SuccessorOutcome::OffsetPastEnd;
    }
    if (offset < m_records[i].size) return SuccessorOutcome::NotDone;
    if (i + 1 >= m_records.size()) return SuccessorOutcome::NoSuccessor;
    successorName = m_records[i + 1].name;
    return SuccessorOutcome::Found;
  }
  return SuccessorOutcome::NotFound;
}

std::optional<std::uint64_t> StorageCatalog::Boundary(
    const std::string &name) const {
  std::shared_lock lock(m_mutex);
  const auto it = std::find_if(
      m_records.rbegin(), m_records.rend(),
      [&](const StoredFileRecord &record) { return record.name == name; });
  if (it == m_records.rend()) return std::nullopt;
  return it->size;
}

std::optional<StoredFileRecord> StorageCatalog::Find(
    const std::string &name) const {
  std::shared_lock lock(m_mutex);
  const auto it = std::find_if(
      m_records.rbegin(), m_records.rend(),
      [&](const StoredFileRecord &record) { return record.name == name; });
  if (it == m_records.rend()) return std::nullopt;
  return *it;
}

bool StorageCatalog::ReadInfo(const std::string &name, bool &onDisk,
                              std::uint64_t &headerLength) const {
  std::shared_lock lock(m_mutex);
  const auto it = std::find_if(
      m_records.rbegin(), m_records.rend(),
      [&](const StoredFileRecord &record) { return record.name == name; });
  if (it == m_records.rend()) return false;
  onDisk = it->onDisk;
  headerLength = it->headerLength;
  return true;
}

}  // namespace binlog_streamer
