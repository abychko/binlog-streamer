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
#include "status/iStorageFacts.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"

namespace binlog_streamer {

class StorageStatusFacts : public StorageFacts {
 public:
  // Nothing is owned; the cache may be null for a run without one.
  StorageStatusFacts(const StorageCatalog &catalog,
                     const PublishedPositionTracker &published,
                     const EventCache *cache, std::uint64_t diskMaxBytes)
      : m_catalog(catalog),
        m_published(published),
        m_cache(cache),
        m_diskMaxBytes(diskMaxBytes) {}

  std::size_t Files() const override;
  std::uint64_t Bytes() const override;
  std::uint64_t MaxBytes() const override;
  MemoryStatus Memory() const override;
  void Published(std::string &file, std::uint64_t &position) const override;
  std::optional<std::uint64_t> Offset(const std::string &file,
                                      std::uint64_t position) const override;
  std::optional<std::uint64_t> Distance(
      const std::string &fromFile, std::uint64_t fromPosition,
      const std::string &toFile, std::uint64_t toPosition) const override;

 private:
  const StorageCatalog &m_catalog;
  const PublishedPositionTracker &m_published;
  const EventCache *m_cache;
  std::uint64_t m_diskMaxBytes;
};

}  // namespace binlog_streamer
