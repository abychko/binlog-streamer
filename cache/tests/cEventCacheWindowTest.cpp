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

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace binlog_streamer {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto S = CACHE_SEGMENT_SIZE;

class EventCacheWindowTest : public ::testing::Test {
 protected:
  Clock::time_point Now() const {
    return Clock::time_point(std::chrono::nanoseconds(ticks.load()));
  }
  void At(std::chrono::nanoseconds time) { ticks = time.count(); }
  void Reserve(std::size_t slots = 4, std::chrono::seconds window = 10s) {
    hooks.now = [&] {
      ++clockCalls;
      return Now();
    };
    if (!hooks.returnPages)
      hooks.returnPages = [&](void *, std::size_t size) {
        EXPECT_EQ(size, S);
        ++pageCalls;
        return 0;
      };
    cache = EventCache::Reserve(slots * S, window, stop, error, hooks);
    ASSERT_TRUE(cache) << error;
  }
  void Fill(unsigned count, bool written = true) {
    for (unsigned i = 0; i < count; ++i) {
      ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
      if (written) cache->MarkWritten("data", (i + 1) * S);
    }
  }
  std::uint64_t Miss(const std::string &file, std::uint64_t offset = 0) {
    std::array<std::uint8_t, 1> out{0xcc};
    const auto result = cache->Read(file, offset, out);
    const auto *miss = std::get_if<CacheReadResult::NotCached>(&result.value);
    EXPECT_NE(miss, nullptr);
    EXPECT_EQ(out[0], 0xcc);
    return miss ? miss->firstCached : 0;
  }
  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> ticks{0};
  std::atomic<unsigned> clockCalls{0}, pageCalls{0};
  CacheHooks hooks;
  std::string error;
  std::optional<EventCache> cache;
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(S, 0x57);
};

TEST_F(EventCacheWindowTest,
       StrictBoundaryKeepsExactAgeThenReturnsWrittenPrefix) {
  Reserve();
  cache->BeginFile("data", 0);
  Fill(3);
  const auto before = cache->Counters();
  At(10s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters(), before);
  At(10s + 1ns);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(cache->Counters().evictedForWindow, 2 * S);
  EXPECT_EQ(cache->Counters().evictedForSpace, 0u);
  EXPECT_EQ(cache->Counters().pagesReturnedCalls, 2u);
  EXPECT_EQ(pageCalls, 2u);
  EXPECT_EQ(Miss("data"), 2 * S);
  EXPECT_EQ(clockCalls, 3u);
}

TEST_F(EventCacheWindowTest, UnwrittenHeadAndEverythingBehindItRemainReadable) {
  Reserve();
  cache->BeginFile("data", 0);
  Fill(3, false);
  cache->MarkWritten("data", S - 1);
  At(11s);
  const auto before = cache->Counters();
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters(), before);
  std::vector<std::uint8_t> out(3 * S);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("data", 0, out).value).n,
      out.size());
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](auto b) { return b == 0x57; }));
}

TEST_F(EventCacheWindowTest, PinnedHeadStopsExpirationOfFollowingSlots) {
  std::binary_semaphore entered(0), release(0);
  std::atomic<bool> block{true};
  hooks.beforeCopy = [&] {
    if (block.exchange(false)) {
      entered.release();
      release.acquire();
    }
  };
  Reserve();
  cache->BeginFile("data", 0);
  Fill(3);
  std::vector<std::uint8_t> out(S);
  auto reader = std::async(std::launch::async,
                           [&] { return cache->Read("data", 0, out); });
  entered.acquire();
  At(11s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().occupied, 3u);
  EXPECT_EQ(cache->Counters().evictedForWindow, 0u);
  std::array<std::uint8_t, 1> check{};
  const auto first = cache->Read("data", 0, check);
  const auto second = cache->Read("data", S, check);
  EXPECT_TRUE(std::holds_alternative<CacheReadResult::Copied>(first.value));
  EXPECT_TRUE(std::holds_alternative<CacheReadResult::Copied>(second.value));
  release.release();
  EXPECT_EQ(std::get<CacheReadResult::Copied>(reader.get().value).n, S);
  EXPECT_EQ(out, bytes);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(pageCalls, 2u);
}

TEST_F(EventCacheWindowTest,
       IdleDoesNothingUntilNextEventAndMarkWrittenUsesAppendTime) {
  Reserve();
  cache->BeginFile("data", 0);
  Fill(2);
  const auto before = cache->Counters();
  At(11s);
  EXPECT_EQ(cache->Counters(), before);
  EXPECT_EQ(clockCalls, 2u);
  cache->MarkWritten("data", 2 * S);
  EXPECT_EQ(cache->Counters(), before);
  EXPECT_EQ(clockCalls, 2u);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(cache->Counters().evictedForWindow, 2 * S);
  EXPECT_EQ(clockCalls, 3u);
}

TEST_F(EventCacheWindowTest,
       MarkWrittenExpiresThePrefixAfterItsFinalBytesReachDisk) {
  Reserve();
  cache->BeginFile("data", 0);
  Fill(2, false);
  At(11s);
  ASSERT_EQ(cache->Append(std::span(bytes).first(7)), AppendOutcome::Appended);
  EXPECT_EQ(cache->Counters().evictedForWindow, 0u);
  cache->MarkWritten("data", 2 * S + 7);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(cache->Counters().evictedForWindow, 2 * S);
  EXPECT_EQ(Miss("data"), 2 * S);
  EXPECT_EQ(clockCalls, 3u);
}

TEST_F(EventCacheWindowTest,
       RecentAppendRefreshesAgeAndOpenTailKeepsItsAlignment) {
  Reserve();
  cache->BeginFile("data", 37);
  ASSERT_EQ(cache->Append(std::span(bytes).first(S - 17)),
            AppendOutcome::Appended);
  cache->MarkWritten("data", 37 + S - 17);
  At(9s);
  ASSERT_EQ(cache->Append(std::span(bytes).first(17)), AppendOutcome::Appended);
  cache->MarkWritten("data", 37 + S);
  cache->EndFile();
  At(11s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(cache->Counters().evictedForWindow, 0u);
  At(20s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().files, 0u);
  EXPECT_EQ(Miss("data"), NOT_CACHED_ANYWHERE);
  cache->BeginFile("next", 37);
  ASSERT_EQ(cache->Append(std::span(bytes).first(17)), AppendOutcome::Appended);
  cache->MarkWritten("next", 54);
  At(40s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  std::vector<std::uint8_t> out(S + 17);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("next", 37, out).value).n,
      out.size());
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](auto b) { return b == 0x57; }));
}

TEST_F(EventCacheWindowTest,
       ClosedFilesDisappearButOpenFileKeepsExactlyOneSlot) {
  Reserve();
  cache->BeginFile("data", 0);
  Fill(2);
  cache->EndFile();
  cache->BeginFile("open", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  cache->MarkWritten("open", S);
  At(11s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().files, 1u);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(Miss("data"), NOT_CACHED_ANYWHERE);
  EXPECT_EQ(pageCalls, 2u);
}

TEST_F(EventCacheWindowTest,
       SpaceEvictionNeverReturnsPagesAndMadviseErrorsAreNonfatal) {
  hooks.returnPages = [&](void *, std::size_t) {
    ++pageCalls;
    return -1;
  };
  Reserve(2);
  cache->BeginFile("data", 0);
  Fill(3);
  EXPECT_EQ(cache->Counters().evictedForSpace, S);
  EXPECT_EQ(pageCalls, 0u);
  cache->EndFile();
  At(11s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().occupied, 0u);
  EXPECT_EQ(cache->Counters().pagesReturnedCalls, 2u);
  EXPECT_EQ(cache->Counters().pagesReturnedErrors, 2u);
  cache->BeginFile("next", 0);
  EXPECT_EQ(cache->Append(bytes), AppendOutcome::Appended);
}

TEST_F(EventCacheWindowTest,
       ReturningPagesDoesNotHoldTheIndexLockOrExposeTheSlotForReuse) {
  std::binary_semaphore entered(0), release(0);
  hooks.returnPages = [&](void *address, std::size_t size) {
    entered.release();
    release.acquire();
    std::memset(address, 0xcc, size);
    return 0;
  };
  Reserve(2);
  cache->BeginFile("data", 0);
  Fill(1);
  cache->EndFile();
  cache->BeginFile("live", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  At(11s);
  auto evictor =
      std::async(std::launch::async, [&] { cache->EvictExpired(Now(), 10s); });
  entered.acquire();
  EXPECT_EQ(cache->Counters().occupied, 2u);
  EXPECT_EQ(Miss("data"), NOT_CACHED_ANYWHERE);
  std::vector<std::uint8_t> out(S);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("live", 0, out).value).n,
      S);
  EXPECT_EQ(out, bytes);
  EXPECT_EQ(cache->Append(bytes), AppendOutcome::NoSpace);
  release.release();
  evictor.get();
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("live", S, out).value).n,
      S);
  EXPECT_EQ(out, bytes);
}

TEST_F(EventCacheWindowTest,
       ConcurrentExpirationCannotRetireTheUnpublishedAppendDestination) {
  std::binary_semaphore entered(0), release(0);
  std::atomic<bool> block{false};
  hooks.beforeAppendCopy = [&] {
    if (block.load()) {
      entered.release();
      release.acquire();
    }
  };
  Reserve(3);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(std::span(bytes).first(S - 17)),
            AppendOutcome::Appended);
  cache->MarkWritten("data", S - 17);
  At(11s);
  block = true;
  auto producer =
      std::async(std::launch::async, [&] { return cache->Append(bytes); });
  entered.acquire();
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().evictedForWindow, 0u);
  release.release();
  EXPECT_EQ(producer.get(), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S - 17);
  std::vector<std::uint8_t> out(2 * S - 17);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("data", 0, out).value).n,
      out.size());
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](auto b) { return b == 0x57; }));
}

TEST_F(EventCacheWindowTest,
       ActualPageReturnHandlesMoreThanOneBatchAndAllowsReuse) {
  hooks.now = [&] { return Now(); };
  cache = EventCache::Reserve(70 * S, 10s, stop, error, hooks);
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  Fill(70);
  cache->EndFile();
  At(11s);
  cache->EvictExpired(Now(), 10s);
  EXPECT_EQ(cache->Counters().pagesReturnedCalls, 70u);
  EXPECT_EQ(cache->Counters().pagesReturnedErrors, 0u);
  EXPECT_EQ(cache->Counters().occupied, 0u);
  cache->BeginFile("new", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  std::vector<std::uint8_t> out(S);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("new", 0, out).value).n, S);
  EXPECT_EQ(out, bytes);
}
}  // namespace
}  // namespace binlog_streamer
