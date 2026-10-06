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

#include "storage/cPublishedPositionTracker.hpp"

#include <algorithm>
#include <thread>

namespace binlog_streamer {

void PublishedPositionTracker::Advance(const std::string &fileName,
                                       std::uint64_t position) {
  bool anyoneBlocked = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool newFile = m_current.fileName != fileName;
    if (newFile) m_current.fileName = fileName;
    m_current.position = position;

    // Release stores rather than a fence (ThreadSanitizer supports no
    // fences): a Mark() that reads either value also sees the odd count.
    const std::uint64_t sequence = m_sequence.load(std::memory_order_relaxed);
    m_sequence.store(sequence + 1, std::memory_order_relaxed);
    if (newFile)
      m_file.store(m_file.load(std::memory_order_relaxed) + 1,
                   std::memory_order_release);
    m_position.store(position, std::memory_order_release);
    m_sequence.store(sequence + 2, std::memory_order_release);

    anyoneBlocked = m_blockedWaiters != 0;
  }
  // notify_all(), not notify_one(): each waiter has a different target.
  if (anyoneBlocked) m_cv.notify_all();
}

PublishedPosition PublishedPositionTracker::Current() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_current;
}

PublishedMark PublishedPositionTracker::Mark() const {
  for (;;) {
    const std::uint64_t before = m_sequence.load(std::memory_order_acquire);
    if (before & 1) {
      std::this_thread::yield();
      continue;
    }
    // Acquire loads keep the second read of the count after both values.
    PublishedMark mark{m_file.load(std::memory_order_acquire),
                       m_position.load(std::memory_order_acquire)};
    if (m_sequence.load(std::memory_order_relaxed) == before) return mark;
  }
}

PublishedMark PublishedPositionTracker::Locate(const std::string &fileName,
                                               bool &published) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  published = m_current.fileName == fileName;
  return {m_file.load(std::memory_order_relaxed), m_current.position};
}

bool PublishedPositionTracker::HasAdvancedPast(
    const PublishedPosition &target) const {
  if (m_current.fileName != target.fileName) return true;
  return m_current.position > target.position;
}

WaitOutcome PublishedPositionTracker::Wait(const PublishedPosition &target,
                                           std::chrono::milliseconds timeout,
                                           WaitStyle style) const {
  const auto startedAt = std::chrono::steady_clock::now();
  std::uint64_t file = 0;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (HasAdvancedPast(target)) return WaitOutcome::Advanced;
    file = m_file.load(std::memory_order_relaxed);
  }

  const auto pollUntil =
      style == WaitStyle::Block
          ? startedAt
          : startedAt + std::min<std::chrono::steady_clock::duration>(
                            timeout, POLL_WINDOW);
  while (std::chrono::steady_clock::now() < pollUntil) {
    std::this_thread::sleep_for(POLL_INTERVAL);
    const PublishedMark mark = Mark();
    if (mark.file != file || mark.position > target.position)
      return WaitOutcome::Advanced;
  }

  const auto elapsed = std::chrono::steady_clock::now() - startedAt;
  const auto remaining = elapsed < timeout
                             ? timeout - elapsed
                             : std::chrono::steady_clock::duration::zero();

  std::unique_lock<std::mutex> lock(m_mutex);
  ++m_blockedWaiters;
  const bool advanced = m_cv.wait_for(
      lock, remaining, [this, &target] { return HasAdvancedPast(target); });
  --m_blockedWaiters;
  return advanced ? WaitOutcome::Advanced : WaitOutcome::TimedOut;
}

}  // namespace binlog_streamer
