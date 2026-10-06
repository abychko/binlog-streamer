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
#include "storage/cBinlogIndexFile.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

TEST(BinlogIndexFileTest, RoundTripsThroughReplaceAndLoad) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();
  const std::vector<std::string> names = {"binlog.000001", "binlog.000002",
                                          "binlog.000003"};

  std::string error;
  ASSERT_TRUE(BinlogIndexFile::Replace(path, names, error)) << error;

  std::vector<std::string> loaded;
  ASSERT_TRUE(BinlogIndexFile::Load(path, loaded, error)) << error;
  EXPECT_EQ(loaded, names);
}

TEST(BinlogIndexFileTest, ReplaceLeavesNoTmpFileBehind) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();
  const auto tmpPath = fixture.Path("binlog.index.tmp");

  std::string error;
  ASSERT_TRUE(BinlogIndexFile::Replace(path, {"binlog.000001"}, error))
      << error;
  EXPECT_FALSE(std::filesystem::exists(tmpPath));

  ASSERT_TRUE(
      BinlogIndexFile::Replace(path, {"binlog.000001", "binlog.000002"}, error))
      << error;
  EXPECT_FALSE(std::filesystem::exists(tmpPath));
}

TEST(BinlogIndexFileTest, LoadOnMissingIndexReturnsAnEmptyIndexNotAnError) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();

  std::vector<std::string> names = {"leftover"};
  std::string error;
  ASSERT_TRUE(BinlogIndexFile::Load(path, names, error)) << error;
  EXPECT_TRUE(names.empty());
  EXPECT_TRUE(error.empty());
}

TEST(BinlogIndexFileTest, LoadWithLeftoverTmpReadsTheOldIndexUnaffected) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();
  const auto tmpPath = fixture.Path("binlog.index.tmp");

  std::string error;
  ASSERT_TRUE(
      BinlogIndexFile::Replace(path, {"binlog.000001", "binlog.000002"}, error))
      << error;

  {
    std::ofstream tmp(tmpPath, std::ios::binary);
    tmp << "binlog.999999\n";
  }

  std::vector<std::string> loaded;
  ASSERT_TRUE(BinlogIndexFile::Load(path, loaded, error)) << error;
  EXPECT_EQ(loaded,
            (std::vector<std::string>{"binlog.000001", "binlog.000002"}));
  EXPECT_TRUE(std::filesystem::exists(tmpPath));
}

TEST(BinlogIndexFileTest, LoadRejectsAnEmptyLine) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path(std::string(INDEX_FILE_NAME));
  {
    std::ofstream file(path, std::ios::binary);
    file << "binlog.000001\n\nbinlog.000002\n";
  }

  std::vector<std::string> names;
  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Load(path.string(), names, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(names.empty());
}

TEST(BinlogIndexFileTest, LoadRejectsALineThatIsAPathRatherThanABareName) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path(std::string(INDEX_FILE_NAME));
  {
    std::ofstream file(path, std::ios::binary);
    file << "data/binlog.000001\n";
  }

  std::vector<std::string> names;
  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Load(path.string(), names, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(names.empty());
}

TEST(BinlogIndexFileTest, LoadRejectsADuplicateName) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path(std::string(INDEX_FILE_NAME));
  {
    std::ofstream file(path, std::ios::binary);
    file << "binlog.000001\nbinlog.000002\nbinlog.000001\n";
  }

  std::vector<std::string> names;
  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Load(path.string(), names, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(names.empty());
}

TEST(BinlogIndexFileTest, LoadRejectsACarriageReturnLeftInTheName) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path(std::string(INDEX_FILE_NAME));
  {
    std::ofstream file(path, std::ios::binary);
    file << "binlog.000001\r\n";
  }

  std::vector<std::string> names;
  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Load(path.string(), names, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(names.empty());
}

TEST(BinlogIndexFileTest, LoadRejectsAFileMissingItsFinalNewline) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path(std::string(INDEX_FILE_NAME));
  {
    std::ofstream file(path, std::ios::binary);
    file << "binlog.000001\nbinlog.000002";
  }

  std::vector<std::string> names;
  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Load(path.string(), names, error));
  EXPECT_NE(error.find("newline"), std::string::npos) << error;
  EXPECT_TRUE(names.empty());
}

TEST(BinlogIndexFileTest, ReplaceRejectsAnEmptyName) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();

  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Replace(path, {""}, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(BinlogIndexFileTest, ReplaceRejectsANameContainingAPathSeparator) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();

  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Replace(path, {"data/binlog.000001"}, error));
  EXPECT_FALSE(error.empty());
}

TEST(BinlogIndexFileTest, ReplaceRejectsANameContainingAnEmbeddedNewline) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();

  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Replace(path, {"bin\nlog.000001"}, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(BinlogIndexFileTest, ReplaceRejectsADuplicateName) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();

  std::string error;
  EXPECT_FALSE(BinlogIndexFile::Replace(
      path, {"binlog.000001", "binlog.000001"}, error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(BinlogIndexFileTest, AppendGrowsAFreshIndexOneNameAtATime) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();

  std::string error;
  ASSERT_TRUE(BinlogIndexFile::Append(path, "binlog.000001", error)) << error;
  ASSERT_TRUE(BinlogIndexFile::Append(path, "binlog.000002", error)) << error;

  std::vector<std::string> loaded;
  ASSERT_TRUE(BinlogIndexFile::Load(path, loaded, error)) << error;
  EXPECT_EQ(loaded,
            (std::vector<std::string>{"binlog.000001", "binlog.000002"}));
}

TEST(BinlogIndexFileTest, IndexIsNeverPartiallyOrMissingWhileReplaced) {
  TempDirectoryFixture fixture;
  const std::string path = fixture.Path(std::string(INDEX_FILE_NAME)).string();
  std::string error;
  ASSERT_TRUE(
      BinlogIndexFile::Replace(path, {"binlog.000001", "binlog.000002"}, error))
      << error;

  std::atomic<bool> stop{false};
  std::atomic<bool> sawBadIndex{false};
  std::thread checker([&] {
    std::string checkerError;
    while (!stop.load(std::memory_order_relaxed)) {
      std::vector<std::string> seen;
      if (!BinlogIndexFile::Load(path, seen, checkerError) ||
          seen.size() != 2) {
        sawBadIndex.store(true, std::memory_order_relaxed);
        break;
      }
    }
  });

  constexpr int iterations = 500;
  for (int i = 0;
       i < iterations && !sawBadIndex.load(std::memory_order_relaxed); ++i) {
    const std::vector<std::string> names = {
        "binlog.000001", "binlog.00000" + std::to_string(i % 8 + 2)};
    if (!BinlogIndexFile::Replace(path, names, error)) {
      stop.store(true, std::memory_order_relaxed);
      checker.join();
      FAIL() << error;
    }
  }

  stop.store(true, std::memory_order_relaxed);
  checker.join();
  EXPECT_FALSE(sawBadIndex.load());
}

}  // namespace
}  // namespace binlog_streamer
