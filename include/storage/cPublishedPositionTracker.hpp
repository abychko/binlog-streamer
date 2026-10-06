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

#include "storage/eWaitOutcome.hpp"
#include "storage/eWaitStyle.hpp"
#include "storage/sPublishedPosition.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace binlog_streamer {

// The file is published as a number that changes with the file, so a reader can
// compare marks without the mutex.
struct PublishedMark {
  std::uint64_t file = 0;
  std::uint64_t position = 0;
};

// Advance() is the sole writer. fileName and position are written together
// under one mutex; the mark is also published through a sequence counter, so
// Mark() reads a matching pair without it.
class PublishedPositionTracker {
 public:
  // The caller is trusted to only move forward - within one file, or to
  // a new file only once its header is durable; Wait() depends on this.
  void Advance(const std::string &fileName, std::uint64_t position);

  PublishedPosition Current() const;

  PublishedMark Mark() const;

  PublishedMark Locate(const std::string &fileName, bool &published) const;

  // target's file name must be one this tracker has already published
  // - otherwise "different file" can't be told from "never published".
  WaitOutcome Wait(const PublishedPosition &target,
                   std::chrono::milliseconds timeout,
                   WaitStyle style = WaitStyle::PollFirst) const;

 private:
  // True once m_current has moved past target; caller must already
  // hold m_mutex.
  bool HasAdvancedPast(const PublishedPosition &target) const;

  static constexpr std::chrono::microseconds POLL_WINDOW{200};
  static constexpr std::chrono::microseconds POLL_INTERVAL{50};

  mutable std::mutex m_mutex;
  mutable std::condition_variable m_cv;
  PublishedPosition m_current;
  // Odd while Advance() is changing the two below; Mark() retries when it
  // saw an odd value or the value changed while it read them.
  std::atomic<std::uint64_t> m_sequence{0};
  std::atomic<std::uint64_t> m_file{0};
  std::atomic<std::uint64_t> m_position{0};
  mutable unsigned m_blockedWaiters = 0;
};

}  // namespace binlog_streamer
