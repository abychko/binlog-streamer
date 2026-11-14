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
#include "storage/cStorageIndexRebuild.hpp"

#include "cTempDirectoryFixture.hpp"
#include "gtid/cGtidSet.hpp"
#include "hStoredFileWriter.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cDirectoryLock.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;
using test::WriteFreshFile;

GtidSet Gtids(const std::string &text) {
  GtidSet set;
  std::string error;
  EXPECT_TRUE(set.AddFromText(text, error)) << error;
  return set;
}

const std::string UUID = "3e11fa47-71ca-11e1-9e33-c80aa9429562";

std::vector<std::string> IndexOf(const std::filesystem::path &dataDir) {
  std::vector<std::string> names;
  std::string error;
  EXPECT_TRUE(
      BinlogIndexFile::Load((dataDir / INDEX_FILE_NAME).string(), names, error))
      << error;
  return names;
}

void WriteText(const std::filesystem::path &path, const std::string &text) {
  std::ofstream(path, std::ios::binary) << text;
}

bool Rebuild(const std::filesystem::path &dataDir,
             StorageIndexRebuild::Report &report, std::string &error) {
  bool accessProblem = false;
  return StorageIndexRebuild::Run(dataDir, report, accessProblem, error);
}

void WriteHistory(const TempDirectoryFixture &fixture) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, false, Gtids(UUID + ":1-5"));
  WriteFreshFile(fixture.Path("binlog.000004"), 7, false, Gtids(UUID + ":1-9"));
  WriteFreshFile(fixture.Path("binlog.000005"), 7, true, Gtids(UUID + ":1-12"));
}

TEST(StorageIndexRebuildTest, WritesAMissingIndexFromTheFilesInOrder) {
  TempDirectoryFixture fixture;
  WriteHistory(fixture);
  WriteText(fixture.Path(std::string(SERVER_UUID_FILE_NAME)), "[auto]\n");
  WriteText(fixture.Path("ca.pem"), "x");
  StorageIndexRebuild::Report report;
  std::string error;
  ASSERT_TRUE(Rebuild(fixture.Directory(), report, error)) << error;
  const std::vector<std::string> expected{"binlog.000003", "binlog.000004",
                                          "binlog.000005"};
  EXPECT_EQ(report.indexed, expected);
  EXPECT_FALSE(report.previousExisted);
  EXPECT_EQ(IndexOf(fixture.Directory()), expected);
  StorageCatalog catalog;
  bool indexExisted = false;
  ASSERT_TRUE(catalog.Load(fixture.Directory(), indexExisted, error)) << error;
  EXPECT_EQ(catalog.Size(), 3u);
}

TEST(StorageIndexRebuildTest, ReplacesAnIndexThatCannotBeRead) {
  TempDirectoryFixture fixture;
  WriteHistory(fixture);
  WriteText(fixture.Path(std::string(INDEX_FILE_NAME)), "binlog.000003\nbinl");
  StorageIndexRebuild::Report report;
  std::string error;
  ASSERT_TRUE(Rebuild(fixture.Directory(), report, error)) << error;
  EXPECT_TRUE(report.previousExisted);
  EXPECT_FALSE(report.previousError.empty());
  EXPECT_EQ(IndexOf(fixture.Directory()).size(), 3u);
}

TEST(StorageIndexRebuildTest, ReplacesAnIndexThatDoesNotMatchTheFiles) {
  TempDirectoryFixture fixture;
  WriteHistory(fixture);
  std::string error;
  ASSERT_TRUE(
      BinlogIndexFile::Replace((fixture.Directory() / INDEX_FILE_NAME).string(),
                               {"binlog.000002", "binlog.000003"}, error))
      << error;
  StorageIndexRebuild::Report report;
  ASSERT_TRUE(Rebuild(fixture.Directory(), report, error)) << error;
  EXPECT_EQ(report.previous,
            (std::vector<std::string>{"binlog.000002", "binlog.000003"}));
  EXPECT_EQ(IndexOf(fixture.Directory()),
            (std::vector<std::string>{"binlog.000003", "binlog.000004",
                                      "binlog.000005"}));
}

TEST(StorageIndexRebuildTest, LeavesOutANewestFileWhoseHeaderIsNotWhole) {
  TempDirectoryFixture fixture;
  WriteHistory(fixture);
  WriteText(fixture.Path("binlog.000006"), std::string("\xfe"
                                                       "bin",
                                                       4));
  StorageIndexRebuild::Report report;
  std::string error;
  ASSERT_TRUE(Rebuild(fixture.Directory(), report, error)) << error;
  EXPECT_EQ(report.leftOut, "binlog.000006");
  EXPECT_EQ(IndexOf(fixture.Directory()).back(), "binlog.000005");
  StorageCatalog catalog;
  bool indexExisted = false;
  ASSERT_TRUE(catalog.Load(fixture.Directory(), indexExisted, error)) << error;
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000006")));
}

TEST(StorageIndexRebuildTest, WritesAnEmptyIndexForADirectoryWithoutFiles) {
  TempDirectoryFixture fixture;
  StorageIndexRebuild::Report report;
  std::string error;
  ASSERT_TRUE(Rebuild(fixture.Directory(), report, error)) << error;
  EXPECT_TRUE(report.indexed.empty());
  EXPECT_TRUE(std::filesystem::exists(fixture.Directory() / INDEX_FILE_NAME));
}

class StorageIndexRebuildRefusal : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    ASSERT_TRUE(BinlogIndexFile::Replace(
        (fixture.Directory() / INDEX_FILE_NAME).string(), {}, error))
        << error;
  }
  void ExpectRefused(const std::string &reason) {
    StorageIndexRebuild::Report report;
    std::string error;
    EXPECT_FALSE(Rebuild(fixture.Directory(), report, error));
    EXPECT_NE(error.find(reason), std::string::npos) << error;
    EXPECT_TRUE(IndexOf(fixture.Directory()).empty());
  }
  TempDirectoryFixture fixture;
};

TEST_F(StorageIndexRebuildRefusal, AHoleInTheNumbers) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, false, Gtids(UUID + ":1-5"));
  WriteFreshFile(fixture.Path("binlog.000005"), 7, true, Gtids(UUID + ":1-12"));
  ExpectRefused("no file between binlog.000003 and binlog.000005");
}

TEST_F(StorageIndexRebuildRefusal, TheInUseBitOnAnOlderFile) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, true, Gtids(UUID + ":1-5"));
  WriteFreshFile(fixture.Path("binlog.000004"), 7, false, Gtids(UUID + ":1-9"));
  ExpectRefused("binlog.000003 has the \"file in use\" bit set");
}

TEST_F(StorageIndexRebuildRefusal, PreviousGtidsThatLoseTransactions) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, false, Gtids(UUID + ":1-9"));
  WriteFreshFile(fixture.Path("binlog.000004"), 7, false, Gtids(UUID + ":1-5"));
  ExpectRefused(
      "the Previous_gtids of binlog.000004 do not hold those of "
      "binlog.000003");
}

TEST_F(StorageIndexRebuildRefusal, AnOlderFileWhoseHeaderIsNotWhole) {
  WriteText(fixture.Path("binlog.000003"), std::string("\xfe"
                                                       "bin",
                                                       4));
  WriteFreshFile(fixture.Path("binlog.000004"), 7, false, Gtids(UUID + ":1-9"));
  ExpectRefused("binlog.000003: ");
}

TEST_F(StorageIndexRebuildRefusal, AFileThatIsNoBinaryLog) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, false);
  WriteText(fixture.Path("notes.txt"), "x");
  ExpectRefused("notes.txt: not a binary log");
}

TEST_F(StorageIndexRebuildRefusal, TwoBasenames) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, false);
  WriteFreshFile(fixture.Path("mysql-bin.000004"), 7, false);
  ExpectRefused("more than one basename");
}

TEST_F(StorageIndexRebuildRefusal, ADirectoryInUse) {
  WriteFreshFile(fixture.Path("binlog.000003"), 7, true);
  DirectoryLock held;
  bool accessProblem = false;
  std::string error;
  ASSERT_TRUE(held.Acquire(fixture.Directory(), accessProblem, error)) << error;
  StorageIndexRebuild::Report report;
  EXPECT_FALSE(Rebuild(fixture.Directory(), report, error));
  EXPECT_TRUE(IndexOf(fixture.Directory()).empty());
}

}  // namespace
}  // namespace binlog_streamer
