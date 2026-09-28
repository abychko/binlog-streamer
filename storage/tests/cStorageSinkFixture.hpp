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

#include "cTempDirectoryFixture.hpp"
#include "cache/cEventCache.hpp"
#include "receiver/iEventSink.hpp"
#include "sSinkOptions.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageEventSink.hpp"
#include "storage/cStorageWriter.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace binlog_streamer::test {

class StorageSinkFixture {
 public:
  ~StorageSinkFixture() { CloseSink(); }
  const std::filesystem::path &Directory() const {
    return m_directory.Directory();
  }
  std::filesystem::path Path(const std::string &name) const {
    return m_directory.Path(name);
  }
  StorageCatalog &Catalog() { return m_catalog; }
  PublishedPositionTracker &Published() { return m_published; }
  EventCache &Cache() { return m_cache.value(); }

  StorageWriter &Writer() { return m_writer.value(); }
  void Stop() { m_stop.store(true, std::memory_order_relaxed); }
  void StartWriter() {
    m_writer->Start();
    m_writerStarted = true;
  }

  StorageEventSink &OpenSink(
      EventSink &next,
      std::function<void(const std::string &, std::uint64_t)>
          onAbandonedFileClosed = nullptr,
      SinkOptions options = {}) {
    CloseSink();
    m_stop.store(false, std::memory_order_relaxed);
    std::string error;
    m_cache = EventCache::Reserve(options.cacheSize, m_stop, error,
                                  std::move(options.cacheHooks));
    if (!m_cache) throw std::runtime_error(error);
    m_writer.emplace(Directory(), *m_cache, m_catalog,
                     std::move(options.writerHooks));
    auto &sink =
        m_sink.emplace(Directory(), 0, next, m_catalog, *m_cache, *m_writer,
                       &m_published, std::move(onAbandonedFileClosed));
    if (options.startWriter) StartWriter();
    return sink;
  }

  void CloseSink() {
    if (m_writerStarted && !m_writer->Failed()) m_writer->DrainAndSync();
    m_sink.reset();
    m_writer.reset();
    m_writerStarted = false;
    m_cache.reset();
  }

  // Every disk observation or modification after sink calls must follow
  // Drain(), including a "before" size. The writer runs on its own thread.
  void Drain() {
    if (m_writer && !m_writer->DrainAndSync())
      throw std::runtime_error("drain: " + m_writer->LastError());
  }

 private:
  TempDirectoryFixture m_directory;
  StorageCatalog m_catalog;
  PublishedPositionTracker m_published;
  std::atomic<bool> m_stop{false};
  std::optional<EventCache> m_cache;
  std::optional<StorageWriter> m_writer;
  std::optional<StorageEventSink> m_sink;
  bool m_writerStarted = false;
};

}  // namespace binlog_streamer::test
