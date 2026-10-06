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
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include "cache/sWriteRange.hpp"
#include "cache/sWriteSnapshot.hpp"
#include "storage/cStoragePurger.hpp"
#include "storage/sStorageExpiry.hpp"
#include "storage/sStorageWriterHooks.hpp"
#include "storage/sWriterTask.hpp"
namespace binlog_streamer {
class EventCache;
class StorageCatalog;
class BinlogFileWriter;
// One producer posts tasks in stream order. The cache/catalog must
// outlive this object. Start/Stop/DrainAndSync are serialized by the
// owner, not by the worker.
class StorageWriter {
 public:
  StorageWriter(std::filesystem::path directory, EventCache &cache,
                StorageCatalog &catalog, StorageWriterHooks hooks = {},
                std::function<void()> onFirstSpaceWait = {},
                StorageExpiry expiry = {});
  ~StorageWriter();
  StorageWriter(const StorageWriter &) = delete;
  StorageWriter &operator=(const StorageWriter &) = delete;
  void Start();
  void Stop();
  void PostCreate(std::string name, std::span<const std::uint8_t> fde,
                  std::span<const std::uint8_t> pge);
  void PostOpenExisting(std::string name);
  void PostCloseAbandoned(std::string name);
  void PostClose(std::string name);
  void PostPurge();
  void Wake();
  void WakeForData();
  void NoteEventCompleted() {
    m_events.fetch_add(1, std::memory_order_relaxed);
  }
  // The producer must stop posting until this returns. Does not close the
  // current file.
  bool DrainAndSync();
  bool Failed() const { return m_failed.load(std::memory_order_acquire); }
  std::string LastError() const;
  std::uint64_t SyncsPerformed() const {
    return m_syncs.load(std::memory_order_relaxed);
  }
  std::uint64_t BytesWritten() const {
    return m_bytes.load(std::memory_order_relaxed);
  }
  std::uint64_t WriteCalls() const {
    return m_writes.load(std::memory_order_relaxed);
  }
  std::uint64_t MaxUnwrittenAgeMilliseconds() const {
    return m_maxAge.load(std::memory_order_relaxed);
  }

 private:
  void Post(WriterTask task);
  void WakeLocked();
  void Run();
  bool WritePending(WriteSnapshot &snapshot);
  bool Execute(const WriterTask &task);
  bool Purge();
  bool Sync(std::uint64_t completed);
  void Fail(std::string error);
  std::filesystem::path m_directory;
  EventCache &m_cache;
  StorageCatalog &m_catalog;
  StorageWriterHooks m_hooks;
  StorageExpiry m_expiry;
  StoragePurger m_purger;
  bool m_spaceShort = false;  // only the writer thread
  std::function<void()> m_onFirstSpaceWait;
  bool m_reportedSpaceWait =
      false;  // only the cache producer invokes the handler
  mutable std::mutex m_mutex;
  std::condition_variable m_workCv, m_drainCv;
  std::deque<WriterTask> m_tasks;
  std::thread m_thread;
  bool m_started = false, m_stopped = false, m_sleeping = false, m_wake = false;
  bool m_drain = false;
  bool m_dataPending = false, m_gathering = false;
  bool m_registered = false;
  std::string m_error;
  std::atomic<bool> m_stop{false}, m_failed{false};
  std::atomic<std::uint64_t> m_events{0}, m_syncs{0}, m_bytes{0}, m_writes{0},
      m_maxAge{0};
  std::unique_ptr<BinlogFileWriter> m_file;
  std::string m_name;
  WriteRange m_range;
  bool m_dirty = false;
  bool m_moreWork = false;
  std::uint64_t m_syncedEvents = 0;
  std::uint64_t m_createdPrefix = 0;
};
}  // namespace binlog_streamer
