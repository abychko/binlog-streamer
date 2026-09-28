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
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>
#include "cTempDirectoryFixture.hpp"
#include "cache/cEventCache.hpp"
#include "cache/hCacheDefaults.hpp"
#include "storage/cBinlogFileWriter.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageWriter.hpp"
#include "storage/hStorageDefaults.hpp"
namespace binlog_streamer {
namespace {
using namespace std::chrono_literals;
constexpr auto S = CACHE_SEGMENT_SIZE;
constexpr std::uint64_t HeaderSize = 32;
template <class Predicate>
bool Until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(100us);
  }
  return true;
}
std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}
class StorageWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cache = EventCache::Reserve(2 * S, stop, error);
    ASSERT_TRUE(cache) << error;
  }
  void Add(const std::string &name) {
    StoredFileRecord record;
    record.name = name;
    record.headerLength = HeaderSize;
    record.size = HeaderSize;
    record.onDisk = false;
    record.inUse = true;
    catalog.Add(record);
  }
  void Create(StorageWriter &writer, const std::string &name) {
    Add(name);
    cache->BeginFile(name, HeaderSize);
    writer.PostCreate(name, fde, pge);
  }
  std::vector<std::uint8_t> Header(bool inUse = true) {
    std::vector<std::uint8_t> result(BINLOG_MAGIC.begin(), BINLOG_MAGIC.end());
    result.insert(result.end(), fde.begin(), fde.end());
    result[IN_USE_FLAG_OFFSET] = static_cast<std::uint8_t>(inUse);
    result.insert(result.end(), pge.begin(), pge.end());
    return result;
  }
  void AddWithTime(const std::string &name, std::uint32_t createdAt) {
    StoredFileRecord record;
    record.name = name;
    record.createdAt = createdAt;
    record.headerLength = HeaderSize;
    record.size = HeaderSize;
    record.onDisk = false;
    record.inUse = true;
    catalog.Add(record);
  }
  void CreateCached(StorageWriter &writer, const std::string &name,
                    std::uint32_t createdAt) {
    AddWithTime(name, createdAt);
    cache->BeginFile(name, 0);
    ASSERT_EQ(cache->Append(Header()), AppendOutcome::Appended);
    writer.PostCreate(name, fde, pge);
  }
  using Names = std::vector<std::string>;
  static std::string Name(unsigned n) {
    return "binlog.00000" + std::to_string(n);
  }
  void Seed(unsigned count, bool abandoned = false) {
    Names names;
    for (unsigned n = 1; n <= count; ++n) {
      const auto name = Name(n);
      BinlogFileWriter file;
      ASSERT_TRUE(file.Create(directory.Path(name).string(), fde, pge, error))
          << error;
      if (n != count && !(abandoned && n == 2)) {
        ASSERT_TRUE(file.MarkClosed(error)) << error;
      }
      StoredFileRecord record;
      record.name = name;
      record.createdAt = n * 100;
      record.headerLength = HeaderSize;
      record.size = HeaderSize;
      record.inUse = n == count;
      record.onDisk = true;
      catalog.Add(record);
      names.push_back(name);
    }
    ASSERT_TRUE(BinlogIndexFile::Replace(
        (directory.Directory() / INDEX_FILE_NAME).string(), names, error))
        << error;
  }
  void SeedSized(const std::vector<std::uint64_t> &sizes) {
    Names names;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      const auto name = Name(static_cast<unsigned>(i + 1));
      BinlogFileWriter file;
      ASSERT_TRUE(file.Create(directory.Path(name).string(), fde, pge, error))
          << error;
      if (i + 1 != sizes.size()) {
        ASSERT_TRUE(file.MarkClosed(error)) << error;
      }
      StoredFileRecord record;
      record.name = name;
      record.createdAt = static_cast<std::uint32_t>((i + 1) * 100);
      record.headerLength = HeaderSize;
      record.size = sizes[i];
      record.inUse = i + 1 == sizes.size();
      record.onDisk = true;
      catalog.Add(record);
      names.push_back(name);
    }
    ASSERT_TRUE(BinlogIndexFile::Replace(
        (directory.Directory() / INDEX_FILE_NAME).string(), names, error))
        << error;
  }
  StorageWriterHooks Hooks() {
    StorageWriterHooks hooks;
    hooks.measureAvailable = [this](std::uint64_t &available,
                                    std::string &failure) {
      if (measureFails.load()) {
        failure = "injected";
        return false;
      }
      available = freeBytes.load();
      return true;
    };
    return hooks;
  }
  using Changes = std::vector<std::tuple<bool, std::uint64_t, std::uint64_t>>;
  StorageDiskLimits Disk() {
    return {100, 10, 80, 50, 20, [this](const PurgeResult &result) {
              std::lock_guard lock(purgedMutex);
              changes.emplace_back(result.spaceShort, result.usedBytes,
                                   result.availableBytes);
            }};
  }
  StorageExpiry DiskOnly() {
    StorageExpiry expiry;
    expiry.onPurged = [this](const PurgeResult &result) {
      std::lock_guard lock(purgedMutex);
      batches.push_back(result.purged);
    };
    expiry.disk = Disk();
    return expiry;
  }
  Changes SpaceChanges() {
    std::lock_guard lock(purgedMutex);
    return changes;
  }
  std::atomic<std::uint64_t> freeBytes{1000};
  std::atomic<bool> measureFails{false};
  Changes changes;
  StorageExpiry Expiry() {
    return {[this] { return std::optional<std::uint64_t>(now.load()); }, 100s,
            [this](const PurgeResult &result) {
              std::lock_guard lock(purgedMutex);
              batches.push_back(result.purged);
            }};
  }
  std::vector<Names> Batches() {
    std::lock_guard lock(purgedMutex);
    return batches;
  }
  void CheckRemaining(const Names &expected) {
    Names index;
    ASSERT_TRUE(BinlogIndexFile::Load(
        (directory.Directory() / INDEX_FILE_NAME).string(), index, error));
    EXPECT_EQ(index, expected);
    Names records;
    for (std::size_t i = 0; i < catalog.Size(); ++i) {
      const auto record = catalog.At(i);
      if (record.onDisk) records.push_back(record.name);
    }
    EXPECT_EQ(records, expected);
    for (const auto &name : expected)
      EXPECT_TRUE(std::filesystem::exists(directory.Path(name)));
  }
  std::atomic<std::uint64_t> now{1000};
  std::mutex purgedMutex;
  std::vector<Names> batches;
  test::TempDirectoryFixture directory;
  std::atomic<bool> stop{false};
  std::string error;
  std::optional<EventCache> cache;
  StorageCatalog catalog;
  const std::vector<std::uint8_t> fde = std::vector<std::uint8_t>(24, 0);
  const std::array<std::uint8_t, 4> pge{1, 2, 3, 4};
};
TEST_F(StorageWriterTest, SpacePurgeRunsWithoutTheSourceClock) {
  SeedSized({30, 30, 30, 5});
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       DiskOnly());
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1), Name(2)}}));
  CheckRemaining({Name(3), Name(4)});
  EXPECT_EQ(SpaceChanges(), Changes{});
  writer.Stop();
}
TEST_F(StorageWriterTest, SpacePurgeRunsWhenTheClockReturnsNothing) {
  SeedSized({30, 30, 30, 5});
  auto expiry = DiskOnly();
  expiry.sourceNow = [] { return std::nullopt; };
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       expiry);
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1), Name(2)}}));
  CheckRemaining({Name(3), Name(4)});
  EXPECT_EQ(SpaceChanges(), Changes{});
  writer.Stop();
}
TEST_F(StorageWriterTest, ExpiryStillAppliesWithDiskLimits) {
  SeedSized({10, 10, 10, 10, 10});
  auto expiry = Expiry();
  expiry.disk = Disk();
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       expiry);
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(Batches(),
            (std::vector<Names>{{Name(1), Name(2), Name(3), Name(4)}}));
  CheckRemaining({Name(5)});
  writer.Stop();
}
TEST_F(StorageWriterTest, FreeSpaceIsMeasuredForEachPass) {
  SeedSized({10, 10, 10, 10, 5});
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       DiskOnly());
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(Batches(), (std::vector<Names>{}));
  freeBytes = 5;
  writer.PostPurge();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1), Name(2)}}));
  CheckRemaining({Name(3), Name(4), Name(5)});
  EXPECT_EQ(SpaceChanges(), Changes{});
  writer.Stop();
}
TEST_F(StorageWriterTest, SpaceShortIsReportedOnlyOnChange) {
  SeedSized({10, 10, 10, 10, 5});
  auto pin = catalog.Pin(Name(1), error);
  ASSERT_TRUE(pin) << error;
  freeBytes = 5;
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       DiskOnly());
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  writer.PostPurge();
  ASSERT_TRUE(writer.DrainAndSync());
  freeBytes = 1000;
  writer.PostPurge();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(SpaceChanges(), (Changes{{true, 45, 5}, {false, 45, 1000}}));
  EXPECT_EQ(Batches(), (std::vector<Names>{}));
  CheckRemaining({Name(1), Name(2), Name(3), Name(4), Name(5)});
  writer.Stop();
}
TEST_F(StorageWriterTest, FreeSpaceFailureFailsTheWriter) {
  SeedSized({10, 10, 5});
  measureFails = true;
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       DiskOnly());
  writer.PostPurge();
  writer.Start();
  EXPECT_TRUE(Until([&] { return writer.Failed(); }));
  EXPECT_EQ(writer.LastError(), "measuring free space: injected");
  EXPECT_FALSE(writer.DrainAndSync());
  CheckRemaining({Name(1), Name(2), Name(3)});
  EXPECT_EQ(Batches(), (std::vector<Names>{}));
  writer.Stop();
}
TEST_F(StorageWriterTest, CreateRefusedAtMaxSizeMinusReserve) {
  SeedSized({40, 50});
  ASSERT_TRUE(catalog.Close(50, error));
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       DiskOnly());
  CreateCached(writer, Name(3), 300);
  writer.Start();
  EXPECT_TRUE(Until([&] { return writer.Failed(); }));
  EXPECT_TRUE(writer.LastError().starts_with(
      "storage.disk.max_size reached: 90 byte(s) stored before binlog.000003"));
  EXPECT_FALSE(writer.DrainAndSync());
  EXPECT_FALSE(std::filesystem::exists(directory.Path(Name(3))));
  CheckRemaining({Name(1), Name(2)});
  writer.Stop();
}
TEST_F(StorageWriterTest, CreateAllowedBelowTheReserveIgnoringTheNewFile) {
  SeedSized({40, 49});
  ASSERT_TRUE(catalog.Close(49, error));
  StorageWriter writer(directory.Directory(), *cache, catalog, Hooks(), {},
                       DiskOnly());
  CreateCached(writer, Name(3), 300);
  ASSERT_TRUE(catalog.UpdateSize(500, error));
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_TRUE(std::filesystem::exists(directory.Path(Name(3))));
  CheckRemaining({Name(1), Name(2), Name(3)});
  EXPECT_FALSE(writer.Failed());
  writer.Stop();
}
TEST_F(StorageWriterTest, ClosePurgesTheFileBeforeTheNewestInQueueOrder) {
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  CreateCached(writer, Name(1), 100);
  cache->EndFile();
  ASSERT_TRUE(catalog.Close(HeaderSize, error));
  writer.PostClose(Name(1));
  CreateCached(writer, Name(2), 200);
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1)}}));
  EXPECT_FALSE(std::filesystem::exists(directory.Path(Name(1))));
  CheckRemaining({Name(2)});
  EXPECT_EQ(catalog.Size(), 1u);
  EXPECT_FALSE(writer.Failed());
  writer.Stop();
}
TEST_F(StorageWriterTest, StartupPurgeRunsAsTheFirstTask) {
  Seed(4);
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1), Name(2), Name(3)}}));
  CheckRemaining({Name(4)});
  EXPECT_EQ(catalog.Size(), 1u);
  for (unsigned n = 1; n <= 3; ++n)
    EXPECT_FALSE(std::filesystem::exists(directory.Path(Name(n))));
  writer.Stop();
}
TEST_F(StorageWriterTest, PeriodIsSubtractedFromSourceNow) {
  Seed(3);
  now = 300;
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_TRUE(Batches().empty());
  CheckRemaining({Name(1), Name(2), Name(3)});
  writer.Stop();
}
TEST_F(StorageWriterTest, FileOpenOnDiskIsNotPurged) {
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  CreateCached(writer, Name(1), 100);
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  cache->EndFile();
  ASSERT_TRUE(catalog.Close(HeaderSize, error));
  AddWithTime(Name(2), 200);
  writer.PostPurge();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_TRUE(Batches().empty());
  CheckRemaining({Name(1)});
  writer.PostClose(Name(1));
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1)}}));
  EXPECT_FALSE(std::filesystem::exists(directory.Path(Name(1))));
  CheckRemaining({});
  writer.Stop();
}
TEST_F(StorageWriterTest, AbandonedCloseKeepsFilesInsideThePeriod) {
  Seed(3, true);
  now = 250;
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  writer.PostCloseAbandoned(Name(2));
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_TRUE(Batches().empty());
  CheckRemaining({Name(1), Name(2), Name(3)});
  EXPECT_EQ(ReadFile(directory.Path(Name(2))), Header(false));
  writer.Stop();
}
TEST_F(StorageWriterTest, AbandonedCloseAlsoPurges) {
  Seed(3, true);
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  writer.PostCloseAbandoned(Name(2));
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(Batches(), (std::vector<Names>{{Name(1), Name(2)}}));
  CheckRemaining({Name(3)});
  EXPECT_FALSE(std::filesystem::exists(directory.Path(Name(1))));
  EXPECT_FALSE(std::filesystem::exists(directory.Path(Name(2))));
  writer.Stop();
}
TEST_F(StorageWriterTest, UnknownSourceTimeSkipsThePass) {
  Seed(3);
  auto expiry = Expiry();
  expiry.sourceNow = [] { return std::nullopt; };
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {}, expiry);
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_TRUE(Batches().empty());
  CheckRemaining({Name(1), Name(2), Name(3)});
  EXPECT_FALSE(writer.Failed());
  writer.Stop();
}
TEST_F(StorageWriterTest, ExpiryOffNeverTouchesTheCatalog) {
  Seed(3);
  StorageWriter writer(directory.Directory(), *cache, catalog);
  writer.PostPurge();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  CheckRemaining({Name(1), Name(2), Name(3)});
  EXPECT_FALSE(writer.Failed());
  writer.Stop();
}
TEST_F(StorageWriterTest, PurgeIndexFailureFailsTheWriter) {
  Seed(3);
  ASSERT_TRUE(
      std::filesystem::create_directory(directory.Path("binlog.index.tmp")));
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, {},
                       Expiry());
  writer.PostPurge();
  writer.Start();
  EXPECT_TRUE(Until([&] { return writer.Failed(); }));
  EXPECT_TRUE(writer.LastError().starts_with("purging files: "));
  EXPECT_FALSE(writer.DrainAndSync());
  EXPECT_TRUE(Batches().empty());
  for (unsigned n = 1; n <= 3; ++n)
    EXPECT_TRUE(std::filesystem::exists(directory.Path(Name(n))));
  writer.Stop();
}
TEST_F(StorageWriterTest,
       DrainsExactBytesWithoutCopyingAndMarksCatalogAfterCreate) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  std::vector<std::uint8_t> bytes(S, 0x59);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(std::span(bytes).first(713)),
            AppendOutcome::Appended);
  bool onDisk = true;
  std::uint64_t header = 0;
  ASSERT_TRUE(catalog.ReadInfo("binlog.000001", onDisk, header));
  EXPECT_FALSE(onDisk);
  EXPECT_FALSE(std::filesystem::exists(directory.Path("binlog.000001")));
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  auto expected = Header();
  expected.insert(expected.end(), bytes.begin(), bytes.end());
  expected.insert(expected.end(), bytes.begin(), bytes.begin() + 713);
  EXPECT_EQ(ReadFile(directory.Path("binlog.000001")), expected);
  EXPECT_EQ(writer.BytesWritten(), S + 713);
  EXPECT_EQ(writer.WriteCalls(), 2u);
  EXPECT_EQ(writer.SyncsPerformed(), 1u);
  ASSERT_TRUE(catalog.ReadInfo("binlog.000001", onDisk, header));
  EXPECT_TRUE(onDisk);
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(writer.SyncsPerformed(), 1u);
  writer.Stop();
  writer.Stop();
}
TEST_F(StorageWriterTest, TwoSlotsBackpressureFiveChunksUntilWorkerStarts) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  const std::vector<std::uint8_t> bytes(S, 0x67);
  auto producer = std::async(std::launch::async, [&] {
    for (int i = 0; i < 5; ++i) {
      if (cache->AppendOrWait(bytes) != AppendOutcome::Appended) return false;
    }
    return true;
  });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > 0; }));
  EXPECT_EQ(cache->Counters().appended, 2 * S);
  EXPECT_EQ(producer.wait_for(0ms), std::future_status::timeout);
  writer.Start();
  const auto ready = producer.wait_for(3s);
  EXPECT_EQ(ready, std::future_status::ready);
  if (ready != std::future_status::ready) cache->Abort();
  EXPECT_TRUE(producer.get());
  EXPECT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(writer.BytesWritten(), 5 * S);
  EXPECT_LE(cache->Counters().occupied, 2u);
  EXPECT_LE(cache->Counters().maxUnwritten, 2 * S);
  auto expected = Header();
  for (int i = 0; i < 5; ++i)
    expected.insert(expected.end(), bytes.begin(), bytes.end());
  EXPECT_EQ(ReadFile(directory.Path("binlog.000001")), expected);
}
TEST_F(StorageWriterTest,
       SleepingWriterAcceptsEightSlotGroupWithoutExplicitWake) {
  std::counting_semaphore<100> sleeping{0};
  StorageWriterHooks hooks;
  hooks.beforeSleep = [&] { sleeping.release(); };
  StorageWriter writer(directory.Directory(), *cache, catalog, hooks);
  Create(writer, "binlog.000001");
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  sleeping.acquire();
  const std::vector<std::uint8_t> bytes(S, 0x79);
  auto producer = std::async(std::launch::async, [&] {
    for (int i = 0; i < 8; ++i)
      if (cache->AppendOrWait(bytes) != AppendOutcome::Appended) return false;
    return true;
  });
  const auto ready = producer.wait_for(2s);
  EXPECT_EQ(ready, std::future_status::ready);
  if (ready != std::future_status::ready) cache->Abort();
  EXPECT_TRUE(producer.get());
  EXPECT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(writer.BytesWritten(), 8 * S);
  auto expected = Header();
  for (int i = 0; i < 8; ++i)
    expected.insert(expected.end(), bytes.begin(), bytes.end());
  EXPECT_EQ(ReadFile(directory.Path("binlog.000001")), expected);
}
TEST_F(StorageWriterTest, AnIdleWriterTakesPublishedBytesAtOnce) {
  std::counting_semaphore<100> sleeping{0};
  StorageWriterHooks hooks;
  hooks.beforeSleep = [&] { sleeping.release(); };
  StorageWriter writer(directory.Directory(), *cache, catalog, hooks);
  Create(writer, "binlog.000001");
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  sleeping.acquire();
  const std::array<std::uint8_t, 1> bytes{0x21};
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  writer.WakeForData();
  EXPECT_TRUE(Until([&] { return writer.BytesWritten() == 1; }));
}
// A writer woken for every publication makes one write(2) per group.
TEST_F(StorageWriterTest, ABusyWriterGathersPublicationsIntoFewWrites) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  const auto before = writer.WriteCalls();
  const std::array<std::uint8_t, 1> bytes{0x22};
  constexpr int PUBLICATIONS = 1000;
  const auto startedAt = std::chrono::steady_clock::now();
  for (int i = 0; i < PUBLICATIONS; ++i) {
    ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
    writer.WakeForData();
    std::this_thread::sleep_for(20us);
  }
  const auto elapsed = std::chrono::steady_clock::now() - startedAt;
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(writer.BytesWritten(), static_cast<std::uint64_t>(PUBLICATIONS));
  const auto allowed = static_cast<std::uint64_t>(elapsed / WRITE_COALESCE) + 3;
  EXPECT_LE(writer.WriteCalls() - before, allowed);
}
TEST_F(StorageWriterTest, MeasuresAgeWhenTakingAnUnwrittenRange) {
  CacheHooks hooks;
  hooks.now = [] { return std::chrono::steady_clock::now() - 500ms; };
  cache = EventCache::Reserve(2 * S, stop, error, hooks);
  ASSERT_TRUE(cache);
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  const std::array<std::uint8_t, 1> bytes{42};
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_GE(writer.MaxUnwrittenAgeMilliseconds(), 500u);
}

TEST_F(StorageWriterTest,
       SyncCadenceFollowsCompletedEventsAndFinalPartialBatch) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  writer.Start();
  const std::array<std::uint8_t, 1> bytes{0x31};
  for (std::uint64_t batch = 1; batch <= 25; ++batch) {
    for (int i = 0; i < 1000; ++i) {
      ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
      writer.NoteEventCompleted();
    }
    writer.Wake();
    ASSERT_TRUE(Until([&] { return writer.BytesWritten() == batch * 1000; }));
    if (batch % 10 == 0) {
      ASSERT_TRUE(Until([&] { return writer.SyncsPerformed() >= batch / 10; }));
    }
    EXPECT_EQ(writer.SyncsPerformed(), batch / 10);
  }
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(writer.SyncsPerformed(), 3u);
  EXPECT_EQ(writer.BytesWritten(), 25000u);
}
TEST_F(StorageWriterTest, CoalescesTwentyThousandEventsIntoOnePeriodicSync) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  ASSERT_EQ(cache->Append(std::vector<std::uint8_t>(20000, 0x42)),
            AppendOutcome::Appended);
  for (int i = 0; i < 20000; ++i) writer.NoteEventCompleted();
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(writer.SyncsPerformed(), 1u);
}
TEST_F(StorageWriterTest,
       ContinuousPacketsSyncBeforeDrainWithoutEmptyingTheQueue) {
  std::binary_semaphore initial{0}, requested{0}, supplied{0};
  unsigned snapshots = 0;
  StorageWriterHooks hooks;
  hooks.afterSnapshot = [&] {
    if (snapshots++ < 9) {
      requested.release();
      if (!supplied.try_acquire_for(2s))
        throw std::runtime_error("packet arrival timed out");
    }
  };
  StorageWriter writer(directory.Directory(), *cache, catalog, hooks);
  Create(writer, "binlog.000001");
  auto producer = std::async(std::launch::async, [&] {
    const std::array<std::uint8_t, 1> event{42};
    for (unsigned packet = 0; packet < 10; ++packet) {
      if (packet != 0 && !requested.try_acquire_for(2s)) return false;
      for (std::uint64_t i = 0; i < SYNC_EVENT_PERIOD; ++i) {
        if (cache->AppendOrWait(event) != AppendOutcome::Appended) return false;
        writer.NoteEventCompleted();
      }
      if (packet == 0)
        initial.release();
      else
        supplied.release();
    }
    return true;
  });
  const bool ready = initial.try_acquire_for(2s);
  EXPECT_TRUE(ready);
  if (!ready) {
    cache->Abort();
    EXPECT_FALSE(producer.get());
    return;
  }
  writer.Start();
  const auto done = producer.wait_for(4s);
  EXPECT_EQ(done, std::future_status::ready);
  if (done != std::future_status::ready) cache->Abort();
  EXPECT_TRUE(producer.get());
  EXPECT_TRUE(Until([&] { return writer.SyncsPerformed() >= 9; }));
  EXPECT_GE(writer.SyncsPerformed(), 9u);
  EXPECT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(writer.BytesWritten(), 100000u);
}
TEST_F(StorageWriterTest,
       EventsCompletedAfterSnapshotRemainInNextSyncInterval) {
  std::binary_semaphore captured{0}, resume{0};
  bool first = true;
  StorageWriterHooks hooks;
  hooks.afterSnapshot = [&] {
    if (!first) return;
    first = false;
    captured.release();
    if (!resume.try_acquire_for(2s))
      throw std::runtime_error("snapshot release timed out");
  };
  StorageWriter writer(directory.Directory(), *cache, catalog, hooks);
  Create(writer, "binlog.000001");
  ASSERT_EQ(cache->Append(std::vector<std::uint8_t>(10000, 0x31)),
            AppendOutcome::Appended);
  for (int i = 0; i < 10000; ++i) writer.NoteEventCompleted();
  writer.Start();
  EXPECT_TRUE(captured.try_acquire_for(2s));
  const std::array<std::uint8_t, 1> late{0x32};
  EXPECT_EQ(cache->Append(late), AppendOutcome::Appended);
  writer.NoteEventCompleted();
  resume.release();
  EXPECT_TRUE(Until([&] {
    return writer.SyncsPerformed() == 1 && writer.BytesWritten() == 10001;
  }));
  EXPECT_EQ(cache->Append(std::vector<std::uint8_t>(9999, 0x33)),
            AppendOutcome::Appended);
  for (int i = 0; i < 9999; ++i) writer.NoteEventCompleted();
  writer.Wake();
  EXPECT_TRUE(Until([&] { return writer.SyncsPerformed() == 2; }));
  EXPECT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(writer.BytesWritten(), 20000u);
}

TEST_F(StorageWriterTest, ClosesAbandonedBeforeFailingToCreateSuccessor) {
  {
    BinlogFileWriter old;
    ASSERT_TRUE(
        old.Create(directory.Path("binlog.000001").string(), fde, pge, error));
  }
  StorageWriter writer(directory.Directory(), *cache, catalog);
  writer.PostCloseAbandoned("binlog.000001");
  Create(writer, "binlog.000002");
  ASSERT_FALSE(std::filesystem::exists(directory.Path("binlog.000002")));
  std::filesystem::create_directory(directory.Path("binlog.000002"));
  writer.Start();
  EXPECT_FALSE(writer.DrainAndSync());
  EXPECT_TRUE(writer.Failed());
  const auto old = ReadFile(directory.Path("binlog.000001"));
  ASSERT_GT(old.size(), IN_USE_FLAG_OFFSET);
  EXPECT_EQ(old[IN_USE_FLAG_OFFSET] & 1, 0);
  EXPECT_FALSE(writer.LastError().empty());
}
TEST_F(StorageWriterTest, FileQueueKeepsFutureBytesBehindTheirCreate) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  const std::array<std::uint8_t, 3> first{1, 2, 3}, second{7, 8, 9};
  ASSERT_EQ(cache->Append(first), AppendOutcome::Appended);
  cache->EndFile();
  writer.PostClose("binlog.000001");
  Create(writer, "binlog.000002");
  ASSERT_EQ(cache->Append(second), AppendOutcome::Appended);
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  auto expected = Header(false);
  expected.insert(expected.end(), first.begin(), first.end());
  EXPECT_EQ(ReadFile(directory.Path("binlog.000001")), expected);
  expected = Header();
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(ReadFile(directory.Path("binlog.000002")), expected);
  std::vector<std::string> names;
  ASSERT_TRUE(BinlogIndexFile::Load(directory.Path("binlog.index").string(),
                                    names, error));
  EXPECT_EQ(names,
            (std::vector<std::string>{"binlog.000001", "binlog.000002"}));
}
TEST_F(StorageWriterTest, ClosingOneFileDoesNotConsumeFutureFilesEventCount) {
  StorageWriter writer(directory.Directory(), *cache, catalog);
  Create(writer, "binlog.000001");
  const std::array<std::uint8_t, 1> byte{42};
  ASSERT_EQ(cache->Append(byte), AppendOutcome::Appended);
  writer.NoteEventCompleted();
  cache->EndFile();
  writer.PostClose("binlog.000001");
  Create(writer, "binlog.000002");
  ASSERT_EQ(cache->Append(std::vector<std::uint8_t>(10000, 0x43)),
            AppendOutcome::Appended);
  for (int i = 0; i < 10000; ++i) writer.NoteEventCompleted();
  writer.Start();
  EXPECT_TRUE(Until([&] {
    return writer.BytesWritten() == 10001 && writer.SyncsPerformed() == 2;
  }));
  EXPECT_TRUE(writer.DrainAndSync());
  EXPECT_EQ(writer.SyncsPerformed(), 2u);
}

TEST_F(StorageWriterTest, OpenExistingAppendsAtRecoveredLength) {
  const std::vector<std::uint8_t> original(77, 0x51);
  {
    BinlogFileWriter old;
    ASSERT_TRUE(
        old.Create(directory.Path("binlog.000001").string(), fde, pge, error));
    ASSERT_TRUE(old.WriteDirect(original, error));
  }
  cache->BeginFile("binlog.000001", HeaderSize + original.size());
  StorageWriter writer(directory.Directory(), *cache, catalog);
  writer.PostOpenExisting("binlog.000001");
  const std::array<std::uint8_t, 2> bytes{0x52, 0x53};
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  writer.Start();
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  auto expected = Header();
  expected.insert(expected.end(), original.begin(), original.end());
  expected.insert(expected.end(), bytes.begin(), bytes.end());
  EXPECT_EQ(ReadFile(directory.Path("binlog.000001")), expected);
}
TEST_F(StorageWriterTest, WriteFailureAbortsBlockedProducerAndDrain) {
  std::binary_semaphore writing{0}, release{0};
  int calls = 0;
  StorageWriterHooks hooks;
  hooks.beforeWrite = [&](std::string &message) {
    if (++calls == 1) return true;
    writing.release();
    release.acquire();
    message = "injected write failure";
    return false;
  };
  StorageWriter writer(directory.Directory(), *cache, catalog, hooks);
  Create(writer, "binlog.000001");
  const std::vector<std::uint8_t> bytes(S, 0x63);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  writer.Start();
  writing.acquire();
  ASSERT_EQ(cache->Append(bytes), AppendOutcome::Appended);
  auto producer = std::async(std::launch::async,
                             [&] { return cache->AppendOrWait(bytes); });
  EXPECT_TRUE(Until([&] { return cache->Counters().spaceWaits > 0; }));
  auto drain =
      std::async(std::launch::async, [&] { return writer.DrainAndSync(); });
  release.release();
  EXPECT_EQ(producer.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(producer.get(), AppendOutcome::Stopped);
  EXPECT_FALSE(drain.get());
  EXPECT_TRUE(writer.Failed());
  EXPECT_EQ(writer.BytesWritten(), S);
  EXPECT_FALSE(writer.DrainAndSync());
  EXPECT_EQ(writer.LastError(), "injected write failure");
}
TEST_F(StorageWriterTest, MissingDirectoryFailsAndStopWithoutStartIsSafe) {
  StorageWriter idle(directory.Directory(), *cache, catalog);
  idle.Stop();
  idle.Stop();
  EXPECT_FALSE(idle.DrainAndSync());
  StorageWriter writer(directory.Path("missing"), *cache, catalog);
  Create(writer, "binlog.000001");
  writer.Start();
  EXPECT_FALSE(writer.DrainAndSync());
  EXPECT_TRUE(writer.Failed());
  EXPECT_FALSE(writer.LastError().empty());
}
}  // namespace
}  // namespace binlog_streamer
