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
#include <cstdint>
#include <future>
#include <semaphore>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace binlog_streamer {
namespace {
constexpr auto S = CACHE_SEGMENT_SIZE;

TEST(EventCacheConcurrencyTest,
     AdmissionDoesNotPublishBytesBeforeCopyCompletes) {
  std::atomic<bool> stop{false}, block{false};
  std::binary_semaphore entered(0), release(0);
  CacheHooks hooks;
  hooks.beforeAppendCopy = [&] {
    if (block) {
      entered.release();
      release.acquire();
    }
  };
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error, hooks);
  ASSERT_TRUE(cache) << error;
  cache->BeginFile("data", 37);
  const std::vector<std::uint8_t> first(S - 17, 0x32), next(71, 0xa7);
  ASSERT_EQ(cache->Append(first), AppendOutcome::Appended);
  block = true;
  auto producer =
      std::async(std::launch::async, [&] { return cache->Append(next); });
  entered.acquire();
  std::array<std::uint8_t, 100> out;
  out.fill(0xcc);
  const auto result = cache->Read("data", 37 + first.size(), out);
  const auto *copied = std::get_if<CacheReadResult::Copied>(&result.value);
  EXPECT_NE(copied, nullptr);
  if (copied) {
    EXPECT_EQ(copied->n, 0u);
  }
  EXPECT_TRUE(
      std::all_of(out.begin(), out.end(), [](auto b) { return b == 0xcc; }));
  EXPECT_EQ(cache->Counters().appended, first.size());
  release.release();
  EXPECT_EQ(producer.get(), AppendOutcome::Appended);
  EXPECT_EQ(std::get<CacheReadResult::Copied>(
                cache->Read("data", 37 + first.size(), out).value)
                .n,
            next.size());
  EXPECT_TRUE(std::equal(next.begin(), next.end(), out.begin()));
}

TEST(EventCacheConcurrencyTest, ConcurrentTailReadsMatchEveryPublishedByte) {
  std::atomic<bool> stop{false}, done{false};
  std::string error;
  auto cache = EventCache::Reserve(4 * S, stop, error);
  ASSERT_TRUE(cache) << error;
  cache->BeginFile("data", 37);
  constexpr std::size_t chunk = 65521, rounds = 128;
  std::vector<std::uint8_t> expected(chunk * rounds);
  for (std::size_t i = 0; i < expected.size(); ++i)
    expected[i] = static_cast<std::uint8_t>(i * 37 + i / 251);
  std::atomic<std::size_t> mismatches{0}, copiedBytes{0};
  std::thread reader([&] {
    std::vector<std::uint8_t> out(chunk + 113);
    std::uint64_t offset = 37;
    while (!done.load() || offset < expected.size() + 37) {
      const auto result = cache->Read("data", offset, out);
      if (const auto *copy =
              std::get_if<CacheReadResult::Copied>(&result.value)) {
        if (!std::equal(out.begin(), out.begin() + copy->n,
                        expected.begin() + offset - 37))
          ++mismatches;
        offset += copy->n;
        copiedBytes += copy->n;
        if (!copy->n) std::this_thread::yield();
      } else
        offset = std::get<CacheReadResult::NotCached>(result.value).firstCached;
    }
  });
  for (std::size_t i = 0; i < rounds; ++i) {
    while (cache->Append(std::span(expected).subspan(i * chunk, chunk)) ==
           AppendOutcome::NoSpace)
      std::this_thread::yield();
    cache->MarkWritten("data", 37 + (i + 1) * chunk);
  }
  done = true;
  reader.join();
  EXPECT_EQ(mismatches, 0u);
  EXPECT_GT(copiedBytes, 0u);
}

TEST(EventCacheConcurrencyTest, NonzeroBaseSurvivesReusedSlotsAtTheSeam) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  const std::uint64_t base = S + 37;
  cache->BeginFile("data", base);
  const std::vector<std::uint8_t> first(S, 0x21), second(S, 0x43),
      third(S, 0x65);
  ASSERT_EQ(cache->Append(first), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(second), AppendOutcome::Appended);
  cache->MarkWritten("data", base + 2 * S);
  ASSERT_EQ(cache->Append(third), AppendOutcome::Appended);
  std::array<std::uint8_t, 50> out{};
  EXPECT_EQ(std::get<CacheReadResult::Copied>(
                cache->Read("data", base + 2 * S - 20, out).value)
                .n,
            out.size());
  EXPECT_TRUE(std::all_of(out.begin(), out.begin() + 20,
                          [](auto byte) { return byte == 0x43; }));
  EXPECT_TRUE(std::all_of(out.begin() + 20, out.end(),
                          [](auto byte) { return byte == 0x65; }));
}

TEST(EventCacheConcurrencyTest, RetiredPinsDoNotKeepClosedFileMetadataAlive) {
  std::atomic<bool> stop{false}, block{true};
  std::binary_semaphore entered(0), release(0);
  CacheHooks hooks;
  hooks.beforeCopy = [&] {
    if (block.exchange(false)) {
      entered.release();
      release.acquire();
    }
  };
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error, hooks);
  ASSERT_TRUE(cache);
  const std::vector<std::uint8_t> first(S, 0x21), second(S, 0x43),
      next(S, 0x65);
  cache->BeginFile("old", 37);
  ASSERT_EQ(cache->Append(first), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(second), AppendOutcome::Appended);
  cache->MarkWritten("old", 37 + 2 * S);
  cache->EndFile();
  std::vector<std::uint8_t> copy(S);
  auto reader = std::async(std::launch::async,
                           [&] { return cache->Read("old", 37, copy); });
  entered.acquire();
  cache->BeginFile("new", 19);
  EXPECT_EQ(cache->Append(next), AppendOutcome::Appended);
  EXPECT_EQ(cache->Counters().files, 1u);
  EXPECT_EQ(cache->Counters().occupied, 2u);
  EXPECT_EQ(
      std::get<CacheReadResult::NotCached>(cache->Read("old", 37, {}).value)
          .firstCached,
      NOT_CACHED_ANYWHERE);
  EXPECT_EQ(cache->Append(next), AppendOutcome::NoSpace);
  release.release();
  EXPECT_EQ(std::get<CacheReadResult::Copied>(reader.get().value).n, S);
  EXPECT_EQ(copy, first);
  EXPECT_EQ(cache->Counters().occupied, 1u);
  ASSERT_EQ(cache->Append(next), AppendOutcome::Appended);
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(cache->Read("new", 19 + S, copy).value)
          .n,
      S);
  EXPECT_EQ(copy, next);
}

TEST(EventCacheConcurrencyTest,
     RemovingClosedHeadsPreservesLiveFileIdsAndBytes) {
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache);
  std::array<std::uint8_t, 1> out{};
  for (unsigned i = 0; i < 8; ++i) {
    const auto name = std::to_string(i);
    const std::vector<std::uint8_t> bytes(S, static_cast<std::uint8_t>(i + 1));
    cache->BeginFile(name, 37);
    ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
    cache->MarkWritten(name, 37 + S);
    cache->EndFile();
    EXPECT_EQ(cache->Counters().files, std::min(i + 1, 2u));
    EXPECT_EQ(
        std::get<CacheReadResult::Copied>(cache->Read(name, 37, out).value).n,
        1u);
    EXPECT_EQ(out[0], i + 1);
    if (i >= 2) {
      const auto miss = cache->Read(std::to_string(i - 2), 37, out);
      EXPECT_EQ(std::get<CacheReadResult::NotCached>(miss.value).firstCached,
                NOT_CACHED_ANYWHERE);
      EXPECT_EQ(std::get<CacheReadResult::Copied>(
                    cache->Read(std::to_string(i - 1), 37, out).value)
                    .n,
                1u);
      EXPECT_EQ(out[0], i);
    }
  }
}
}  // namespace
}  // namespace binlog_streamer
