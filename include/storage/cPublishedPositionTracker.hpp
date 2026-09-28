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

// Which file is published, as a number that changes each time the file
// does, and how far into it. Comparing numbers needs no file name, so a
// reader that already knows which number its own file has can check it
// without taking the tracker's mutex.
struct PublishedMark {
  std::uint64_t file = 0;
  std::uint64_t position = 0;
};

// Shared between the sink (Advance(), sole writer) and every reader
// (Wait()/Current()/Mark()). fileName and position are always written
// together under one mutex, to avoid a reader observing a torn pair; the
// mark is also published through a sequence counter, so Mark() reads a
// matching pair without the mutex.
class PublishedPositionTracker {
 public:
  // The caller is trusted to only move forward - within one file, or to
  // a new file only once its header is durable; Wait() depends on this.
  void Advance(const std::string &fileName, std::uint64_t position);

  PublishedPosition Current() const;

  // Never blocks on the writer's mutex.
  PublishedMark Mark() const;

  // The current mark, and whether fileName is the file it is in.
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

  // How long a PollFirst reader polls before blocking. A stream
  // publishing faster than one window never makes Advance() wake anyone.
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
  // Readers blocked on m_cv right now; changed and read only under m_mutex.
  mutable unsigned m_blockedWaiters = 0;
};

}  // namespace binlog_streamer
