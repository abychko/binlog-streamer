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
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include "cTempDirectoryFixture.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStoragePurger.hpp"
#include "storage/hStorageDefaults.hpp"

namespace binlog_streamer {
namespace {
using namespace std::chrono_literals;
constexpr auto BIG = std::numeric_limits<std::uint64_t>::max();
using Names = std::vector<std::string>;

class StoragePurgerTest : public ::testing::Test {
 protected:
  test::TempDirectoryFixture directory;
  StorageCatalog catalog;
  PurgeResult result;
  std::string error;

  static std::string Name(unsigned n) {
    return "binlog.00000" + std::to_string(n);
  }
  static std::vector<StoredFileRecord> Records(unsigned count) {
    std::vector<StoredFileRecord> records;
    for (unsigned n = 1; n <= count; ++n) {
      StoredFileRecord record;
      record.name = Name(n);
      record.createdAt = n * 100;
      record.inUse = n == count;
      records.push_back(record);
    }
    return records;
  }
  static std::vector<StoredFileRecord> Sized(unsigned count,
                                             std::uint64_t size) {
    auto records = Records(count);
    for (auto &record : records) record.size = size;
    return records;
  }
  static SpaceBudget S(std::uint64_t high, std::uint64_t low,
                       std::uint64_t minimum, std::uint64_t available) {
    return {high, low, minimum, available};
  }
  void Seed(const std::vector<StoredFileRecord> &records) {
    Names names;
    for (const auto &record : records) {
      catalog.Add(record);
      if (record.onDisk) {
        std::ofstream file(directory.Path(record.name));
        file << "data";
        ASSERT_TRUE(file.good());
        names.push_back(record.name);
      }
    }
    ASSERT_TRUE(BinlogIndexFile::Replace(
        (directory.Directory() / INDEX_FILE_NAME).string(), names, error))
        << error;
  }
  Names Index() {
    Names names;
    EXPECT_TRUE(BinlogIndexFile::Load(
        (directory.Directory() / INDEX_FILE_NAME).string(), names, error))
        << error;
    return names;
  }
  void Check(const Names &expected, const std::string &retainedDirectory = {}) {
    EXPECT_EQ(result.purged, expected);
    Names names;
    for (std::size_t i = 0; i < catalog.Size(); ++i) {
      const auto record = catalog.At(i);
      if (record.onDisk) {
        names.push_back(record.name);
        EXPECT_TRUE(std::filesystem::exists(directory.Path(record.name)));
      }
    }
    EXPECT_EQ(Index(), names);
    for (const auto &name : expected) {
      if (name != retainedDirectory) {
        EXPECT_FALSE(std::filesystem::exists(directory.Path(name)));
      }
      std::string pinError;
      EXPECT_FALSE(catalog.Pin(name, pinError).has_value());
    }
  }
};

TEST_F(StoragePurgerTest, BoundaryIsStrict) {
  Seed(Records(3));
  StoragePurger purger(directory.Directory(), catalog);
  struct stat before{}, after{};
  ASSERT_EQ(::stat((directory.Directory() / INDEX_FILE_NAME).c_str(), &before),
            0);
  result.purged = {"stale"};
  result.warning = "stale";
  ASSERT_TRUE(purger.Purge({.cutoff = 200, .space = {}}, "", result, error))
      << error;
  Check({});
  EXPECT_TRUE(result.warning.empty());
  ASSERT_EQ(::stat((directory.Directory() / INDEX_FILE_NAME).c_str(), &after),
            0);
  EXPECT_EQ(before.st_ino, after.st_ino);
  ASSERT_TRUE(purger.Purge({.cutoff = 201, .space = {}}, "", result, error))
      << error;
  Check({Name(1)});
}

TEST_F(StoragePurgerTest, LastFileStays) {
  auto records = Records(2);
  records.back().inUse = false;
  Seed(records);
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({Name(1)});
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({});
  StorageCatalog single;
  single.Add(records.back());
  StoragePurger one(directory.Directory(), single);
  ASSERT_TRUE(one.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  EXPECT_TRUE(result.purged.empty());
  EXPECT_EQ(single.Size(), 1u);
  Check({});
}

TEST_F(StoragePurgerTest, PinnedFileStopsThePass) {
  Seed(Records(4));
  auto pin = catalog.Pin(Name(2), error);
  ASSERT_TRUE(pin.has_value());
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({Name(1)});
  EXPECT_TRUE(result.warning.empty());
  EXPECT_TRUE(error.empty());
  pin.reset();
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({Name(2), Name(3)});
}

TEST_F(StoragePurgerTest, FileOpenOnDiskStays) {
  Seed(Records(3));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(
      purger.Purge({.cutoff = BIG, .space = {}}, Name(1), result, error))
      << error;
  Check({});
  ASSERT_TRUE(
      purger.Purge({.cutoff = BIG, .space = {}}, Name(2), result, error))
      << error;
  Check({Name(1)});
}

TEST_F(StoragePurgerTest, FileNotYetOnDiskStops) {
  auto records = Records(3);
  records[1].onDisk = false;
  records[2].onDisk = false;
  Seed(records);
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({Name(1)});
  EXPECT_EQ(Index(), Names{});
}

TEST_F(StoragePurgerTest, OpenRecordStops) {
  auto records = Records(2);
  records[0].inUse = true;
  Seed(records);
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({});
}

TEST_F(StoragePurgerTest, UnknownSuccessorTimeStops) {
  auto records = Records(3);
  records[1].createdAt = 0;
  Seed(records);
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({});
}

TEST_F(StoragePurgerTest, IndexDropsNamesBeforeAnyUnlink) {
  Seed(Records(5));
  Names calls;
  StoragePurgerHooks hooks;
  hooks.beforeUnlink = [&](const std::string &name) {
    EXPECT_EQ(Index(), (Names{Name(4), Name(5)}));
    EXPECT_TRUE(std::filesystem::exists(directory.Path(name)));
    calls.push_back(name);
  };
  StoragePurger purger(directory.Directory(), catalog, hooks);
  ASSERT_TRUE(purger.Purge({.cutoff = 401, .space = {}}, "", result, error))
      << error;
  Check({Name(1), Name(2), Name(3)});
  EXPECT_EQ(calls, result.purged);
}

TEST_F(StoragePurgerTest, MissingFileIsNotAnError) {
  Seed(Records(3));
  ASSERT_TRUE(std::filesystem::remove(directory.Path(Name(1))));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({Name(1), Name(2)});
  EXPECT_TRUE(result.warning.empty());
}

TEST_F(StoragePurgerTest, UnlinkFailureWarnsAndContinues) {
  Seed(Records(3));
  ASSERT_TRUE(std::filesystem::remove(directory.Path(Name(1))));
  ASSERT_TRUE(std::filesystem::create_directory(directory.Path(Name(1))));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error))
      << error;
  Check({Name(1), Name(2)}, Name(1));
  EXPECT_TRUE(std::filesystem::is_directory(directory.Path(Name(1))));
  EXPECT_NE(result.warning.find(Name(1)), std::string::npos);
  EXPECT_EQ(std::count(result.warning.begin(), result.warning.end(), '\n'), 0);
  EXPECT_EQ(Index(), Names{Name(3)});
}

TEST_F(StoragePurgerTest, IndexWriteFailureLeavesFiles) {
  Seed(Records(3));
  ASSERT_TRUE(
      std::filesystem::create_directory(directory.Path("binlog.index.tmp")));
  unsigned calls = 0;
  StoragePurgerHooks hooks;
  hooks.beforeUnlink = [&](const std::string &) { ++calls; };
  StoragePurger purger(directory.Directory(), catalog, hooks);
  EXPECT_FALSE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(calls, 0u);
  for (unsigned n = 1; n <= 3; ++n)
    EXPECT_TRUE(std::filesystem::exists(directory.Path(Name(n))));
}

TEST_F(StoragePurgerTest, IndexCatalogMismatchRefuses) {
  Seed(Records(3));
  ASSERT_TRUE(BinlogIndexFile::Replace(
      (directory.Directory() / INDEX_FILE_NAME).string(), {Name(2), Name(3)},
      error));
  unsigned calls = 0;
  StoragePurgerHooks hooks;
  hooks.beforeUnlink = [&](const std::string &) { ++calls; };
  StoragePurger purger(directory.Directory(), catalog, hooks);
  EXPECT_FALSE(purger.Purge({.cutoff = BIG, .space = {}}, "", result, error));
  EXPECT_NE(error.find(Name(1)), std::string::npos);
  EXPECT_EQ(calls, 0u);
  for (unsigned n = 1; n <= 3; ++n)
    EXPECT_TRUE(std::filesystem::exists(directory.Path(Name(n))));
}

TEST_F(StoragePurgerTest, CutoffSubtractsPeriod) {
  EXPECT_EQ(StoragePurger::Cutoff(1000, 100s), 900u);
  EXPECT_EQ(StoragePurger::Cutoff(100, 100s), 0u);
  EXPECT_EQ(StoragePurger::Cutoff(50, 100s), 0u);
  EXPECT_EQ(StoragePurger::Cutoff(1000, 0s), 1000u);
  EXPECT_EQ(StoragePurger::Cutoff(1000, -1s), 1000u);
}
TEST_F(StoragePurgerTest, SpaceAtHighWatermarkKeepsFiles) {
  Seed(Sized(5, 10));
  StoragePurger purger(directory.Directory(), catalog);
  struct stat before{}, after{};
  ASSERT_EQ(::stat((directory.Directory() / INDEX_FILE_NAME).c_str(), &before),
            0);
  ASSERT_TRUE(purger.Purge({.cutoff = {}, .space = S(50, 20, 0, 100)}, "",
                           result, error))
      << error;
  Check({});
  ASSERT_EQ(::stat((directory.Directory() / INDEX_FILE_NAME).c_str(), &after),
            0);
  EXPECT_EQ(before.st_ino, after.st_ino);
  EXPECT_FALSE(result.spaceShort);
  EXPECT_EQ(result.usedBytes, 50u);
  EXPECT_EQ(result.availableBytes, 100u);
}
TEST_F(StoragePurgerTest, SpaceAboveHighPurgesDownToLow) {
  Seed(Sized(5, 10));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = {}, .space = S(49, 20, 0, 100)}, "",
                           result, error))
      << error;
  Check({Name(1), Name(2), Name(3)});
  EXPECT_EQ(result.usedBytes, 20u);
  EXPECT_EQ(result.availableBytes, 130u);
  EXPECT_FALSE(result.spaceShort);
}
TEST_F(StoragePurgerTest, SpaceIgnoresAgeAndUnknownTime) {
  auto records = Sized(5, 10);
  records[1].createdAt = 0;
  Seed(records);
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = 0, .space = S(49, 20, 0, 100)}, "",
                           result, error))
      << error;
  Check({Name(1), Name(2), Name(3)});
}
TEST_F(StoragePurgerTest, LowFreeSpacePurgesUntilMinimum) {
  Seed(Sized(5, 10));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = {}, .space = S(1000, 900, 25, 5)}, "",
                           result, error))
      << error;
  Check({Name(1), Name(2)});
  EXPECT_EQ(result.availableBytes, 25u);
  EXPECT_FALSE(result.spaceShort);
}
TEST_F(StoragePurgerTest, FreeSpaceAtMinimumKeepsFiles) {
  Seed(Sized(5, 10));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = {}, .space = S(1000, 20, 25, 25)}, "",
                           result, error))
      << error;
  Check({});
}
TEST_F(StoragePurgerTest, BothTargetsMustHold) {
  Seed(Sized(5, 10));
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = {}, .space = S(45, 40, 30, 0)}, "",
                           result, error))
      << error;
  Check({Name(1), Name(2), Name(3)});
  EXPECT_EQ(result.usedBytes, 20u);
  EXPECT_EQ(result.availableBytes, 30u);
}
TEST_F(StoragePurgerTest, SpaceStopsAtPinnedFileAndLastFile) {
  Seed(Sized(5, 10));
  auto pin = catalog.Pin(Name(3), error);
  ASSERT_TRUE(pin.has_value());
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(
      purger.Purge({.cutoff = {}, .space = S(0, 0, 0, 100)}, "", result, error))
      << error;
  Check({Name(1), Name(2)});
  EXPECT_TRUE(result.spaceShort);
  EXPECT_EQ(result.usedBytes, 30u);
  pin.reset();
  ASSERT_TRUE(
      purger.Purge({.cutoff = {}, .space = S(0, 0, 0, 120)}, "", result, error))
      << error;
  Check({Name(3), Name(4)});
  EXPECT_TRUE(result.spaceShort);
  EXPECT_EQ(result.usedBytes, 10u);
  EXPECT_EQ(result.availableBytes, 140u);
}
TEST_F(StoragePurgerTest, SpaceShortComparesWithHighNotLow) {
  Seed(Sized(5, 10));
  auto pin = catalog.Pin(Name(4), error);
  ASSERT_TRUE(pin.has_value());
  StoragePurger purger(directory.Directory(), catalog);
  ASSERT_TRUE(purger.Purge({.cutoff = {}, .space = S(25, 5, 0, 100)}, "",
                           result, error))
      << error;
  Check({Name(1), Name(2), Name(3)});
  EXPECT_EQ(result.usedBytes, 20u);
  EXPECT_FALSE(result.spaceShort);
}
TEST_F(StoragePurgerTest, ExpiryAndSpaceShareOneIndexWrite) {
  Seed(Sized(5, 10));
  Names calls;
  StoragePurgerHooks hooks;
  hooks.beforeUnlink = [&](const std::string &name) {
    EXPECT_EQ(Index(), (Names{Name(3), Name(4), Name(5)}));
    calls.push_back(name);
  };
  StoragePurger purger(directory.Directory(), catalog, hooks);
  ASSERT_TRUE(purger.Purge({.cutoff = 201, .space = S(39, 30, 0, 100)}, "",
                           result, error))
      << error;
  Check({Name(1), Name(2)});
  EXPECT_EQ(calls, (Names{Name(1), Name(2)}));
}
TEST_F(StoragePurgerTest, NoLimitsTouchNothing) {
  Seed(Sized(5, 10));
  StoragePurger purger(directory.Directory(), catalog);
  struct stat before{}, after{};
  ASSERT_EQ(::stat((directory.Directory() / INDEX_FILE_NAME).c_str(), &before),
            0);
  ASSERT_TRUE(purger.Purge({}, "", result, error)) << error;
  Check({});
  EXPECT_FALSE(result.spaceShort);
  ASSERT_EQ(::stat((directory.Directory() / INDEX_FILE_NAME).c_str(), &after),
            0);
  EXPECT_EQ(before.st_ino, after.st_ino);
}
}  // namespace
}  // namespace binlog_streamer
