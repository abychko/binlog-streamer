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
#include <sys/mman.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace binlog_streamer {
namespace {
constexpr auto S = CACHE_SEGMENT_SIZE;

std::vector<std::uint8_t> Pattern(std::size_t size, std::uint8_t salt) {
  std::vector<std::uint8_t> result(size);
  for (std::size_t i = 0; i < size; ++i)
    result[i] = static_cast<std::uint8_t>((i * 37 + i / 251 + salt) % 256);
  return result;
}

std::size_t Copied(const CacheReadResult &result) {
  const auto *copy = std::get_if<CacheReadResult::Copied>(&result.value);
  EXPECT_NE(copy, nullptr);
  return copy ? copy->n : 0;
}

std::uint64_t FirstCached(const CacheReadResult &result) {
  const auto *miss = std::get_if<CacheReadResult::NotCached>(&result.value);
  EXPECT_NE(miss, nullptr);
  return miss ? miss->firstCached : std::numeric_limits<std::uint64_t>::max();
}

class EventCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache = EventCache::Reserve(2 * S, stop, error);
    ASSERT_TRUE(cache) << error;
  }
  void Added() { EXPECT_GT(cache->Counters().appended, 0u); }
  std::atomic<bool> stop{false};
  std::string error;
  std::optional<EventCache> cache;
};

TEST_F(EventCacheTest, CopiesAcrossSegmentsAndFileBoundaries) {
  const auto a = Pattern(S - 13, 3);
  const auto b = Pattern(113, 77);
  cache->BeginFile("first", 0);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  std::vector<std::uint8_t> expected = a;
  expected.insert(expected.end(), b.begin(), b.end());
  std::vector<std::uint8_t> out(expected.size() + 32, 0xcc);
  EXPECT_EQ(Copied(cache->Read("first", 0, out)), expected.size());
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out.begin()));
  EXPECT_EQ(out.back(), 0xcc);
  cache->MarkWritten("first", expected.size());
  cache->EndFile();
  cache->BeginFile("second", 0);
  const auto c = Pattern(97, 101);
  ASSERT_EQ(cache->Append(c), AppendOutcome::Appended);
  std::vector<std::uint8_t> boundary(200, 0);
  EXPECT_EQ(Copied(cache->Read("first", S, boundary)), 100u);
  EXPECT_TRUE(
      std::equal(expected.begin() + S, expected.end(), boundary.begin()));
  EXPECT_EQ(Copied(cache->Read("second", 0, boundary)), c.size());
  EXPECT_TRUE(std::equal(c.begin(), c.end(), boundary.begin()));
  EXPECT_EQ(FirstCached(cache->Read("first", S - 1, boundary)), S);
  Added();
}

TEST_F(EventCacheTest, ResumedBaseControlsSlotIndexAndMissBoundary) {
  const std::uint64_t base = S + 37;
  cache->BeginFile("resumed", base);
  const auto a = Pattern(S, 5);
  const auto b = Pattern(200, 7);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  std::vector<std::uint8_t> out(70);
  EXPECT_EQ(FirstCached(cache->Read("resumed", base - 1, out)), base);
  EXPECT_EQ(Copied(cache->Read("resumed", base + S - 20, out)), out.size());
  EXPECT_TRUE(std::equal(a.end() - 20, a.end(), out.begin()));
  EXPECT_TRUE(std::equal(b.begin(), b.begin() + 50, out.begin() + 20));
  EXPECT_EQ(Copied(cache->Read("resumed", base + S + b.size(), out)), 0u);
  Added();
}

TEST_F(EventCacheTest, MissIsStrictlyBelowObservedBoundary) {
  const auto bytes = Pattern(S, 9);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  std::array<std::uint8_t, 1> out{};
  EXPECT_EQ(FirstCached(cache->Read("data", 0, out)), S);
  EXPECT_EQ(FirstCached(cache->Read("data", S - 1, out)), S);
  EXPECT_EQ(Copied(cache->Read("data", S, out)), 1u);
  EXPECT_EQ(out[0], bytes[0]);
  EXPECT_EQ(Copied(cache->Read("data", 3 * S, out)), 0u);
  EXPECT_EQ(Copied(cache->Read("data", 3 * S + 1, out)), 0u);
  EXPECT_EQ(FirstCached(cache->Read("data", S - 1, {})), S);
  Added();
}

TEST_F(EventCacheTest, FifoIgnoresReadActivityAndPreservesUnwrittenBytes) {
  const auto a = Pattern(S, 1);
  const auto b = Pattern(S, 2);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  cache->MarkWritten("data", S - 1);
  const auto before = cache->Counters();
  EXPECT_EQ(cache->Append(a), AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
  std::array<std::uint8_t, 1> out{};
  EXPECT_EQ(Copied(cache->Read("data", 0, out)), 1u);
  cache->MarkWritten("data", S);
  EXPECT_EQ(cache->Append(a), AppendOutcome::Appended);
  EXPECT_EQ(FirstCached(cache->Read("data", 0, out)), S);
  EXPECT_EQ(Copied(cache->Read("data", S, out)), 1u);
  EXPECT_EQ(out[0], b[0]);
  EXPECT_EQ(cache->Append(b), AppendOutcome::NoSpace);
  Added();
}

TEST_F(EventCacheTest, FullSpanAdmissionFailureLeavesTailAndCountersUnchanged) {
  const auto a = Pattern(S, 1);
  const auto b = Pattern(S - 10, 2);
  const auto crossing = Pattern(20, 3);
  cache->BeginFile("data", 0);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  std::array<std::uint8_t, 40> beforeBytes{};
  const auto end = 2 * S - 10;
  EXPECT_EQ(Copied(cache->Read("data", end - 10, beforeBytes)), 10u);
  const auto before = cache->Counters();
  EXPECT_EQ(cache->Append(crossing), AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
  std::array<std::uint8_t, 40> afterBytes{};
  EXPECT_EQ(Copied(cache->Read("data", end - 10, afterBytes)), 10u);
  EXPECT_EQ(afterBytes, beforeBytes);
  EXPECT_EQ(Copied(cache->Read("data", end, afterBytes)), 0u);
  cache->MarkWritten("data", S);
  ASSERT_EQ(cache->Append(crossing), AppendOutcome::Appended);
  std::vector<std::uint8_t> actual(crossing.size());
  EXPECT_EQ(Copied(cache->Read("data", end, actual)), actual.size());
  EXPECT_EQ(actual, crossing);
  Added();
}

TEST_F(EventCacheTest, CrossingAppendUsesOneFreeSlot) {
  cache->BeginFile("data", 0);
  const auto a = Pattern(S - 10, 4);
  const auto b = Pattern(20, 8);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  std::vector<std::uint8_t> out(30);
  EXPECT_EQ(Copied(cache->Read("data", S - 20, out)), out.size());
  EXPECT_TRUE(std::equal(a.end() - 10, a.end(), out.begin()));
  EXPECT_TRUE(std::equal(b.begin(), b.end(), out.begin() + 10));
  EXPECT_EQ(cache->Counters().occupied, 2u);
  Added();
}

TEST_F(EventCacheTest, StoppedAndOversizedAppendsDoNotChangeState) {
  cache->BeginFile("data", 0);
  const auto bytes = Pattern(S - 1, 8);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  const auto before = cache->Counters();
  EXPECT_EQ(cache->Append(Pattern(S + 1, 9)), AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
  stop.store(true);
  EXPECT_EQ(cache->Append(bytes), AppendOutcome::Stopped);
  EXPECT_EQ(cache->Append({}), AppendOutcome::Stopped);
  EXPECT_EQ(cache->Counters(), before);
  stop.store(false);
  EXPECT_EQ(cache->Append({}), AppendOutcome::Appended);
  EXPECT_EQ(cache->Counters(), before);
  Added();
}

TEST_F(EventCacheTest, RetiredPinnedPrefixIsNotReusedUntilLastReaderFinishes) {
  std::binary_semaphore entered(0), release(0);
  std::atomic<bool> block{true};
  CacheHooks hooks;
  hooks.beforeCopy = [&] {
    if (block.exchange(false)) {
      entered.release();
      release.acquire();
    }
  };
  cache = EventCache::Reserve(2 * S, stop, error, std::move(hooks));
  ASSERT_TRUE(cache) << error;
  cache->BeginFile("data", 0);
  const auto a = Pattern(S, 11), b = Pattern(S, 22), c = Pattern(S, 33);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S);
  std::vector<std::uint8_t> oldCopy(S);
  auto reader = std::async(std::launch::async,
                           [&] { return cache->Read("data", 0, oldCopy); });
  entered.acquire();
  EXPECT_EQ(cache->Counters().pinned, 1u);
  EXPECT_EQ(cache->Append(c), AppendOutcome::Appended);
  std::array<std::uint8_t, 1> out{};
  EXPECT_NO_THROW(EXPECT_EQ(FirstCached(cache->Read("data", 0, out)), 2 * S));
  EXPECT_NO_THROW(EXPECT_EQ(FirstCached(cache->Read("data", S, out)), 2 * S));
  EXPECT_EQ(cache->Counters().occupied, 2u);
  const auto before = cache->Counters();
  EXPECT_EQ(cache->Append(b), AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
  release.release();
  EXPECT_EQ(Copied(reader.get()), S);
  EXPECT_EQ(oldCopy, a);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(cache->Counters().pinned, 0u);
  EXPECT_EQ(cache->Append(b), AppendOutcome::Appended);
  std::vector<std::uint8_t> finalCopy(S);
  EXPECT_EQ(Copied(cache->Read("data", 3 * S, finalCopy)), S);
  EXPECT_EQ(finalCopy, b);
  Added();
}

TEST_F(EventCacheTest, RetiredSlotWaitsForEveryReader) {
  std::counting_semaphore<2> entered(0), release(0);
  CacheHooks hooks;
  hooks.beforeCopy = [&] {
    entered.release();
    release.acquire();
  };
  cache = EventCache::Reserve(2 * S, stop, error, std::move(hooks));
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  const auto a = Pattern(S, 14), b = Pattern(S, 28);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(b), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S);
  std::array<std::uint8_t, 13> first{}, second{};
  std::atomic<unsigned> finished{0};
  auto run = [&](std::span<std::uint8_t> out) {
    const auto result = cache->Read("data", 0, out);
    finished.fetch_add(1);
    finished.notify_one();
    return result;
  };
  auto readerOne = std::async(std::launch::async, [&] { return run(first); });
  auto readerTwo = std::async(std::launch::async, [&] { return run(second); });
  entered.acquire();
  entered.acquire();
  EXPECT_EQ(cache->Counters().pinned, 1u);
  EXPECT_EQ(cache->Append(b), AppendOutcome::Appended);
  release.release();
  finished.wait(0);
  EXPECT_EQ(cache->Counters().pinned, 1u);
  EXPECT_EQ(cache->Counters().occupied, 2u);
  EXPECT_EQ(cache->Append(b), AppendOutcome::NoSpace);
  release.release();
  EXPECT_EQ(Copied(readerOne.get()), first.size());
  EXPECT_EQ(Copied(readerTwo.get()), second.size());
  EXPECT_TRUE(std::equal(first.begin(), first.end(), a.begin()));
  EXPECT_EQ(first, second);
  EXPECT_EQ(cache->Counters().pinned, 0u);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  EXPECT_EQ(cache->Append(a), AppendOutcome::Appended);
  Added();
}

TEST_F(EventCacheTest, ConcurrentAppendDoesNotExtendAnAlreadyPinnedRead) {
  std::binary_semaphore entered(0), release(0);
  std::atomic<bool> block{true};
  CacheHooks hooks;
  hooks.beforeCopy = [&] {
    if (block.exchange(false)) {
      entered.release();
      release.acquire();
    }
  };
  cache = EventCache::Reserve(2 * S, stop, error, std::move(hooks));
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  const auto a = Pattern(100, 19), b = Pattern(100, 38);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  std::vector<std::uint8_t> out(200, 0xcc);
  auto reader = std::async(std::launch::async,
                           [&] { return cache->Read("data", 0, out); });
  entered.acquire();
  EXPECT_EQ(cache->Append(b), AppendOutcome::Appended);
  release.release();
  EXPECT_EQ(Copied(reader.get()), a.size());
  EXPECT_TRUE(std::equal(a.begin(), a.end(), out.begin()));
  EXPECT_TRUE(std::all_of(out.begin() + 100, out.end(),
                          [](auto byte) { return byte == 0xcc; }));
  EXPECT_EQ(Copied(cache->Read("data", 100, out)), b.size());
  EXPECT_TRUE(std::equal(b.begin(), b.end(), out.begin()));
  Added();
}

TEST_F(EventCacheTest, AllPinnedSlotsBlockAdmissionWithoutChangingState) {
  std::binary_semaphore entered(0), release(0);
  CacheHooks hooks;
  hooks.beforeCopy = [&] {
    entered.release();
    release.acquire();
  };
  cache = EventCache::Reserve(2 * S, stop, error, std::move(hooks));
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  const auto a = Pattern(S, 7);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(a), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S);
  std::vector<std::uint8_t> copy(2 * S);
  auto reader = std::async(std::launch::async,
                           [&] { return cache->Read("data", 0, copy); });
  entered.acquire();
  const auto before = cache->Counters();
  EXPECT_EQ(before.pinned, 2u);
  EXPECT_EQ(cache->Append(a), AppendOutcome::NoSpace);
  EXPECT_EQ(cache->Counters(), before);
  release.release();
  EXPECT_EQ(Copied(reader.get()), 2 * S);
  EXPECT_TRUE(std::equal(a.begin(), a.end(), copy.begin()));
  EXPECT_TRUE(std::equal(a.begin(), a.end(), copy.begin() + S));
  EXPECT_EQ(cache->Append(a), AppendOutcome::Appended);
  Added();
}

TEST_F(EventCacheTest, MissBoundaryComesFromTheSameLockAcquisition) {
  constexpr std::size_t iterations = 1000;
  std::binary_semaphore observed(0), evicted(0);
  CacheHooks hooks;
  hooks.afterMiss = [&] {
    observed.release();
    evicted.acquire();
  };
  cache = EventCache::Reserve(2 * S, stop, error, std::move(hooks));
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  const std::vector<std::uint8_t> bytes(S, 0x5a);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  cache->MarkWritten("data", 2 * S);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  cache->MarkWritten("data", 3 * S);
  std::size_t mismatches = 0;
  std::thread reader([&] {
    std::array<std::uint8_t, 1> out{};
    for (std::size_t i = 0; i < iterations; ++i) {
      const auto result = cache->Read("data", 0, out);
      const auto *miss = std::get_if<CacheReadResult::NotCached>(&result.value);
      if (!miss || miss->firstCached != (i + 1) * S) ++mismatches;
    }
  });
  for (std::size_t i = 0; i < iterations; ++i) {
    observed.acquire();
    EXPECT_EQ(cache->Append(bytes), AppendOutcome::Appended);
    cache->MarkWritten("data", (i + 4) * S);
    evicted.release();
  }
  reader.join();
  std::cout << "snapshot mismatches=" << mismatches << "/" << iterations
            << '\n';
  EXPECT_EQ(mismatches, 0u);
  Added();
}

TEST_F(EventCacheTest, ReservationFailureReportsErrorAndRoundedSize) {
  std::size_t requested = 0;
  CacheHooks hooks;
  hooks.reserve = [&](std::size_t size) -> void * {
    requested = size;
    errno = ENOMEM;
    return MAP_FAILED;
  };
  auto failed =
      EventCache::Reserve(2 * S + S / 2, stop, error, std::move(hooks));
  EXPECT_FALSE(failed);
  EXPECT_EQ(requested, 2 * S);
  EXPECT_NE(error.find("cache reservation failed"), std::string::npos);
  EXPECT_FALSE(error.empty());
}

TEST_F(EventCacheTest, TooSmallReservationIsRejectedBeforeMapping) {
  bool called = false;
  CacheHooks hooks;
  hooks.reserve = [&](std::size_t) -> void * {
    called = true;
    return MAP_FAILED;
  };
  EXPECT_FALSE(EventCache::Reserve(S, stop, error, std::move(hooks)));
  EXPECT_FALSE(called);
  EXPECT_FALSE(error.empty());
}

TEST_F(EventCacheTest,
       InvalidWrittenPositionsCannotMakeUnwrittenBytesEvictable) {
  cache->BeginFile("data", 31);
  const auto bytes = Pattern(19, 8);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  EXPECT_THROW(cache->MarkWritten("data", 51), std::invalid_argument);
  EXPECT_THROW(cache->MarkWritten("data", 30), std::invalid_argument);
  EXPECT_THROW(cache->MarkWritten("unknown", 0), std::invalid_argument);
  cache->MarkWritten("data", 50);
  EXPECT_THROW(cache->MarkWritten("data", 49), std::invalid_argument);
  Added();
}

TEST_F(EventCacheTest,
       UnknownFileLeavesOutputUntouchedAndRemovedFileReturnsDiskSentinel) {
  std::array<std::uint8_t, 4> out{11, 22, 33, 44};
  const auto sentinel = out;
  const auto before = cache->Counters();
  EXPECT_EQ(FirstCached(cache->Read("unknown", 73, out)), NOT_CACHED_ANYWHERE);
  EXPECT_EQ(out, sentinel);
  EXPECT_EQ(cache->Counters(), before);
  cache->BeginFile("unknown", 73);
  ASSERT_EQ(cache->Append(sentinel), AppendOutcome::Appended);
  out.fill(0);
  EXPECT_EQ(Copied(cache->Read("unknown", 73, out)), sentinel.size());
  EXPECT_EQ(out, sentinel);
  cache->MarkWritten("unknown", 77);
  cache->EndFile();
  cache->BeginFile("old", 37);
  ASSERT_EQ(cache->Append(sentinel), AppendOutcome::Appended);
  cache->MarkWritten("old", 41);
  cache->EndFile();
  cache->BeginFile("current", 0);
  const auto bytes = Pattern(S, 53);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  EXPECT_EQ(FirstCached(cache->Read("old", 37, out)), NOT_CACHED_ANYWHERE);
  EXPECT_EQ(out, sentinel);
  Added();
}

TEST_F(EventCacheTest, EmptyFilesUnknownNamesAndEndOfFileAreDistinct) {
  std::array<std::uint8_t, 1> out{};
  EXPECT_EQ(FirstCached(cache->Read("unknown", 0, out)), NOT_CACHED_ANYWHERE);
  cache->BeginFile("empty", 13);
  EXPECT_EQ(Copied(cache->Read("empty", 13, out)), 0u);
  EXPECT_EQ(FirstCached(cache->Read("empty", 12, out)), 13u);
  cache->EndFile();
  EXPECT_THROW(cache->Append(out), std::logic_error);
  EXPECT_EQ(FirstCached(cache->Read("empty", 13, out)), NOT_CACHED_ANYWHERE);
}

TEST_F(EventCacheTest, MoveTransfersReservationAndRestartBeginsEmpty) {
  cache->BeginFile("data", 37);
  const auto bytes = Pattern(57, 4);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  Added();
  EventCache moved(std::move(*cache));
  std::vector<std::uint8_t> out(bytes.size());
  EXPECT_EQ(Copied(moved.Read("data", 37, out)), bytes.size());
  EXPECT_EQ(out, bytes);
  cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  EXPECT_EQ(cache->Counters().appended, 0u);
  cache->BeginFile("data", 94);
  EXPECT_EQ(FirstCached(cache->Read("data", 37, out)), 94u);
}

TEST_F(EventCacheTest, CopyHookExceptionReleasesPins) {
  CacheHooks hooks;
  hooks.beforeCopy = [] { throw std::runtime_error("injected copy failure"); };
  cache = EventCache::Reserve(2 * S, stop, error, std::move(hooks));
  ASSERT_TRUE(cache);
  cache->BeginFile("data", 0);
  const std::array<std::uint8_t, 1> bytes{42};
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  std::array<std::uint8_t, 1> out{};
  EXPECT_THROW(cache->Read("data", 0, out), std::runtime_error);
  EXPECT_EQ(cache->Counters().pinned, 0u);
  Added();
}

TEST_F(EventCacheTest, DeferredInterfacesLeaveStateAndOutputUnchanged) {
  cache->BeginFile("data", 0);
  const std::array<std::uint8_t, 1> bytes{42};
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  cache->MarkWritten("data", 1);
  const auto before = cache->Counters();
  WriteRange range{"sentinel", 17, bytes};
  EXPECT_FALSE(cache->NextUnwritten("data", range));
  EXPECT_EQ(range.file, "sentinel");
  EXPECT_EQ(range.offset, 17u);
  EXPECT_EQ(range.bytes.data(), bytes.data());
  cache->EvictExpired(std::chrono::steady_clock::now(),
                      std::chrono::seconds(0));
  EXPECT_EQ(cache->Counters(), before);
  Added();
}

TEST(EventCacheCapacityTest,
     ReservationBoundsTwoSixtyFourAndTwoThousandFortyEightSlots) {
  std::atomic<bool> stop{false};
  for (const std::size_t count : {2u, 64u, 2048u}) {
    SCOPED_TRACE(count);
    std::string error;
    auto cache = EventCache::Reserve(count * S + S - 1, stop, error);
    ASSERT_TRUE(cache) << error;
    EXPECT_EQ(cache->Counters().capacity, count);
    const std::array<std::uint8_t, 1> bytes{83};
    for (std::size_t i = 0; i < count; ++i) {
      const auto name = std::to_string(i);
      cache->BeginFile(name, 0);
      ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
      cache->EndFile();
      EXPECT_LE(cache->Counters().occupied, count);
    }
    cache->BeginFile("extra", 0);
    const auto full = cache->Counters();
    EXPECT_EQ(full.occupied, count);
    EXPECT_EQ(cache->Append(bytes), AppendOutcome::NoSpace);
    EXPECT_EQ(cache->Counters(), full);
    for (std::size_t i = 0; i < count; ++i) {
      std::array<std::uint8_t, 1> out{};
      EXPECT_EQ(Copied(cache->Read(std::to_string(i), 0, out)), 1u);
      EXPECT_EQ(out, bytes);
    }
    cache->MarkWritten("0", 1);
    EXPECT_EQ(cache->Append(bytes), AppendOutcome::Appended);
    EXPECT_EQ(cache->Counters().occupied, count);
    EXPECT_EQ(FirstCached(cache->Read("0", 0, {})), NOT_CACHED_ANYWHERE);
    EXPECT_GT(cache->Counters().appended, 0u);
  }
}

}  // namespace
}  // namespace binlog_streamer
