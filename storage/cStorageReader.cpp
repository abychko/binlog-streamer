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

#include "storage/cStorageReader.hpp"
#include "cache/hCacheDefaults.hpp"
#include "storage/cReadOnlyBinlogFile.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <variant>

namespace binlog_streamer {
StorageReader::StorageReader(std::filesystem::path dataDir,
                             StorageCatalog &catalog,
                             PublishedPositionTracker &published,
                             EventCache *cache)
    : m_dataDir(std::move(dataDir)),
      m_catalog(catalog),
      m_published(published),
      m_cache(cache) {}

std::optional<std::string> StorageReader::FindStartFile(
    const GtidSet &replicaSet) const {
  return m_catalog.FindStartFile(replicaSet);
}

std::unique_ptr<FileCursor> StorageReader::Open(const std::string &fileName,
                                                std::string &error) {
  auto pin = m_catalog.Pin(fileName, error);
  if (!pin.has_value()) return nullptr;

  bool onDisk = true;
  std::uint64_t headerLength = 0;
  if (!m_catalog.ReadInfo(fileName, onDisk, headerLength)) {
    error = fileName + " is no longer in the storage catalog";
    return nullptr;
  }
  const auto path = (m_dataDir / fileName).string();
  auto cursor = std::unique_ptr<FileCursor>(
      new FileCursor(std::move(*pin), -1, path, headerLength));
  if (onDisk && cursor->Fd(error) < 0) return nullptr;
  return cursor;
}

bool StorageReader::Boundary(const FileCursor &cursor, std::uint64_t &boundary,
                             std::string &error) const {
  PublishedMark mark = m_published.Mark();
  if (mark.file != cursor.m_checkedMark) {
    mark = m_published.Locate(cursor.FileName(), cursor.m_published);
    cursor.m_checkedMark = mark.file;
  }
  if (cursor.m_published) {
    boundary = mark.position;
    return true;
  }
  const auto size = m_catalog.Boundary(cursor.FileName());
  if (!size) {
    error = cursor.FileName() + " is no longer in the storage catalog";
    return false;
  }
  boundary = *size;
  return true;
}

std::size_t StorageReader::Read(const FileCursor &cursor, std::uint64_t offset,
                                std::span<std::uint8_t> out,
                                std::string &error) {
  std::uint64_t boundary = 0;
  if (!Boundary(cursor, boundary, error)) return 0;
  if (offset > boundary) {
    error = cursor.FileName() + ": read offset " + std::to_string(offset) +
            " is past what may be read right now (" + std::to_string(boundary) +
            ")";
    return 0;
  }
  if (offset == boundary) return 0;

  const std::uint64_t available = boundary - offset;
  std::size_t toRead =
      static_cast<std::size_t>(std::min<std::uint64_t>(available, out.size()));
  if (toRead == 0) return 0;
  if (m_cache) {
    const auto result =
        m_cache->Read(cursor.FileName(), offset, out.first(toRead));
    if (const auto *copied =
            std::get_if<CacheReadResult::Copied>(&result.value)) {
      if (copied->n != 0) {
        m_readFromCache.fetch_add(copied->n, std::memory_order_relaxed);
        if (cursor.m_previousReadFromDisk)
          m_seamCrossings.fetch_add(1, std::memory_order_relaxed);
        cursor.m_previousReadFromDisk = false;
      }
      return copied->n;  // 0 here means retry-after-publication, not EOF
    }
    const auto firstCached =
        std::get<CacheReadResult::NotCached>(result.value).firstCached;
    if (firstCached != NOT_CACHED_ANYWHERE)
      toRead = static_cast<std::size_t>(
          std::min<std::uint64_t>(toRead, firstCached - offset));
  }
  const int fd = cursor.Fd(error);
  if (fd < 0 ||
      !ReadOnlyBinlogFile::ReadAt(fd, offset, out.first(toRead), error))
    return 0;
  m_readFromDisk.fetch_add(toRead, std::memory_order_relaxed);
  if (offset < cursor.m_headerLength)
    m_readFromDiskHeader.fetch_add(
        std::min<std::uint64_t>(toRead, cursor.m_headerLength - offset),
        std::memory_order_relaxed);
  cursor.m_previousReadFromDisk = true;
  return toRead;
}

NextFileOutcome StorageReader::Next(const FileCursor &current,
                                    std::uint64_t offset,
                                    std::unique_ptr<FileCursor> &next,
                                    std::string &error) {
  std::string successorName;
  std::uint64_t closedSize = 0;
  switch (m_catalog.FindSuccessor(current.FileName(), offset, successorName,
                                  closedSize)) {
    case SuccessorOutcome::NotDone:
    case SuccessorOutcome::NoSuccessor:
      return NextFileOutcome::NotYetAvailable;
    case SuccessorOutcome::OffsetPastEnd:
      error = current.FileName() + ": offset " + std::to_string(offset) +
              " is past its own final size (" + std::to_string(closedSize) +
              ")";
      return NextFileOutcome::Failed;
    case SuccessorOutcome::NotFound:
      error = current.FileName() + " is no longer in the storage catalog";
      return NextFileOutcome::Failed;
    case SuccessorOutcome::Found:
      break;
  }
  next = Open(successorName, error);
  return next ? NextFileOutcome::Found : NextFileOutcome::Failed;
}

WaitOutcome StorageReader::WaitForNewEvents(const PublishedPosition &target,
                                            std::chrono::milliseconds timeout,
                                            WaitStyle style) {
  return m_published.Wait(target, timeout, style);
}

PublishedPosition StorageReader::Published() const {
  return m_published.Current();
}

ReaderCounters StorageReader::Counters() const {
  return {m_readFromCache.load(std::memory_order_relaxed),
          m_readFromDisk.load(std::memory_order_relaxed),
          m_readFromDiskHeader.load(std::memory_order_relaxed),
          m_seamCrossings.load(std::memory_order_relaxed)};
}

}  // namespace binlog_streamer
