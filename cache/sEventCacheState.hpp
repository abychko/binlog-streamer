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
#include <sys/mman.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "cache/sCacheCounters.hpp"
#include "cache/sCacheHooks.hpp"
#include "cache/sCacheSlot.hpp"
#include "sCacheFile.hpp"
namespace binlog_streamer {
struct EventCacheState {
  ~EventCacheState() {
    if (reservationSize != 0) munmap(reservation, reservationSize);
  }
  std::uint8_t *reservation = nullptr;
  std::size_t reservationSize = 0;
  const std::atomic<bool> *stop = nullptr;
  CacheHooks hooks;
  std::chrono::steady_clock::duration window =
      std::chrono::steady_clock::duration::max();
  std::chrono::steady_clock::time_point lastAppendAt{};
  std::unique_ptr<CacheSlot[]> slots;
  std::vector<std::size_t> free;
  std::deque<std::size_t> fifo;
  std::deque<CacheFile> files;
  std::size_t frontId = 0;
  std::optional<std::size_t> current;
  mutable std::mutex mutex;
  std::condition_variable spaceCv, handlersFinished;
  std::function<void()> spaceWaitHandler;
  std::size_t activeHandlers = 0;
  std::size_t spaceWaiters = 0;
  bool aborted = false;
  CacheCounters counters;
};
}  // namespace binlog_streamer
