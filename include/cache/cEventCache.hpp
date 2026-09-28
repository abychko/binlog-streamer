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
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include "cache/eAppendOutcome.hpp"
#include "cache/sCacheCounters.hpp"
#include "cache/sCacheHooks.hpp"
#include "cache/sCacheReadResult.hpp"
#include "cache/sWriteRange.hpp"
#include "cache/sWriteSnapshot.hpp"
namespace binlog_streamer {
struct EventCacheState;

// One producer calls BeginFile/Append/EndFile. Readers and MarkWritten may
// run concurrently. stop and this object must outlive all operations.
class EventCache {
 public:
  // Without a window, automatic expiration is disabled.
  static std::optional<EventCache> Reserve(std::uint64_t maxSize,
                                           const std::atomic<bool> &stop,
                                           std::string &error);
  static std::optional<EventCache> Reserve(std::uint64_t maxSize,
                                           const std::atomic<bool> &stop,
                                           std::string &error,
                                           CacheHooks hooks);
  static std::optional<EventCache> Reserve(std::uint64_t maxSize,
                                           std::chrono::seconds window,
                                           const std::atomic<bool> &stop,
                                           std::string &error);
  static std::optional<EventCache> Reserve(std::uint64_t maxSize,
                                           std::chrono::seconds window,
                                           const std::atomic<bool> &stop,
                                           std::string &error,
                                           CacheHooks hooks);
  ~EventCache();
  EventCache(EventCache &&) noexcept;
  EventCache &operator=(EventCache &&) noexcept;
  EventCache(const EventCache &) = delete;
  EventCache &operator=(const EventCache &) = delete;

  // Names are unique within an instance.
  void BeginFile(std::string name, std::uint64_t base);
  // NoSpace and Stopped leave all bytes, indices and counters unchanged.
  AppendOutcome Append(std::span<const std::uint8_t> bytes);
  // stop/Abort cancel admission while waiting; oversized spans still
  // return NoSpace immediately instead of waiting.
  AppendOutcome AppendOrWait(std::span<const std::uint8_t> bytes);
  void Abort();
  // Must not call this from the handler or while holding its owner's
  // lock: removal waits for in-flight calls, and the handler itself runs
  // outside the index mutex.
  void SetSpaceWaitHandler(std::function<void()> handler);
  void EndFile();
  void MarkWritten(const std::string &file, std::uint64_t upTo);
  // One consumer only; returned bytes stay valid until MarkWritten
  // advances past them, so the cache must outlive the write.
  bool NextUnwritten(const std::string &file, WriteRange &range) const;
  // Samples completed events under the index lock; the producer
  // increments events only after its corresponding Append.
  WriteSnapshot SnapshotForWrite(
      const std::string &file, const std::atomic<std::uint64_t> &events) const;
  // NotCached carries NOT_CACHED_ANYWHERE so callers can bound their disk
  // read by their own catalog position instead.
  CacheReadResult Read(const std::string &file, std::uint64_t offset,
                       std::span<std::uint8_t> out) const;
  // Evicts only the expired+written+unpinned FIFO prefix; a
  // non-evictable entry stops the sweep.
  void EvictExpired(std::chrono::steady_clock::time_point now,
                    std::chrono::seconds window);
  CacheCounters Counters() const;

 private:
  AppendOutcome AppendImpl(std::span<const std::uint8_t> bytes, bool wait);
  explicit EventCache(std::unique_ptr<EventCacheState> state);
  std::unique_ptr<EventCacheState> m_state;
};
}  // namespace binlog_streamer
