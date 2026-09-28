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

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>
#include "cache/cEventCache.hpp"
#include "cache/hCacheDefaults.hpp"
namespace binlog_streamer {
namespace {
using namespace std::chrono_literals;
constexpr auto S = CACHE_SEGMENT_SIZE;
template <class Predicate>
bool Until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(100us);
  }
  return true;
}
TEST(EventCacheWaitTest,
     RangesReferenceSlotsAndAdvanceFromWrittenAcrossNonzeroBase) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  std::vector<std::uint8_t> bytes(S, 0x37);
  cache->BeginFile("data", 37);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  WriteRange range;
  ASSERT_TRUE(cache->NextUnwritten("data", range));
  EXPECT_NE(range.bytes.data(), bytes.data());
  EXPECT_EQ(range.offset, 37u);
  EXPECT_EQ(range.bytes.size(), S);
  EXPECT_TRUE(
      std::equal(range.bytes.begin(), range.bytes.end(), bytes.begin()));
  const auto *address = range.bytes.data();
  cache->MarkWritten("data", 137);
  ASSERT_TRUE(cache->NextUnwritten("data", range));
  EXPECT_EQ(range.bytes.data(), address + 100);
  EXPECT_EQ(range.bytes.size(), S - 100);
  cache->MarkWritten("data", 37 + S);
  ASSERT_TRUE(cache->NextUnwritten("data", range));
  EXPECT_EQ(range.offset, 37 + S);
  EXPECT_EQ(range.bytes.size(), S);
  cache->MarkWritten("data", 37 + 2 * S);
  EXPECT_FALSE(cache->NextUnwritten("data", range));
  EXPECT_EQ(cache->Counters().written, 2 * S);
  EXPECT_EQ(cache->Counters().maxUnwritten, 2 * S);
}
TEST(EventCacheWaitTest,
     MarkWrittenWakesAtomicAdmissionAndNonblockingAppendStillRefuses) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  std::vector<std::uint8_t> bytes(S, 0x41);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  const auto before = cache->Counters();
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
  auto pending = std::async(std::launch::async,
                            [&] { return cache->AppendOrWait(bytes); });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > 0; }));
  EXPECT_EQ(cache->Counters().appended, 2 * S);
  cache->MarkWritten("data", S);
  EXPECT_EQ(pending.wait_for(1s), std::future_status::ready);
  cache->Abort();
  EXPECT_EQ(pending.get(), AppendOutcome::Appended);
  EXPECT_GT(cache->Counters().spaceWaitNanoseconds, 0u);
  EXPECT_EQ(cache->Counters().occupied, 2u);
}
TEST(EventCacheWaitTest, LastUnpinWakesBeforePollingDeadline) {
  std::atomic<bool> stop{false};
  std::string error;
  std::binary_semaphore pinned{0}, release{0};
  CacheHooks hooks;
  hooks.beforeCopy = [&] {
    pinned.release();
    release.acquire();
  };
  auto cache = EventCache::Reserve(2 * S, stop, error, hooks);
  ASSERT_TRUE(cache);
  std::vector<std::uint8_t> bytes(S, 0x51), out(2 * S);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S);
  auto reader =
      std::async(std::launch::async, [&] { cache->Read("data", 0, out); });
  pinned.acquire();
  auto pending = std::async(std::launch::async,
                            [&] { return cache->AppendOrWait(bytes); });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > 0; }));
  release.release();
  EXPECT_EQ(pending.wait_for(30ms), std::future_status::ready);
  reader.get();
  cache->MarkWritten("data", 2 * S);
  EXPECT_EQ(pending.get(), AppendOutcome::Appended);
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](auto b) { return b == 0x51; }));
}
TEST(EventCacheWaitTest, SignalStopIsPolledWithinOneHundredMilliseconds) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  std::vector<std::uint8_t> bytes(S, 0x61);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  auto pending = std::async(std::launch::async,
                            [&] { return cache->AppendOrWait(bytes); });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > 0; }));
  stop.store(true);
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::ready);
  cache->Abort();  // Also releases an incorrectly unbounded wait on failure.
  EXPECT_EQ(pending.get(), AppendOutcome::Stopped);
  EXPECT_EQ(cache->Counters().appended, 2 * S);
}
TEST(EventCacheWaitTest, AbortWakesImmediatelyAndRejectsAllFutureAppends) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  std::vector<std::uint8_t> bytes(S, 0x71);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  auto pending = std::async(std::launch::async,
                            [&] { return cache->AppendOrWait(bytes); });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > 0; }));
  cache->Abort();
  EXPECT_EQ(pending.wait_for(30ms), std::future_status::ready);
  cache->MarkWritten("data",
                     S);  // Bounded cleanup even without Abort's notification.
  EXPECT_EQ(pending.get(), AppendOutcome::Stopped);
  const auto before = cache->Counters();
  EXPECT_EQ(cache->Append(bytes), AppendOutcome::Stopped);
  EXPECT_EQ(cache->AppendOrWait(bytes), AppendOutcome::Stopped);
  EXPECT_EQ(cache->Counters(), before);
}
TEST(EventCacheWaitTest,
     RemovingHandlerWaitsForActiveCallAndPreventsLaterCalls) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  const std::vector<std::uint8_t> bytes(S, 0x33);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  std::binary_semaphore entered{0}, release{0};
  std::atomic<int> calls{0};
  cache->SetSpaceWaitHandler([&] {
    ++calls;
    EXPECT_EQ(cache->Counters().occupied, 2u);  // The index mutex is not held.
    entered.release();
    release.acquire();
  });
  auto producer = std::async(std::launch::async,
                             [&] { return cache->AppendOrWait(bytes); });
  entered.acquire();
  auto removal =
      std::async(std::launch::async, [&] { cache->SetSpaceWaitHandler({}); });
  EXPECT_EQ(removal.wait_for(10ms), std::future_status::timeout);
  release.release();
  EXPECT_EQ(removal.wait_for(1s), std::future_status::ready);
  removal.get();
  cache->MarkWritten("data", S);
  EXPECT_EQ(producer.get(), AppendOutcome::Appended);
  const auto waits = cache->Counters().spaceWaits;
  auto second = std::async(std::launch::async,
                           [&] { return cache->AppendOrWait(bytes); });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > waits; }));
  cache->Abort();
  EXPECT_EQ(second.get(), AppendOutcome::Stopped);
  EXPECT_EQ(calls.load(), 1);
}

TEST(EventCacheWaitTest, OversizedWaitingAppendReturnsWithoutWaiting) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  const auto before = cache->Counters();
  EXPECT_EQ(cache->AppendOrWait(std::vector<std::uint8_t>(S + 1)),
            AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
}
}  // namespace
}  // namespace binlog_streamer
