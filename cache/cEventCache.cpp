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

#include "cache/cEventCache.hpp"
#include "cache/hCacheDefaults.hpp"
#include "sCacheReadPiece.hpp"
#include "sEventCacheState.hpp"

#include <sys/mman.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace binlog_streamer {
namespace {

void *ReserveMemory(std::size_t size) {
  return mmap(nullptr, size, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

CacheFile *FindFile(EventCacheState &state, const std::string &name) {
  const auto it =
      std::find_if(state.files.rbegin(), state.files.rend(),
                   [&](const CacheFile &file) { return file.name == name; });
  return it == state.files.rend() ? nullptr : &*it;
}

void RemoveEmptyFiles(EventCacheState &state) {
  while (!state.files.empty() && state.files.front().closed &&
         state.files.front().slots.empty()) {
    state.files.pop_front();
    ++state.frontId;
  }
  state.counters.files = state.files.size();
}

void ReleaseSlot(EventCacheState &state, std::size_t index) {
  state.free.push_back(index);
  --state.counters.occupied;
}

// Retiring the whole prefix preserves one contiguous cached interval;
// readers already copying it keep ownership until their last pin.
void EvictThrough(EventCacheState &state, std::size_t victim) {
  auto &chosen = state.slots[victim];
  auto &file = state.files.at(chosen.file - state.frontId);
  const auto boundary =
      chosen.offset + chosen.appended.load(std::memory_order_acquire);
  while (!file.slots.empty()) {
    const auto index = file.slots.front();
    auto &slot = state.slots[index];
    if (slot.offset >= boundary) break;
    file.slots.pop_front();
    slot.retired = true;
    state.counters.evictedForSpace +=
        slot.appended.load(std::memory_order_acquire);
    if (slot.pins == 0) ReleaseSlot(state, index);
  }
  file.firstCached = boundary;
  std::erase_if(state.fifo,
                [&](std::size_t index) { return state.slots[index].retired; });
  RemoveEmptyFiles(state);
}

std::chrono::steady_clock::duration WindowDuration(
    std::chrono::seconds window) {
  using Duration = std::chrono::steady_clock::duration;
  if (window >=
      std::chrono::duration_cast<std::chrono::seconds>(Duration::max()))
    return Duration::max();
  if (window.count() < 0)
    throw std::invalid_argument("cache window must not be negative");
  return std::chrono::duration_cast<Duration>(window);
}

int ReturnPages(EventCacheState &state, std::size_t index) {
  auto *address = state.slots[index].address;
  if (state.hooks.returnPages)
    return state.hooks.returnPages(address, CACHE_SEGMENT_SIZE);
#if defined(__APPLE__)
  return madvise(address, CACHE_SEGMENT_SIZE, MADV_FREE);
#else
  return madvise(address, CACHE_SEGMENT_SIZE, MADV_DONTNEED);
#endif
}

bool WindowDue(const EventCacheState &state,
               std::chrono::steady_clock::time_point now,
               std::chrono::steady_clock::duration window) {
  return !state.fifo.empty() &&
         now - state.slots[state.fifo.front()].lastAppendAt > window;
}

void ReturnExpired(EventCacheState &state, std::unique_lock<std::mutex> &lock,
                   std::chrono::steady_clock::time_point now,
                   std::chrono::steady_clock::duration window) {
  // Bounded stack batches avoid allocation and keep retired slots unavailable
  // until every page-return call for the batch has completed outside the lock.
  std::array<std::size_t, 64> pending{};
  while (WindowDue(state, now, window)) {
    std::size_t count = 0;
    while (count < pending.size() && WindowDue(state, now, window)) {
      const auto index = state.fifo.front();
      auto &slot = state.slots[index];
      auto &file = state.files.at(slot.file - state.frontId);
      const auto bytes = slot.appended.load(std::memory_order_acquire);
      const auto end = slot.offset + bytes;
      if (slot.pins != 0 || end > file.written) break;
      // During an unlocked Append, the old tail still ends at the published
      // boundary even if a new empty slot has been registered - protect that
      // destination too, not just the eventual open tail.
      if (!file.closed && end >= file.appended) break;
      file.firstCached = end;
      file.slots.pop_front();
      state.fifo.pop_front();
      slot.retired = true;
      state.counters.evictedForWindow += bytes;
      pending[count++] = index;
    }
    if (count == 0) return;
    RemoveEmptyFiles(state);
    lock.unlock();
    std::uint64_t errors = 0;
    for (std::size_t i = 0; i < count; ++i)
      if (ReturnPages(state, pending[i]) != 0) ++errors;
    lock.lock();
    state.counters.pagesReturnedCalls += count;
    state.counters.pagesReturnedErrors += errors;
    for (std::size_t i = 0; i < count; ++i) ReleaseSlot(state, pending[i]);
  }
}

void Unpin(EventCacheState &state, std::span<const CacheReadPiece> pieces) {
  bool released = false;
  for (const auto &piece : pieces) {
    auto &slot = state.slots[piece.slot];
    if (--slot.pins == 0) {
      released = true;
      --state.counters.pinned;
      if (slot.retired) ReleaseSlot(state, piece.slot);
    }
  }
  if (released && state.spaceWaiters > 0) state.spaceCv.notify_all();
}

}  // namespace

EventCache::EventCache(std::unique_ptr<EventCacheState> state)
    : m_state(std::move(state)) {}
EventCache::~EventCache() = default;
EventCache::EventCache(EventCache &&) noexcept = default;
EventCache &EventCache::operator=(EventCache &&) noexcept = default;

std::optional<EventCache> EventCache::Reserve(std::uint64_t maxSize,
                                              const std::atomic<bool> &stop,
                                              std::string &error) {
  return Reserve(maxSize, stop, error, {});
}

std::optional<EventCache> EventCache::Reserve(std::uint64_t maxSize,
                                              const std::atomic<bool> &stop,
                                              std::string &error,
                                              CacheHooks hooks) {
  return Reserve(maxSize, std::chrono::seconds::max(), stop, error,
                 std::move(hooks));
}

std::optional<EventCache> EventCache::Reserve(std::uint64_t maxSize,
                                              std::chrono::seconds window,
                                              const std::atomic<bool> &stop,
                                              std::string &error) {
  return Reserve(maxSize, window, stop, error, {});
}

std::optional<EventCache> EventCache::Reserve(std::uint64_t maxSize,
                                              std::chrono::seconds window,
                                              const std::atomic<bool> &stop,
                                              std::string &error,
                                              CacheHooks hooks) {
  error.clear();
  if (window.count() < 0) {
    error = "cache window must not be negative";
    return std::nullopt;
  }
  const auto count = maxSize / CACHE_SEGMENT_SIZE;
  if (count < 2 ||
      count > std::numeric_limits<std::size_t>::max() / CACHE_SEGMENT_SIZE) {
    error =
        "cache reservation requires at least two segments and an addressable "
        "size";
    return std::nullopt;
  }
  try {
    auto state = std::make_unique<EventCacheState>();
    state->stop = &stop;
    state->window = WindowDuration(window);
    state->hooks = std::move(hooks);
    const auto size = static_cast<std::size_t>(count) * CACHE_SEGMENT_SIZE;
    void *address =
        state->hooks.reserve ? state->hooks.reserve(size) : ReserveMemory(size);
    if (address == MAP_FAILED) {
      error = "cache reservation failed: " +
              std::error_code(errno, std::generic_category()).message();
      return std::nullopt;
    }
    state->reservation = static_cast<std::uint8_t *>(address);
    state->reservationSize = size;
    state->counters.capacity = static_cast<std::size_t>(count);
    state->slots =
        std::make_unique<CacheSlot[]>(static_cast<std::size_t>(count));
    state->free.reserve(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
      state->slots[i].address = state->reservation + i * CACHE_SEGMENT_SIZE;
      state->free.push_back(static_cast<std::size_t>(count) - 1 - i);
    }
    return EventCache(std::move(state));
  } catch (const std::bad_alloc &) {
    error = "cache metadata allocation failed";
    return std::nullopt;
  }
}

void EventCache::BeginFile(std::string name, std::uint64_t base) {
  auto &state = *m_state;
  std::unique_lock lock(state.mutex);
  if (state.current || name.empty() || FindFile(state, name))
    throw std::invalid_argument("cache requires a new file name after EndFile");
  state.files.push_back(CacheFile{std::move(name), base, base, base, base, {}});
  state.current = state.frontId + state.files.size() - 1;
  state.counters.files = state.files.size();
  lock.unlock();
  if (state.hooks.afterBeginFile) state.hooks.afterBeginFile();
}

AppendOutcome EventCache::Append(std::span<const std::uint8_t> bytes) {
  return AppendImpl(bytes, false);
}

AppendOutcome EventCache::AppendOrWait(std::span<const std::uint8_t> bytes) {
  return AppendImpl(bytes, true);
}

AppendOutcome EventCache::AppendImpl(std::span<const std::uint8_t> bytes,
                                     bool wait) {
  auto &state = *m_state;
  std::unique_lock lock(state.mutex);
  if (state.aborted || state.stop->load(std::memory_order_acquire))
    return AppendOutcome::Stopped;
  // Enforced in every build, not just an assert: an oversized chunk must
  // leave state unchanged.
  if (bytes.size() > CACHE_SEGMENT_SIZE) return AppendOutcome::NoSpace;
  if (!state.current)
    throw std::logic_error("cache Append requires an open file");
  auto &file = state.files.at(*state.current - state.frontId);
  if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - file.appended)
    return AppendOutcome::NoSpace;
  if (bytes.empty()) return AppendOutcome::Appended;

  std::optional<std::size_t> tail;
  std::size_t remaining = 0;
  if (!file.slots.empty()) {
    const auto index = file.slots.back();
    const auto used =
        state.slots[index].appended.load(std::memory_order_acquire);
    if (used < CACHE_SEGMENT_SIZE) {
      tail = index;
      remaining = CACHE_SEGMENT_SIZE - used;
    }
  }
  // A bounded chunk needs at most one new slot besides its existing tail;
  // find a victim without mutating anything, so a failed admission is
  // retryable.
  const bool needsSlot = bytes.size() > remaining;
  std::optional<std::size_t> victim;
  bool requestedSpace = false;
  while (needsSlot && state.free.empty()) {
    for (const auto index : state.fifo) {
      const auto &slot = state.slots[index];
      if (tail && index == *tail) continue;
      if (slot.pins == 0 &&
          slot.offset + slot.appended.load(std::memory_order_acquire) <=
              state.files.at(slot.file - state.frontId).written) {
        victim = index;
        break;
      }
    }
    if (victim) break;
    if (!wait) return AppendOutcome::NoSpace;
    if (!requestedSpace) {
      requestedSpace = true;
      if (auto handler = state.spaceWaitHandler) {
        ++state.activeHandlers;
        lock.unlock();
        try {
          handler();
        } catch (...) {
          lock.lock();
          --state.activeHandlers;
          state.handlersFinished.notify_all();
          throw;
        }
        lock.lock();
        --state.activeHandlers;
        state.handlersFinished.notify_all();
        if (state.aborted || state.stop->load(std::memory_order_acquire))
          return AppendOutcome::Stopped;
        // The writer may have freed space before we reacquired the lock.
        continue;
      }
    }
    ++state.spaceWaiters;
    ++state.counters.spaceWaits;
    const auto started = std::chrono::steady_clock::now();
    state.spaceCv.wait_for(lock, std::chrono::milliseconds(50));
    state.counters.spaceWaitNanoseconds += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    --state.spaceWaiters;
    if (state.aborted || state.stop->load(std::memory_order_acquire))
      return AppendOutcome::Stopped;
  }
  // The stop decision is made before any mutation, including retirement.
  if (state.aborted || state.stop->load(std::memory_order_acquire))
    return AppendOutcome::Stopped;
  if (victim) EvictThrough(state, *victim);

  std::array<CacheReadPiece, 2> copies{};
  std::size_t copyCount = 0;
  std::size_t consumed = 0;
  if (tail) {
    const auto used =
        state.slots[*tail].appended.load(std::memory_order_acquire);
    consumed = std::min(bytes.size(), remaining);
    copies[copyCount++] = {*tail, used, consumed};
  }
  if (needsSlot) {
    const auto index = state.free.back();
    state.free.pop_back();
    auto &slot = state.slots[index];
    slot.file = *state.current;
    slot.offset = file.appended + consumed;
    slot.retired = false;
    slot.appended.store(0, std::memory_order_release);
    file.slots.push_back(index);
    state.fifo.push_back(index);
    ++state.counters.occupied;
    copies[copyCount++] = {index, 0, bytes.size() - consumed};
  }
  // The producer owns these destinations. Readers can only pin the previously
  // published prefix; no other producer may admit or retire slots meanwhile.
  lock.unlock();
  if (state.hooks.beforeAppendCopy) state.hooks.beforeAppendCopy();
  std::size_t inputOffset = 0;
  for (std::size_t i = 0; i < copyCount; ++i) {
    const auto &copy = copies[i];
    std::memcpy(state.slots[copy.slot].address + copy.offset,
                bytes.data() + inputOffset, copy.length);
    inputOffset += copy.length;
  }
  const auto now =
      state.hooks.now ? state.hooks.now() : std::chrono::steady_clock::now();
  lock.lock();
  for (std::size_t i = 0; i < copyCount; ++i) {
    const auto &copy = copies[i];
    auto &slot = state.slots[copy.slot];
    slot.lastAppendAt = now;
    slot.appended.store(static_cast<std::uint32_t>(copy.offset + copy.length),
                        std::memory_order_release);
  }
  file.appended += bytes.size();
  state.counters.appended += bytes.size();
  state.counters.maxUnwritten =
      std::max(state.counters.maxUnwritten,
               state.counters.appended - state.counters.written);
  state.lastAppendAt = now;
  // Reuse the timestamp already taken above for a consistent WindowDue
  // decision.
  if (needsSlot && WindowDue(state, now, state.window))
    ReturnExpired(state, lock, now, state.window);
  return AppendOutcome::Appended;
}

void EventCache::SetSpaceWaitHandler(std::function<void()> handler) {
  auto &state = *m_state;
  std::unique_lock lock(state.mutex);
  state.spaceWaitHandler = {};
  state.handlersFinished.wait(lock, [&] { return state.activeHandlers == 0; });
  state.spaceWaitHandler = std::move(handler);
}

void EventCache::Abort() {
  std::lock_guard lock(m_state->mutex);
  m_state->aborted = true;
  if (m_state->spaceWaiters > 0) m_state->spaceCv.notify_all();
}

void EventCache::EndFile() {
  std::lock_guard lock(m_state->mutex);
  if (m_state->current)
    m_state->files.at(*m_state->current - m_state->frontId).closed = true;
  m_state->current.reset();
  RemoveEmptyFiles(*m_state);
}

void EventCache::MarkWritten(const std::string &name, std::uint64_t upTo) {
  std::unique_lock lock(m_state->mutex);
  auto *file = FindFile(*m_state, name);
  if (!file || upTo < file->written || upTo > file->appended)
    throw std::invalid_argument(
        "cache written position must advance within appended data");
  m_state->counters.written += upTo - file->written;
  file->written = upTo;
  if (WindowDue(*m_state, m_state->lastAppendAt, m_state->window))
    ReturnExpired(*m_state, lock, m_state->lastAppendAt, m_state->window);
  if (m_state->spaceWaiters > 0) m_state->spaceCv.notify_all();
}

CacheReadResult EventCache::Read(const std::string &name, std::uint64_t offset,
                                 std::span<std::uint8_t> out) const {
  auto &state = *m_state;
  std::unique_lock lock(state.mutex);
  auto *file = FindFile(state, name);
  const auto firstCached = file ? file->firstCached : NOT_CACHED_ANYWHERE;
  if (!file || offset < firstCached) {
    lock.unlock();
    if (state.hooks.afterMiss) state.hooks.afterMiss();
    return CacheReadResult{CacheReadResult::NotCached{firstCached}};
  }
  if (offset >= file->appended || out.empty())
    return CacheReadResult{CacheReadResult::Copied{0}};
  const auto length = static_cast<std::size_t>(
      std::min<std::uint64_t>(out.size(), file->appended - offset));
  std::array<CacheReadPiece, 4> inlinePieces{};
  std::vector<CacheReadPiece> overflow;
  const auto pieceLimit = length / CACHE_SEGMENT_SIZE + 2;
  if (pieceLimit > inlinePieces.size()) overflow.resize(pieceLimit);
  std::span<CacheReadPiece> storage =
      overflow.empty() ? std::span<CacheReadPiece>(inlinePieces)
                       : std::span<CacheReadPiece>(overflow);
  std::size_t pieceCount = 0;
  auto index = static_cast<std::size_t>(
      (offset - file->base) / CACHE_SEGMENT_SIZE -
      (state.slots[file->slots.front()].offset - file->base) /
          CACHE_SEGMENT_SIZE);
  std::size_t left = length;
  while (left != 0) {
    const auto slotIndex = file->slots.at(index++);
    auto &slot = state.slots[slotIndex];
    const auto within = static_cast<std::size_t>(offset - slot.offset);
    const auto available =
        slot.appended.load(std::memory_order_acquire) - within;
    const auto take = std::min(left, available);
    storage[pieceCount++] = {slotIndex, within, take};
    if (slot.pins++ == 0) ++state.counters.pinned;
    offset += take;
    left -= take;
  }
  const auto pieces = storage.first(pieceCount);
  state.counters.maxPinned =
      std::max(state.counters.maxPinned, state.counters.pinned);
  lock.unlock();
  try {
    if (state.hooks.beforeCopy) state.hooks.beforeCopy();
    std::size_t copied = 0;
    for (const auto &piece : pieces) {
      std::memcpy(out.data() + copied,
                  state.slots[piece.slot].address + piece.offset, piece.length);
      copied += piece.length;
    }
  } catch (...) {
    lock.lock();
    Unpin(state, pieces);
    throw;
  }
  lock.lock();
  Unpin(state, pieces);
  state.counters.readFromCache += length;
  return CacheReadResult{CacheReadResult::Copied{length}};
}

WriteSnapshot EventCache::SnapshotForWrite(
    const std::string &name, const std::atomic<std::uint64_t> &events) const {
  std::lock_guard lock(m_state->mutex);
  WriteSnapshot snapshot;
  snapshot.completedEvents = events.load(std::memory_order_relaxed);
  const auto *file = FindFile(*m_state, name);
  snapshot.appended = file ? file->appended : 0;
  return snapshot;
}

bool EventCache::NextUnwritten(const std::string &name,
                               WriteRange &range) const {
  auto &state = *m_state;
  std::lock_guard lock(state.mutex);
  const auto *file = FindFile(state, name);
  if (!file || file->written == file->appended) return false;
  const auto index = static_cast<std::size_t>(
      (file->written - file->base) / CACHE_SEGMENT_SIZE -
      (state.slots[file->slots.front()].offset - file->base) /
          CACHE_SEGMENT_SIZE);
  const auto &slot = state.slots[file->slots.at(index)];
  const auto within = static_cast<std::size_t>(file->written - slot.offset);
  range.file = name;
  range.offset = file->written;
  range.bytes = {slot.address + within,
                 slot.appended.load(std::memory_order_acquire) - within};
  range.lastAppendAt = slot.lastAppendAt;
  return true;
}
void EventCache::EvictExpired(std::chrono::steady_clock::time_point now,
                              std::chrono::seconds window) {
  const auto duration = WindowDuration(window);
  std::unique_lock lock(m_state->mutex);
  if (WindowDue(*m_state, now, duration))
    ReturnExpired(*m_state, lock, now, duration);
}

CacheCounters EventCache::Counters() const {
  std::lock_guard lock(m_state->mutex);
  return m_state->counters;
}

}  // namespace binlog_streamer
