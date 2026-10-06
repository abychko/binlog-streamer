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

#include "cTempDirectoryFixture.hpp"
#include "cache/hCacheDefaults.hpp"
#include "storage/cBinlogStorage.hpp"
#include "storage/cStorageReader.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace binlog_streamer {
namespace {
constexpr auto S = CACHE_SEGMENT_SIZE;
class StorageReaderCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache = EventCache::Reserve(4 * S, stop, error);
    ASSERT_TRUE(cache) << error;
  }
  void Add(std::uint64_t size, bool onDisk = true,
           std::uint64_t headerLength = 0) {
    StoredFileRecord record;
    record.name = "data";
    record.size = size;
    record.onDisk = onDisk;
    record.headerLength = headerLength;
    catalog.Add(record);
  }
  void Write(std::span<const std::uint8_t> bytes) {
    std::ofstream out(fixture.Path("data"), std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  test::TempDirectoryFixture fixture;
  std::atomic<bool> stop{false};
  std::string error;
  std::optional<EventCache> cache;
  StorageCatalog catalog;
  PublishedPositionTracker published;
};

TEST(StorageReaderCacheOwnershipTest,
     StorageOwnsOptionalReservationAndClassifiesFailure) {
  std::atomic<bool> stop{false};
  BinlogStorage storage;
  StorageOpenFailure failure = StorageOpenFailure::AccessProblem;
  std::string error;
  EXPECT_EQ(storage.Cache(), nullptr);
  EXPECT_FALSE(
      storage.ReserveCache(S, std::chrono::seconds(60), stop, failure, error));
  EXPECT_EQ(failure, StorageOpenFailure::StorageProblem);
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(storage.Cache(), nullptr);
  ASSERT_TRUE(storage.ReserveCache(2 * S, std::chrono::seconds(60), stop,
                                   failure, error))
      << error;
  ASSERT_NE(storage.Cache(), nullptr);
  EXPECT_EQ(storage.Cache()->Counters().capacity, 2u);
}

TEST_F(StorageReaderCacheTest,
       ClosedFileUsesCatalogBoundaryEvenWhenDiskIsLonger) {
  const std::array<std::uint8_t, 10> bytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  Write(bytes);
  Add(6);
  published.Advance("later", 100);
  StorageReader reader(fixture.Directory(), catalog, published, &*cache);
  auto cursor = reader.Open("data", error);
  ASSERT_TRUE(cursor) << error;
  std::array<std::uint8_t, 20> out;
  out.fill(0xcc);
  EXPECT_EQ(reader.Read(*cursor, 0, out, error), 6u) << error;
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.begin() + 6, out.begin()));
  EXPECT_EQ(out[6], 0xcc);
  EXPECT_EQ(reader.Read(*cursor, 6, out, error), 0u);
  EXPECT_EQ(reader.Counters().readFromDisk, 6u);
}

TEST_F(StorageReaderCacheTest,
       DiskReadStopsExactlyAtNonzeroCacheBaseBeforePhysicalEof) {
  std::vector<std::uint8_t> header(37, 0x31), body(S + 31);
  for (std::size_t i = 0; i < body.size(); ++i)
    body[i] = static_cast<std::uint8_t>(i * 37 + i / 251);
  Write(header);
  Add(37 + body.size(), true, 37);
  cache->BeginFile("data", 37);
  ASSERT_EQ(cache->Append(std::span(body).first(S)), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(std::span(body).subspan(S)), AppendOutcome::Appended);
  published.Advance("data", 37 + body.size());
  StorageReader reader(fixture.Directory(), catalog, published, &*cache);
  auto cursor = reader.Open("data", error);
  ASSERT_TRUE(cursor);
  std::vector<std::uint8_t> out(S + 68, 0xcc);
  EXPECT_FALSE(cursor->LastReadFromDisk());
  ASSERT_EQ(reader.Read(*cursor, 0, out, error), 37u) << error;
  EXPECT_TRUE(cursor->LastReadFromDisk());
  EXPECT_TRUE(std::equal(header.begin(), header.end(), out.begin()));
  EXPECT_EQ(out[37], 0xcc);
  ASSERT_EQ(reader.Read(*cursor, 37, out, error), body.size()) << error;
  EXPECT_FALSE(cursor->LastReadFromDisk());
  EXPECT_TRUE(std::equal(body.begin(), body.end(), out.begin()));
  EXPECT_EQ(reader.Counters().readFromDisk, 37u);
  EXPECT_EQ(reader.Counters().readFromDiskHeader, 37u);
  EXPECT_EQ(
      reader.Counters().readFromDisk - reader.Counters().readFromDiskHeader,
      0u);
  EXPECT_EQ(reader.Counters().readFromCache, body.size());
  EXPECT_EQ(reader.Counters().seamCrossings, 1u);
}

TEST_F(StorageReaderCacheTest,
       ColdReaderStaysOnDiskUntilCatchingTheCachedTail) {
  std::vector<std::uint8_t> bytes(16 * S + 19);
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(i * 13 + i / S);
  Write(bytes);
  Add(16 * S, true, 37);
  cache->BeginFile("data", 0);
  for (std::size_t i = 0; i < 16; ++i) {
    ASSERT_EQ(cache->Append(std::span(bytes).subspan(i * S, S)),
              AppendOutcome::Appended);
    cache->MarkWritten("data", (i + 1) * S);
  }
  StorageReader reader(fixture.Directory(), catalog, published, &*cache);
  auto cursor = reader.Open("data", error);
  ASSERT_TRUE(cursor);
  std::vector<std::uint8_t> out(S);
  for (std::size_t i = 0; i < 12; ++i) {
    ASSERT_EQ(reader.Read(*cursor, i * S, out, error), S) << error;
    EXPECT_TRUE(std::equal(out.begin(), out.end(), bytes.begin() + i * S));
  }
  EXPECT_EQ(reader.Counters().readFromDisk, 12 * S);
  EXPECT_EQ(reader.Counters().readFromDiskHeader, 37u);
  EXPECT_EQ(
      reader.Counters().readFromDisk - reader.Counters().readFromDiskHeader,
      12 * S - 37);
  EXPECT_EQ(reader.Counters().readFromCache, 0u);
  for (std::size_t i = 12; i < 16; ++i) {
    ASSERT_EQ(reader.Read(*cursor, i * S, out, error), S) << error;
    EXPECT_TRUE(std::equal(out.begin(), out.end(), bytes.begin() + i * S));
  }
  EXPECT_EQ(reader.Read(*cursor, 16 * S, out, error), 0u);
  EXPECT_EQ(reader.Counters().seamCrossings, 1u);
  EXPECT_EQ(reader.Counters().readFromCache, 4 * S);
  for (std::size_t i = 16; i < 24; ++i) {
    ASSERT_EQ(cache->Append(std::span(bytes).first(S)),
              AppendOutcome::Appended);
    cache->MarkWritten("data", (i + 1) * S);
    published.Advance("data", (i + 1) * S);
    ASSERT_EQ(reader.Read(*cursor, i * S, out, error), S) << error;
    EXPECT_TRUE(std::equal(out.begin(), out.end(), bytes.begin()));
    EXPECT_EQ(reader.Counters().readFromDisk, 12 * S);
    EXPECT_EQ(reader.Counters().readFromDiskHeader, 37u);
    EXPECT_EQ(
        reader.Counters().readFromDisk - reader.Counters().readFromDiskHeader,
        12 * S - 37);
  }
}

TEST_F(StorageReaderCacheTest,
       DiskReadAcrossHeaderBoundaryCountsOnlyOverlapAfterCursorMoves) {
  const std::vector<std::uint8_t> bytes(80, 0x51);
  Write(bytes);
  Add(bytes.size(), true, 37);
  StorageReader reader(fixture.Directory(), catalog, published);
  auto cursor = reader.Open("data", error);
  ASSERT_TRUE(cursor) << error;
  std::array<std::uint8_t, 50> out{};
  ASSERT_EQ(reader.Read(*cursor, 0, std::span(out).first(20), error), 20u);
  EXPECT_EQ(reader.Counters().readFromDiskHeader, 20u);
  FileCursor moved(std::move(*cursor));
  FileCursor assigned;
  assigned = std::move(moved);
  ASSERT_EQ(reader.Read(assigned, 20, out, error), 50u) << error;
  EXPECT_EQ(reader.Counters().readFromDisk, 70u);
  EXPECT_EQ(reader.Counters().readFromDiskHeader, 37u);
  EXPECT_EQ(
      reader.Counters().readFromDisk - reader.Counters().readFromDiskHeader,
      33u);
}

TEST_F(StorageReaderCacheTest,
       MemoryOnlyFileOpensWithoutDescriptorAndOpensDiskOnlyOnMiss) {
  Add(40, false);
  cache->BeginFile("data", 37);
  const std::array<std::uint8_t, 3> bytes{17, 29, 41};
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  published.Advance("data", 40);
  StorageReader reader(fixture.Directory(), catalog, published, &*cache);
  auto cursor = reader.Open("data", error);
  ASSERT_TRUE(cursor) << error;
  std::array<std::uint8_t, 3> out{};
  EXPECT_EQ(reader.Read(*cursor, 37, out, error), 3u);
  EXPECT_EQ(out, bytes);
  EXPECT_EQ(reader.Counters().readFromDisk, 0u);
  EXPECT_EQ(reader.Read(*cursor, 0, out, error), 0u);
  EXPECT_FALSE(error.empty());
  Write(bytes);
  error.clear();
  EXPECT_EQ(reader.Read(*cursor, 0, out, error), 3u) << error;
  EXPECT_EQ(out, bytes);
  EXPECT_EQ(reader.Counters().readFromDisk, 3u);
}
}  // namespace
}  // namespace binlog_streamer
