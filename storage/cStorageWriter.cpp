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

#include "storage/cStorageWriter.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <exception>
#include <utility>
#include "cache/cEventCache.hpp"
#include "storage/cBinlogFileWriter.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/hStorageDefaults.hpp"
namespace binlog_streamer {
StorageWriter::StorageWriter(std::filesystem::path directory, EventCache &cache,
                             StorageCatalog &catalog, StorageWriterHooks hooks,
                             std::function<void()> onFirstSpaceWait,
                             StorageExpiry expiry)
    : m_directory(std::move(directory)),
      m_cache(cache),
      m_catalog(catalog),
      m_hooks(std::move(hooks)),
      m_expiry(std::move(expiry)),
      m_purger(m_directory, catalog),
      m_onFirstSpaceWait(std::move(onFirstSpaceWait)) {}
StorageWriter::~StorageWriter() { Stop(); }
void StorageWriter::Start() {
  {
    std::lock_guard lock(m_mutex);
    assert(!m_started && !m_stopped);
  }
  // Not held here: Wake() (called from this handler) takes it too, and
  // removal waits for an in-flight handler to finish.
  m_cache.SetSpaceWaitHandler([this] {
    Wake();
    if (!m_reportedSpaceWait) {
      m_reportedSpaceWait = true;
      if (m_onFirstSpaceWait) m_onFirstSpaceWait();
    }
  });
  m_registered = true;
  try {
    std::lock_guard lock(m_mutex);
    m_thread = std::thread([this] { Run(); });
    m_started = true;
  } catch (...) {
    m_cache.SetSpaceWaitHandler({});
    m_registered = false;
    throw;
  }
}
void StorageWriter::Stop() {
  {
    std::lock_guard lock(m_mutex);
    m_stopped = true;
    m_stop.store(true, std::memory_order_release);
    WakeLocked();
  }
  if (m_registered) {
    m_cache.SetSpaceWaitHandler({});
    m_registered = false;
  }
  if (m_thread.joinable()) m_thread.join();
}
void StorageWriter::WakeLocked() {
  m_wake = true;
  if (m_sleeping) m_workCv.notify_one();
}
void StorageWriter::Wake() {
  std::lock_guard lock(m_mutex);
  WakeLocked();
}
void StorageWriter::WakeForData() {
  std::lock_guard lock(m_mutex);
  if (m_sleeping && !m_gathering)
    WakeLocked();
  else
    m_dataPending = true;
}
void StorageWriter::Post(WriterTask task) {
  std::lock_guard lock(m_mutex);
  if (Failed() || m_stopped) return;
  task.completedEvents = m_events.load(std::memory_order_relaxed);
  m_tasks.push_back(std::move(task));
  WakeLocked();
}
void StorageWriter::PostCreate(std::string name,
                               std::span<const std::uint8_t> fde,
                               std::span<const std::uint8_t> pge) {
  Post({WriterTaskKind::Create,
        std::move(name),
        {fde.begin(), fde.end()},
        {pge.begin(), pge.end()}});
}
void StorageWriter::PostOpenExisting(std::string name) {
  Post({WriterTaskKind::OpenExisting, std::move(name), {}, {}});
}
void StorageWriter::PostCloseAbandoned(std::string name) {
  Post({WriterTaskKind::CloseAbandoned, std::move(name), {}, {}});
}
void StorageWriter::PostPurge() { Post({WriterTaskKind::Purge, {}, {}, {}}); }
void StorageWriter::PostClose(std::string name) {
  Post({WriterTaskKind::Close, std::move(name), {}, {}});
}
std::string StorageWriter::LastError() const {
  std::lock_guard lock(m_mutex);
  return m_error;
}
bool StorageWriter::DrainAndSync() {
  std::unique_lock lock(m_mutex);
  if (Failed() || !m_started || m_stopped) return false;
  m_drain = true;
  WakeLocked();
  m_drainCv.wait(lock, [this] {
    return !m_drain || Failed() || m_stop.load(std::memory_order_acquire);
  });
  return !Failed() && !m_stop.load(std::memory_order_acquire);
}
void StorageWriter::Fail(std::string error) {
  {
    std::lock_guard lock(m_mutex);
    m_error = std::move(error);
    m_failed.store(true, std::memory_order_release);
  }
  m_cache.Abort();
  m_drainCv.notify_all();
}
bool StorageWriter::Sync(std::uint64_t completed) {
  if (!m_file || !m_dirty) return true;
  if (m_hooks.beforeSync) m_hooks.beforeSync();
  std::string error;
  if (!m_file->Sync(error)) {
    Fail(std::move(error));
    return false;
  }
  m_dirty = false;
  m_syncedEvents = completed;
  m_syncs.fetch_add(1, std::memory_order_relaxed);
  return true;
}
bool StorageWriter::WritePending(WriteSnapshot &snapshot) {
  m_moreWork = false;
  if (!m_file) {
    snapshot.completedEvents = m_syncedEvents;
    return true;
  }
  snapshot = m_cache.SnapshotForWrite(m_name, m_events);
  if (m_hooks.afterSnapshot) m_hooks.afterSnapshot();
  auto &range = m_range;
  while (!m_stop.load(std::memory_order_acquire) &&
         m_cache.NextUnwritten(m_name, range)) {
    if (range.offset >= snapshot.appended) {
      m_moreWork = true;
      break;
    }
    range.bytes =
        range.bytes.first(static_cast<std::size_t>(std::min<std::uint64_t>(
            range.bytes.size(), snapshot.appended - range.offset)));
    if (range.offset < m_createdPrefix) {
      m_cache.MarkWritten(
          m_name, std::min(range.offset + range.bytes.size(), m_createdPrefix));
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - range.lastAppendAt)
                         .count();
    if (age > 0 && static_cast<std::uint64_t>(age) >
                       m_maxAge.load(std::memory_order_relaxed))
      m_maxAge.store(static_cast<std::uint64_t>(age),
                     std::memory_order_relaxed);
    std::string error;
    if (range.offset != m_file->Size()) {
      Fail("cache write offset differs from file length");
      return false;
    }
    if (m_hooks.beforeWrite && !m_hooks.beforeWrite(error)) {
      Fail(std::move(error));
      return false;
    }
    std::uint64_t calls = 0;
    const bool success = m_file->WriteDirect(range.bytes, error, &calls);
    m_writes.fetch_add(calls, std::memory_order_relaxed);
    if (!success) {
      Fail(std::move(error));
      return false;
    }
    m_dirty = true;
    m_cache.MarkWritten(m_name, range.offset + range.bytes.size());
    m_bytes.fetch_add(range.bytes.size(), std::memory_order_relaxed);
  }
  return !m_stop.load(std::memory_order_acquire);
}
bool StorageWriter::Purge() {
  PurgeLimits limits;
  if (m_expiry.sourceNow) {
    if (const auto now = m_expiry.sourceNow())
      limits.cutoff = StoragePurger::Cutoff(*now, m_expiry.period);
  }
  std::string error;
  if (m_expiry.disk) {
    std::uint64_t available = 0;
    if (m_hooks.measureAvailable) {
      if (!m_hooks.measureAvailable(available, error)) {
        Fail("measuring free space: " + error);
        return false;
      }
    } else {
      std::error_code code;
      const auto info = std::filesystem::space(m_directory, code);
      if (code) {
        Fail("measuring free space: " + m_directory.string() + ": " +
             code.message());
        return false;
      }
      available = info.available;
    }
    const auto &disk = *m_expiry.disk;
    limits.space = SpaceBudget{disk.highWatermark, disk.lowWatermark,
                               disk.minFreeSpace, available};
  }
  if (!limits.cutoff && !limits.space) return true;
  PurgeResult result;
  if (!m_purger.Purge(limits,
                      m_file ? std::string_view(m_name) : std::string_view(),
                      result, error)) {
    Fail("purging files: " + error);
    return false;
  }
  if ((!result.purged.empty() || !result.warning.empty()) && m_expiry.onPurged)
    m_expiry.onPurged(result);
  if (limits.space && result.spaceShort != m_spaceShort) {
    m_spaceShort = result.spaceShort;
    if (m_expiry.disk->onSpaceShortChange)
      m_expiry.disk->onSpaceShortChange(result);
  }
  return true;
}

bool StorageWriter::Execute(const WriterTask &task) {
  if (task.kind == WriterTaskKind::Purge) return Purge();
  std::string error;
  const auto path = (m_directory / task.name).string();
  if (task.kind == WriterTaskKind::Close) {
    if (!m_file || m_name != task.name) {
      Fail("close does not match the current file");
      return false;
    }
    WriteSnapshot finalSnapshot;
    if (!WritePending(finalSnapshot)) return false;
    if (!m_file->Flush(error) || !m_file->MarkClosed(error) ||
        !Sync(task.completedEvents)) {
      if (!Failed()) Fail(std::move(error));
      return false;
    }
    m_file.reset();
    m_name.clear();
    return Purge();
  }
  if (m_file) {
    Fail("opening a file before closing the current file");
    return false;
  }
  auto file = std::make_unique<BinlogFileWriter>();
  if (task.kind == WriterTaskKind::CloseAbandoned) {
    if (!file->OpenExisting(path, error) || !file->MarkClosed(error)) {
      Fail(std::move(error));
      return false;
    }
    return Purge();
  }
  m_createdPrefix = 0;
  if (task.kind == WriterTaskKind::Create) {
    if (m_expiry.disk) {
      const auto &disk = *m_expiry.disk;
      const auto limit = disk.maxSize > disk.recoveryReserve
                             ? disk.maxSize - disk.recoveryReserve
                             : 0;
      const auto used = m_catalog.TotalSizeBefore(task.name);
      if (used >= limit) {
        Fail("storage.disk.max_size reached: " + std::to_string(used) +
             " byte(s) stored before " + task.name +
             "; a new file needs less than " + std::to_string(limit) +
             " (max_size minus recovery_reserve)");
        return false;
      }
    }
    if (!file->Create(path, task.fde, task.pge, error) ||
        !BinlogIndexFile::Append((m_directory / INDEX_FILE_NAME).string(),
                                 task.name, error) ||
        !m_catalog.MarkOnDisk(task.name, error)) {
      Fail(std::move(error));
      return false;
    }
    m_createdPrefix = file->Size();
    const auto snapshot = m_cache.SnapshotForWrite(task.name, m_events);
    m_cache.MarkWritten(task.name,
                        std::min(m_createdPrefix, snapshot.appended));
  } else if (!file->OpenExisting(path, error)) {
    Fail(std::move(error));
    return false;
  }
  m_file = std::move(file);
  m_name = task.name;
  return true;
}
void StorageWriter::Run() {
  try {
    while (!m_stop.load(std::memory_order_acquire)) {
      // Snapshot before draining: events arriving during write/fsync must
      // not be erased from the next durability interval.
      WriteSnapshot snapshot;
      const auto bytesBefore = m_bytes.load(std::memory_order_relaxed);
      if (!WritePending(snapshot)) break;
      const bool wrote = m_bytes.load(std::memory_order_relaxed) != bytesBefore;
      auto completed = snapshot.completedEvents;
      {
        std::lock_guard lock(m_mutex);
        // A later file can already have completed events in the queue.
        // Syncing this file must not consume those events' interval.
        if (!m_tasks.empty())
          completed = std::min(completed, m_tasks.front().completedEvents);
      }
      if (completed - m_syncedEvents >= SYNC_EVENT_PERIOD && !Sync(completed))
        break;
      std::unique_lock lock(m_mutex);
      if (m_stop.load(std::memory_order_acquire)) break;
      if (!m_tasks.empty()) {
        auto task = std::move(m_tasks.front());
        m_tasks.pop_front();
        lock.unlock();
        if (!Execute(task)) break;
      } else if (m_drain) {
        lock.unlock();
        // The request may have arrived after the earlier empty lookup.
        // Its producer has now stopped: repeat the lookup before acking.
        WriteSnapshot finalSnapshot;
        if (!WritePending(finalSnapshot) ||
            !Sync(finalSnapshot.completedEvents))
          break;
        lock.lock();
        m_drain = false;
        m_drainCv.notify_all();
      } else if (m_wake) {
        m_wake = false;
      } else if (wrote || m_moreWork || m_dataPending) {
        // More bytes are likely to follow the ones just written: let them
        // gather, and sleep until woken only after a pass found nothing.
        m_dataPending = false;
        m_sleeping = m_gathering = true;
        m_workCv.wait_for(lock, WRITE_COALESCE, [this] {
          return m_wake || m_stop.load(std::memory_order_acquire);
        });
        m_sleeping = m_gathering = false;
        m_wake = false;
      } else {
        m_sleeping = true;
        if (m_hooks.beforeSleep) m_hooks.beforeSleep();
        m_workCv.wait(lock, [this] {
          return m_wake || m_stop.load(std::memory_order_acquire);
        });
        m_sleeping = false;
        m_wake = false;
      }
    }
  } catch (const std::exception &error) {
    Fail(error.what());
  }
  m_drainCv.notify_all();
}
}  // namespace binlog_streamer
