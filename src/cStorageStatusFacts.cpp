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

#include "cStorageStatusFacts.hpp"

#include "cache/hCacheDefaults.hpp"

namespace binlog_streamer {

std::size_t StorageStatusFacts::Files() const { return m_catalog.Size(); }

std::uint64_t StorageStatusFacts::Bytes() const {
  return m_catalog.TotalSize();
}

std::uint64_t StorageStatusFacts::MaxBytes() const { return m_diskMaxBytes; }

// The cache counts whole segments: a segment with one byte in it is
// occupied, which is what the memory is.
MemoryStatus StorageStatusFacts::Memory() const {
  if (m_cache == nullptr) return {};
  const CacheCounters counters = m_cache->Counters();
  return {static_cast<std::uint64_t>(counters.capacity) * CACHE_SEGMENT_SIZE,
          static_cast<std::uint64_t>(counters.occupied) * CACHE_SEGMENT_SIZE,
          counters.files};
}

void StorageStatusFacts::Published(std::string &file,
                                   std::uint64_t &position) const {
  const PublishedPosition current = m_published.Current();
  file = current.fileName;
  position = current.position;
}

std::optional<std::uint64_t> StorageStatusFacts::Offset(
    const std::string &file, std::uint64_t position) const {
  if (!m_catalog.Find(file)) return std::nullopt;
  return m_catalog.TotalSizeBefore(file) + position;
}

std::optional<std::uint64_t> StorageStatusFacts::Distance(
    const std::string &fromFile, std::uint64_t fromPosition,
    const std::string &toFile, std::uint64_t toPosition) const {
  return m_catalog.BytesBetween(fromFile, fromPosition, toFile, toPosition);
}

}  // namespace binlog_streamer
