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

#include "cStorageServerState.hpp"

#include "binlog/hEventLimits.hpp"
#include "storage/cBinlogTailScanner.hpp"

namespace binlog_streamer {

std::string StorageServerState::GtidPurged() const {
  const std::optional<StoredFileRecord> first = m_catalog.First();
  if (!first) return {};
  return first->previousGtids.ToText();
}

std::string StorageServerState::GtidExecuted() const {
  const std::optional<StoredFileRecord> last = m_catalog.Last();
  if (!last) return {};

  std::lock_guard<std::mutex> lock(m_mutex);
  if (last->name != m_scannedFile) {
    m_scannedFile = last->name;
    m_scannedTo = last->headerLength;
    m_executed = last->previousGtids;
  }
  TailScanResult scan;
  std::string error;
  const std::size_t checksumLength =
      last->checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  if (BinlogTailScanner::Scan(m_dataDir / last->name, m_scannedTo,
                              checksumLength, scan, error) &&
      scan.lastBoundary >= m_scannedTo) {
    std::string mergeError;
    m_executed.AddFromEncoding(scan.completedGroups.Encode(false), mergeError);
    m_scannedTo = scan.lastBoundary;
  }
  return m_executed.ToText();
}

std::string StorageServerState::SourceVersion() const {
  const std::optional<StoredFileRecord> last = m_catalog.Last();
  if (!last) return {};
  return last->serverVersion;
}

std::string StorageServerState::BinlogChecksum() const {
  // An empty storage has received nothing yet; a server's own default
  // applies until the first file says otherwise.
  const std::optional<StoredFileRecord> first = m_catalog.First();
  if (!first) return "CRC32";
  const std::string algorithm = first->checksumAlgorithm;
  if (algorithm.empty()) return "CRC32";
  return algorithm == "OFF" ? "NONE" : algorithm;
}

std::optional<PreviousGtidsEvent> StorageServerState::PreviousGtids(
    const std::string &fileName) const {
  const std::optional<StoredFileRecord> record = m_catalog.Find(fileName);
  if (!record) return std::nullopt;

  // The event is the last one of the file's header, and as long as the
  // server writes it: a common header, the encoded set, a checksum.
  const std::size_t checksumLength =
      record->checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  const std::uint64_t length = EVENT_HEADER_LENGTH +
                               record->previousGtids.GetEncodedLength(false) +
                               checksumLength;
  if (length > record->headerLength) return std::nullopt;

  PreviousGtidsEvent event;
  event.position = record->headerLength - length;
  event.endPosition = record->headerLength;
  event.serverId = record->serverId;
  event.gtids = record->previousGtids.ToText();
  return event;
}

}  // namespace binlog_streamer
