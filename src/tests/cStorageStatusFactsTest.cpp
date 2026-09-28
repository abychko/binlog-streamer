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

#include "cStorageStatusFacts.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <vector>
#include "cache/hCacheDefaults.hpp"

namespace binlog_streamer {
namespace {

TEST(StorageStatusFactsTest, ReportsTheDiskLimitAndNoMemoryWithoutACache) {
  const StorageCatalog catalog;
  const PublishedPositionTracker published;
  const StorageStatusFacts facts(catalog, published, nullptr, 4000);
  EXPECT_EQ(facts.MaxBytes(), 4000u);
  const MemoryStatus memory = facts.Memory();
  EXPECT_EQ(memory.maxBytes, 0u);
  EXPECT_EQ(memory.bytes, 0u);
  EXPECT_EQ(memory.files, 0u);
}

TEST(StorageStatusFactsTest, MemoryIsTheCacheInWholeSegments) {
  const StorageCatalog catalog;
  const PublishedPositionTracker published;
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(4 * CACHE_SEGMENT_SIZE, stop, error);
  ASSERT_TRUE(cache.has_value()) << error;
  const StorageStatusFacts facts(catalog, published, &*cache, 0);

  MemoryStatus memory = facts.Memory();
  EXPECT_EQ(memory.maxBytes, 4 * CACHE_SEGMENT_SIZE);
  EXPECT_EQ(memory.bytes, 0u);
  EXPECT_EQ(memory.files, 0u);

  cache->BeginFile("binlog.000001", 4);
  const std::vector<std::uint8_t> one(1, 0x5a);
  ASSERT_EQ(cache->Append(one), AppendOutcome::Appended);
  memory = facts.Memory();
  EXPECT_EQ(memory.bytes, CACHE_SEGMENT_SIZE);
  EXPECT_EQ(memory.files, 1u);
}

}  // namespace
}  // namespace binlog_streamer
