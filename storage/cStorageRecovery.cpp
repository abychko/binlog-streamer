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

#include "storage/cStorageRecovery.hpp"

#include "binlog/hEventLimits.hpp"
#include "storage/cBinlogFileWriter.hpp"
#include "storage/cBinlogTailScanner.hpp"

#include <optional>

namespace binlog_streamer {
namespace {

bool StartSetOf(const StoredFileRecord &last, const TailScanResult &scan,
                GtidSet &startSet, std::string &error) {
  startSet = last.previousGtids;
  std::string mergeError;
  if (!startSet.AddFromEncoding(
          scan.completedGroups.Encode(/*skipTaggedGtids=*/false), mergeError)) {
    error = "merging recovered transaction groups into " + last.name +
            "'s own Previous_gtids: " + mergeError;
    return false;
  }
  return true;
}

}  // namespace

bool StorageRecovery::Recover(const std::filesystem::path &dataDir,
                              StorageCatalog &catalog, StorageStartState &state,
                              std::string &error) {
  state = StorageStartState{};

  const std::size_t count = catalog.Size();
  if (count == 0) {
    state.empty = true;
    return true;
  }
  state.empty = false;

  for (std::size_t i = 0; i + 1 < count; ++i) {
    const StoredFileRecord record = catalog.At(i);
    if (record.inUse) {
      error = record.name +
              " has the \"file in use\" bit set, but is not the last file in "
              "the catalog";
      return false;
    }
  }

  const StoredFileRecord last = catalog.At(count - 1);
  const auto path = dataDir / last.name;
  const std::size_t checksumLength =
      last.checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;

  TailScanResult scan;
  if (!BinlogTailScanner::Scan(path, last.headerLength, checksumLength, scan,
                               error))
    return false;

  if (last.inUse) {
    if (scan.truncatedBytes > 0) {
      BinlogFileWriter writer;
      if (!writer.OpenExisting(path.string(), error)) return false;
      if (!writer.Truncate(scan.lastBoundary, error)) return false;
      if (!catalog.UpdateSize(writer.Size(), error)) return false;
      state.truncated = true;
      state.truncatedBytes = scan.truncatedBytes;
    }
  } else if (scan.truncatedBytes > 0) {
    // A closed file is immutable and always ends exactly on a boundary
    // (its own last event is that ROTATE, seen as Standalone). Anything
    // past the last boundary here means storage does not match its own history.
    error = last.name + " is closed, but " +
            std::to_string(scan.truncatedBytes) +
            " byte(s) past its own last complete transaction boundary do not "
            "belong to any recognized event - "
            "possible corruption";
    return false;
  }

  if (!StartSetOf(last, scan, state.startSet, error)) return false;
  state.lastFileName = last.name;
  state.lastFileLength = last.inUse ? scan.lastBoundary : last.size;
  return true;
}

bool StorageRecovery::ResumePoint(const std::filesystem::path &dataDir,
                                  const StorageCatalog &catalog,
                                  StorageStartState &state,
                                  std::string &error) {
  state = StorageStartState{};

  // Last(), not At(Size() - 1): the purger removes from the front while
  // this runs, and an index taken separately could name another record.
  const std::optional<StoredFileRecord> last = catalog.Last();
  if (!last) {
    state.empty = true;
    return true;
  }
  state.empty = false;

  TailScanResult scan;
  const std::size_t checksumLength =
      last->checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  if (!BinlogTailScanner::Scan(dataDir / last->name, last->headerLength,
                               checksumLength, scan, error))
    return false;

  if (!StartSetOf(*last, scan, state.startSet, error)) return false;
  state.lastFileName = last->name;
  state.lastFileLength = last->inUse ? scan.lastBoundary : last->size;
  return true;
}

}  // namespace binlog_streamer
