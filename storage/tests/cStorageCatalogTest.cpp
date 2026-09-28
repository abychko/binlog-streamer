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

#include "storage/cStorageCatalog.hpp"

#include "cTempDirectoryFixture.hpp"
#include "gtid/cGtidSet.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

// A minimal but valid stored file: magic + FDE (server 5.5.62, no checksum
// trailer) + an empty Previous_gtids_event, nothing past it.
void WriteFreshFile(const std::filesystem::path &path, std::uint32_t serverId,
                    bool inUse) {
  std::vector<std::uint8_t> fdeBody(57, 0x00);
  fdeBody[0] = 3;  // binlogVersion
  const std::string version = "5.5.62";
  for (std::size_t i = 0; i < version.size(); ++i)
    fdeBody[2 + i] = static_cast<std::uint8_t>(version[i]);
  fdeBody[56] = 19;  // commonHeaderLength

  const auto pgeBody =
      GtidSet().Encode(/*skipTaggedGtids=*/false);  // 8 zero bytes, empty set

  std::vector<std::uint8_t> file{0xfe, 0x62, 0x69, 0x6e};  // BINLOG_MAGIC

  const std::uint32_t fdeEventLength =
      static_cast<std::uint32_t>(19 + fdeBody.size());
  std::vector<std::uint8_t> fdeHeader(19, 0x00);
  fdeHeader[4] = 15;  // FormatDescription
  fdeHeader[5] = static_cast<std::uint8_t>(serverId);
  fdeHeader[9] = static_cast<std::uint8_t>(fdeEventLength);
  fdeHeader[13] = static_cast<std::uint8_t>(4 + fdeEventLength);
  fdeHeader[17] = inUse ? 0x01 : 0x00;  // LOG_EVENT_BINLOG_IN_USE_F
  file.insert(file.end(), fdeHeader.begin(), fdeHeader.end());
  file.insert(file.end(), fdeBody.begin(), fdeBody.end());

  const std::uint32_t pgeEventLength =
      static_cast<std::uint32_t>(19 + pgeBody.size());
  std::vector<std::uint8_t> pgeHeader(19, 0x00);
  pgeHeader[4] = 35;  // PreviousGtids
  pgeHeader[5] = static_cast<std::uint8_t>(serverId);
  pgeHeader[9] = static_cast<std::uint8_t>(pgeEventLength);
  file.insert(file.end(), pgeHeader.begin(), pgeHeader.end());
  file.insert(file.end(), pgeBody.begin(), pgeBody.end());

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(file.data()),
            static_cast<std::streamsize>(file.size()));
}

void WriteIndex(const std::filesystem::path &dataDir,
                const std::vector<std::string> &names) {
  std::string error;
  ASSERT_TRUE(BinlogIndexFile::Replace((dataDir / INDEX_FILE_NAME).string(),
                                       names, error))
      << error;
}

// Most tests below do not care whether the index existed beforehand -
// the few that do call catalog.Load() directly instead of this helper.
bool LoadCatalog(StorageCatalog &catalog, const std::filesystem::path &dataDir,
                 std::string &error) {
  bool indexExisted = false;
  return catalog.Load(dataDir, indexExisted, error);
}

TEST(StorageCatalogTest, LoadsAnEmptyDataDirectory) {
  TempDirectoryFixture fixture;
  StorageCatalog catalog;
  std::string error;
  ASSERT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_EQ(catalog.Size(), 0u);
}

TEST(StorageCatalogTest, LoadsFilesListedInTheIndexIntoTheCatalogInOrder) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/7,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/7, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001", "binlog.000002"});

  StorageCatalog catalog;
  std::string error;
  ASSERT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  ASSERT_EQ(catalog.Size(), 2u);
  EXPECT_EQ(catalog.At(0).name, "binlog.000001");
  EXPECT_EQ(catalog.At(0).number, 1u);
  EXPECT_FALSE(catalog.At(0).inUse);
  EXPECT_EQ(catalog.At(1).name, "binlog.000002");
  EXPECT_EQ(catalog.At(1).number, 2u);
  EXPECT_TRUE(
      catalog.At(1)
          .inUse);  // the last file in the index is allowed to still be open
  EXPECT_EQ(catalog.At(1).serverId, 7u);
  EXPECT_TRUE(catalog.At(1).previousGtids.IsEmpty());
}

TEST(StorageCatalogTest, RemovesALeftoverIndexTmpFile) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  {
    std::ofstream tmp(fixture.Path("binlog.index.tmp"));
    tmp << "leftover from an interrupted Replace()";
  }

  StorageCatalog catalog;
  std::string error;
  ASSERT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.index.tmp")));
}

// ".tmp" cleanup is unconditional housekeeping, independent of any
// remnant/refusal decision on the digits-named files.
TEST(StorageCatalogTest,
     RemovesTheIndexTmpFileEvenWhenARefusalExistsElsewhere) {
  TempDirectoryFixture fixture;
  {
    std::ofstream tmp(fixture.Path("binlog.index.tmp"));
    tmp << "leftover from an interrupted Replace()";
  }
  {
    std::ofstream stray(fixture.Path("not-a-binlog-name"));
    stray << "garbage";
  }  // forces a refusal

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.index.tmp")));
}

TEST(StorageCatalogTest,
     RemovesAFileNewerThanTheLastIndexedNumberAsAnInterruptedCreate) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1,
                 /*inUse=*/true);  // index append never happened

  StorageCatalog catalog;
  std::string error;
  ASSERT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_EQ(catalog.Size(), 1u);
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000002")));
}

TEST(StorageCatalogTest,
     RemovesAFileOlderThanTheFirstIndexedNumberAsAnUnfinishedPurge) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000005"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000005"});
  WriteFreshFile(fixture.Path("binlog.000003"), /*serverId=*/1,
                 /*inUse=*/false);  // purge never finished removing it

  StorageCatalog catalog;
  std::string error;
  ASSERT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_EQ(catalog.Size(), 1u);
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000003")));
}

// A file inside the indexed range with no safe rule must be refused, not
// swept up as a remnant.
TEST(StorageCatalogTest, RefusesAFileInsideTheIndexedRangeThatIsNotIndexed) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("binlog.000003"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001", "binlog.000003"});
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1,
                 /*inUse=*/false);  // not indexed, not a safe remnant

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("binlog.000002"), std::string::npos) << error;
  EXPECT_TRUE(std::filesystem::exists(
      fixture.Path("binlog.000002")));  // refused, not deleted
}

TEST(StorageCatalogTest, RefusesAnIndexedFileMissingFromDisk) {
  TempDirectoryFixture fixture;
  WriteIndex(fixture.Directory(),
             {"binlog.000001"});  // never actually written to disk

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("binlog.000001"), std::string::npos) << error;
}

TEST(StorageCatalogTest, RefusesWhenAnEarlierFileHasTheInUseBitSet) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1,
                 /*inUse=*/true);  // not the last file - illegal
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteIndex(fixture.Directory(), {"binlog.000001", "binlog.000002"});

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("binlog.000001"), std::string::npos) << error;
}

// Every indexed file's header is read and checked before anything is
// deleted, so a remnant survives an IN_USE-bit violation elsewhere.
TEST(StorageCatalogTest,
     LeavesARemnantOnDiskWhenAnEarlierFileHasTheInUseBitSet) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1,
                 /*inUse=*/true);  // not the last file - illegal
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteIndex(fixture.Directory(), {"binlog.000001", "binlog.000002"});
  WriteFreshFile(fixture.Path("binlog.000003"), /*serverId=*/1,
                 /*inUse=*/false);  // would otherwise be a safe ">last" remnant

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000003")));
}

// Same as the test above, for the other phase-two failure: an unreadable
// header.
TEST(StorageCatalogTest,
     LeavesARemnantOnDiskWhenAnIndexedFilesHeaderCannotBeRead) {
  TempDirectoryFixture fixture;
  {
    std::ofstream truncated(fixture.Path("binlog.000001"), std::ios::binary);
    truncated << "short";
  }
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1,
                 /*inUse=*/false);  // would otherwise be a safe ">last" remnant

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000002")));
}

// Nothing is deleted while any refusal exists, even a legitimate remnant
// elsewhere in the directory.
TEST(StorageCatalogTest, DoesNotDeleteAnythingWhenAnyRefusalExistsElsewhere) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(
      fixture.Directory(),
      {"binlog.000001", "binlog.000002"});  // 000002 indexed but never written
  WriteFreshFile(fixture.Path("binlog.000003"), /*serverId=*/1,
                 /*inUse=*/true);  // would otherwise be a safe ">last" remnant

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_TRUE(std::filesystem::exists(fixture.Path(
      "binlog.000003")));  // nothing deleted despite being a would-be remnant
}

TEST(StorageCatalogTest, RefusesAnUnparsableStrayFileName) {
  TempDirectoryFixture fixture;
  {
    std::ofstream stray(fixture.Path("not-a-binlog-name"));
    stray << "garbage";
  }

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("not-a-binlog-name"), std::string::npos) << error;
}

TEST(StorageCatalogTest, AcceptsTheServerUuidFileNextToTheBinaryLogs) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  {
    std::ofstream uuidFile(fixture.Path("auto.cnf"));
    uuidFile << "[auto]\nserver-uuid=8a94f357-aab4-11df-86ab-c80aa9429562\n";
  }

  StorageCatalog catalog;
  std::string error;
  EXPECT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_EQ(catalog.Size(), 1u);
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("auto.cnf")));
}

TEST(StorageCatalogTest, AcceptsTheTlsFilesNextToTheBinaryLogs) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  for (const char *name :
       {"ca.pem", "ca-key.pem", "server-cert.pem", "server-key.pem"}) {
    std::ofstream pem(fixture.Path(name));
    pem << "-----BEGIN X-----\n";
  }

  StorageCatalog catalog;
  std::string error;
  EXPECT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_EQ(catalog.Size(), 1u);
}

TEST(StorageCatalogTest, RefusesAFileWithADifferentBasenameThanTheIndex) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  WriteFreshFile(fixture.Path("other.000002"), /*serverId=*/1,
                 /*inUse=*/false);  // a different source's naming

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("other.000002"), std::string::npos) << error;
  // A distinct reason: this basename matches neither index edge.
  EXPECT_NE(error.find("basename matches neither"), std::string::npos) << error;
}

// A missing index is a state this project is only ever in before creating
// its first file, so a stray file here is refused, not swept up as a remnant.
TEST(StorageCatalogTest, RefusesASingleFileWhenTheIndexIsMissingEntirely) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  // binlog.index never written at all.

  StorageCatalog catalog;
  bool indexExisted = true;  // starts wrong on purpose - Load() has to set it
  std::string error;
  EXPECT_FALSE(catalog.Load(fixture.Directory(), indexExisted, error));
  EXPECT_FALSE(indexExisted);
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000001")));
}

TEST(StorageCatalogTest, RefusesTwoFilesWhenTheIndexIsMissingEntirely) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1, /*inUse=*/true);
  // binlog.index never written at all.

  StorageCatalog catalog;
  bool indexExisted = true;
  std::string error;
  EXPECT_FALSE(catalog.Load(fixture.Directory(), indexExisted, error));
  EXPECT_FALSE(indexExisted);
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000001")));
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000002")));
}

// An index present but empty is the window between writing it and the
// first file's append succeeding, so one file here is a legitimate remnant.
TEST(StorageCatalogTest, RemovesASingleFileWhenTheIndexIsPresentButEmpty) {
  TempDirectoryFixture fixture;
  WriteIndex(fixture.Directory(), {});  // present, zero entries
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1,
                 /*inUse=*/true);  // its own index append never happened

  StorageCatalog catalog;
  bool indexExisted = false;
  std::string error;
  ASSERT_TRUE(catalog.Load(fixture.Directory(), indexExisted, error)) << error;
  EXPECT_TRUE(indexExisted);
  EXPECT_EQ(catalog.Size(), 0u);
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000001")));
}

// Two files past an empty index can't both be the one pending append, so
// both are refused.
TEST(StorageCatalogTest, RefusesTwoFilesWhenTheIndexIsPresentButEmpty) {
  TempDirectoryFixture fixture;
  WriteIndex(fixture.Directory(), {});
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1, /*inUse=*/true);

  StorageCatalog catalog;
  bool indexExisted = false;
  std::string error;
  EXPECT_FALSE(catalog.Load(fixture.Directory(), indexExisted, error));
  EXPECT_TRUE(indexExisted);
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000001")));
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000002")));
}

// Two files past the last indexed number can't both be the one pending
// append either, so both are refused.
TEST(StorageCatalogTest, RefusesTwoFilesNewerThanTheLastIndexedNumber) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000001"), /*serverId=*/1, /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000001"});
  WriteFreshFile(fixture.Path("binlog.000002"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("binlog.000003"), /*serverId=*/1, /*inUse=*/true);

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("binlog.000002"), std::string::npos) << error;
  EXPECT_NE(error.find("binlog.000003"), std::string::npos) << error;
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000002")));
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000003")));
}

// A legal basename change mid-index leaves first/last entries without a
// shared basename; a stray file sharing only the first entry's basename is
// not a safe remnant at either edge, so it is refused.
TEST(StorageCatalogTest,
     RefusesAStrayFileSharingTheFirstEntrysBasenameButOutsideItsRemnantRange) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000010"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("mysql-bin.000001"), /*serverId=*/1,
                 /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000010", "mysql-bin.000001"});
  WriteFreshFile(fixture.Path("binlog.000011"), /*serverId=*/1,
                 /*inUse=*/false);

  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(LoadCatalog(catalog, fixture.Directory(), error));
  EXPECT_NE(error.find("binlog.000011"), std::string::npos) << error;
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.000011")));
}

// The other edge: a stray file sharing the last entry's basename and past
// its number is a genuine remnant, judged against that entry.
TEST(StorageCatalogTest,
     RemovesAStrayFileSharingTheLastEntrysBasenameAndPastItsNumber) {
  TempDirectoryFixture fixture;
  WriteFreshFile(fixture.Path("binlog.000010"), /*serverId=*/1,
                 /*inUse=*/false);
  WriteFreshFile(fixture.Path("mysql-bin.000001"), /*serverId=*/1,
                 /*inUse=*/true);
  WriteIndex(fixture.Directory(), {"binlog.000010", "mysql-bin.000001"});
  WriteFreshFile(fixture.Path("mysql-bin.000002"), /*serverId=*/1,
                 /*inUse=*/false);

  StorageCatalog catalog;
  std::string error;
  ASSERT_TRUE(LoadCatalog(catalog, fixture.Directory(), error)) << error;
  EXPECT_EQ(catalog.Size(), 2u);
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("mysql-bin.000002")));
}

TEST(StorageCatalogTest, AddAppendsARecordAndCloseUpdatesItsSizeAndInUseFlag) {
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  record.inUse = true;
  record.size = 90;  // header-only size, before any event bytes
  catalog.Add(record);
  ASSERT_EQ(catalog.Size(), 1u);
  EXPECT_TRUE(catalog.At(0).inUse);

  std::string error;
  ASSERT_TRUE(catalog.Close(/*finalSize=*/1234, error)) << error;
  EXPECT_FALSE(catalog.At(0).inUse);
  EXPECT_EQ(catalog.At(0).size, 1234u);
}

// Callers that want the oldest or the newest record take it in one lock:
// an index from Size() can name another record, or none, by the time At()
// reads it, because Remove() drops from the front.
TEST(StorageCatalogTest, FirstAndLastFollowTheEndsOfTheCatalog) {
  StorageCatalog catalog;
  EXPECT_FALSE(catalog.First().has_value());
  EXPECT_FALSE(catalog.Last().has_value());

  StoredFileRecord first;
  first.name = "binlog.000001";
  catalog.Add(first);
  ASSERT_TRUE(catalog.First().has_value());
  EXPECT_EQ(catalog.First()->name, "binlog.000001");
  EXPECT_EQ(catalog.Last()->name, "binlog.000001");

  StoredFileRecord second;
  second.name = "binlog.000002";
  second.inUse = true;
  catalog.Add(second);
  EXPECT_EQ(catalog.First()->name, "binlog.000001");
  ASSERT_TRUE(catalog.Last().has_value());
  EXPECT_EQ(catalog.Last()->name, "binlog.000002");
  EXPECT_TRUE(catalog.Last()->inUse);

  std::string error;
  ASSERT_TRUE(catalog.Remove(error)) << error;
  EXPECT_EQ(catalog.First()->name, "binlog.000002");
  EXPECT_EQ(catalog.Last()->name, "binlog.000002");

  ASSERT_TRUE(catalog.Remove(error)) << error;
  EXPECT_FALSE(catalog.First().has_value());
  EXPECT_FALSE(catalog.Last().has_value());
}

TEST(StorageCatalogTest, CloseFailsWithNoOpenFileToClose) {
  StorageCatalog catalog;
  std::string error;
  EXPECT_FALSE(catalog.Close(/*finalSize=*/0, error));
  EXPECT_FALSE(error.empty());
}

TEST(StorageCatalogTest, RemoveDropsTheOldestRecordAndFailsOnceEmpty) {
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name = "binlog.000001";
  StoredFileRecord second;
  second.name = "binlog.000002";
  catalog.Add(first);
  catalog.Add(second);

  std::string error;
  ASSERT_TRUE(catalog.Remove(error)) << error;
  ASSERT_EQ(catalog.Size(), 1u);
  EXPECT_EQ(catalog.At(0).name, "binlog.000002");

  ASSERT_TRUE(catalog.Remove(error)) << error;
  EXPECT_EQ(catalog.Size(), 0u);
  EXPECT_FALSE(catalog.Remove(error));
}

// Three files with growing Previous_gtids, as in real storage. Takes catalog
// by reference: StorageCatalog isn't copyable/movable (shared_mutex member).
void FillCatalogWithGrowingHistory(StorageCatalog &catalog) {
  std::string error;

  StoredFileRecord first;
  first.name = "binlog.000001";
  EXPECT_TRUE(first.previousGtids.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-3", error))
      << error;
  catalog.Add(first);

  StoredFileRecord second;
  second.name = "binlog.000002";
  EXPECT_TRUE(second.previousGtids.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  catalog.Add(second);

  StoredFileRecord third;
  third.name = "binlog.000003";
  EXPECT_TRUE(third.previousGtids.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-10", error))
      << error;
  catalog.Add(third);
}

TEST(StorageCatalogTest, FindStartFileFindsTheMiddleFileWhenOnlyItQualifies) {
  StorageCatalog catalog;
  FillCatalogWithGrowingHistory(catalog);
  GtidSet replicaSet;
  std::string error;
  // Covers up to group 7: the middle file (1-5) qualifies, the newest (1-10)
  // does not yet.
  ASSERT_TRUE(
      replicaSet.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-7", error))
      << error;
  const auto found = catalog.FindStartFile(replicaSet);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, "binlog.000002");
}

TEST(StorageCatalogTest, FindStartFileFindsTheNewestFileWhenItQualifies) {
  StorageCatalog catalog;
  FillCatalogWithGrowingHistory(catalog);
  GtidSet replicaSet;
  std::string error;
  ASSERT_TRUE(replicaSet.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-20", error))
      << error;
  const auto found = catalog.FindStartFile(replicaSet);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, "binlog.000003");
}

TEST(StorageCatalogTest,
     FindStartFileReturnsNulloptWhenReplicaNeedsOlderHistoryThanAnyFileHas) {
  StorageCatalog catalog;
  FillCatalogWithGrowingHistory(catalog);
  // Even the oldest file's Previous_gtids (1-3) is not covered by an empty set.
  const auto found = catalog.FindStartFile(GtidSet{});
  EXPECT_FALSE(found.has_value());
}

TEST(StorageCatalogTest, FindStartFileReturnsNulloptOnAnEmptyCatalog) {
  StorageCatalog catalog;
  const auto found = catalog.FindStartFile(GtidSet{});
  EXPECT_FALSE(found.has_value());
}

TEST(StorageCatalogTest, FindStartFileFindsTheOldestFileWhenOnlyItQualifies) {
  // Only the oldest file (1-3) is covered by 1-4; a scan that stops short
  // of it would wrongly return nullopt.
  StorageCatalog catalog;
  FillCatalogWithGrowingHistory(catalog);
  GtidSet replicaSet;
  std::string error;
  ASSERT_TRUE(
      replicaSet.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-4", error))
      << error;
  const auto found = catalog.FindStartFile(replicaSet);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, "binlog.000001");
}

TEST(StorageCatalogTest, PinRefusesAFileNotInTheCatalog) {
  StorageCatalog catalog;
  std::string error;
  const auto pin = catalog.Pin("binlog.000001", error);
  EXPECT_FALSE(pin.has_value());
  EXPECT_EQ(error, "binlog.000001 is not in the storage catalog");
}

TEST(StorageCatalogTest, PinBlocksRemovalUntilReleased) {
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);

  std::string pinError;
  auto pin = catalog.Pin("binlog.000001", pinError);
  ASSERT_TRUE(pin.has_value()) << pinError;

  std::string removeError;
  EXPECT_FALSE(catalog.Remove(removeError));
  EXPECT_EQ(removeError, "binlog.000001 is pinned by 1 reader(s)");
  EXPECT_EQ(catalog.Size(), 1u);

  pin.reset();  // destroying the handle releases the pin
  EXPECT_TRUE(catalog.Remove(removeError)) << removeError;
  EXPECT_EQ(catalog.Size(), 0u);
}

TEST(StorageCatalogTest,
     TwoPinsOnTheSameFileBothHaveToBeReleasedBeforeRemoval) {
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);

  std::string error;
  auto firstPin = catalog.Pin("binlog.000001", error);
  ASSERT_TRUE(firstPin.has_value()) << error;
  auto secondPin = catalog.Pin("binlog.000001", error);
  ASSERT_TRUE(secondPin.has_value()) << error;

  EXPECT_FALSE(catalog.Remove(error));
  firstPin.reset();
  EXPECT_FALSE(catalog.Remove(error)) << "still pinned once";
  secondPin.reset();
  EXPECT_TRUE(catalog.Remove(error)) << error;
}

// Pin() and Remove() share a lock, so whichever thread wins decides the
// other's outcome; neither can both succeed nor both fail. Many iterations
// catch a version that checks presence outside that lock.
TEST(StorageCatalogTest, PinAgainstRemoveHasNoThirdOutcome) {
  constexpr int ITERATIONS = 2000;
  for (int i = 0; i < ITERATIONS; ++i) {
    StorageCatalog catalog;
    StoredFileRecord record;
    record.name = "binlog.000001";
    catalog.Add(record);

    std::atomic<bool> pinnerReady{false};
    std::atomic<bool> removerReady{false};
    std::optional<FilePin> pin;
    std::string pinError;
    bool removeResult = false;
    std::string removeError;

    std::thread pinner([&] {
      pinnerReady.store(true, std::memory_order_release);
      while (!removerReady.load(std::memory_order_acquire)) {
      }
      pin = catalog.Pin("binlog.000001", pinError);
    });
    std::thread remover([&] {
      removerReady.store(true, std::memory_order_release);
      while (!pinnerReady.load(std::memory_order_acquire)) {
      }
      removeResult = catalog.Remove(removeError);
    });
    pinner.join();
    remover.join();

    const bool pinSucceeded = pin.has_value();
    EXPECT_NE(pinSucceeded, removeResult) << "iteration " << i;
    EXPECT_EQ(catalog.Size(), pinSucceeded ? 1u : 0u) << "iteration " << i;
  }
}

TEST(
    StorageCatalogTest,
    FindStartFileFindsTheOldestFileForAnEmptyReplicaSetWhenItsPreviousGtidsIsEmptyToo) {
  // An empty GTID set is a subset of anything, so a first file whose own
  // Previous_gtids is itself empty always qualifies for a replica with no
  // GTIDs - a shortcut on replicaSet.IsEmpty() would wrongly refuse this.
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name =
      "binlog.000001";  // previousGtids left default-constructed: empty
  catalog.Add(first);
  StoredFileRecord second;
  second.name = "binlog.000002";
  std::string error;
  ASSERT_TRUE(second.previousGtids.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-3", error))
      << error;
  catalog.Add(second);

  const auto found = catalog.FindStartFile(GtidSet{});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, "binlog.000001");
}

TEST(StorageCatalogTest, TotalSizeBeforeExcludesTheNamedRecord) {
  StorageCatalog catalog;
  for (unsigned n = 1; n <= 3; ++n) {
    StoredFileRecord record;
    record.name = "binlog.00000" + std::to_string(n);
    record.size = n * 10;
    catalog.Add(record);
  }
  EXPECT_EQ(catalog.TotalSizeBefore("binlog.000001"), 0u);
  EXPECT_EQ(catalog.TotalSizeBefore("binlog.000003"), 30u);
  EXPECT_EQ(catalog.TotalSizeBefore("binlog.999999"), 60u);
}

TEST(StorageCatalogTest, BytesBetweenCountsAcrossFilesInOneLook) {
  StorageCatalog catalog;
  for (unsigned n = 1; n <= 3; ++n) {
    StoredFileRecord record;
    record.name = "binlog.00000" + std::to_string(n);
    record.size = n * 10;
    catalog.Add(record);
  }
  EXPECT_EQ(catalog.BytesBetween("binlog.000001", 4, "binlog.000001", 9),
            std::optional<std::uint64_t>(5));
  EXPECT_EQ(catalog.BytesBetween("binlog.000001", 4, "binlog.000003", 7),
            std::optional<std::uint64_t>(33));
  EXPECT_FALSE(
      catalog.BytesBetween("binlog.000003", 4, "binlog.000001", 4).has_value());
  EXPECT_FALSE(
      catalog.BytesBetween("binlog.000002", 9, "binlog.000002", 4).has_value());
  EXPECT_FALSE(
      catalog.BytesBetween("binlog.999999", 4, "binlog.000002", 4).has_value());
  EXPECT_FALSE(
      catalog.BytesBetween("binlog.000002", 4, "binlog.999999", 4).has_value());
  // Purging a file before both points leaves the distance as it was.
  std::string error;
  ASSERT_TRUE(catalog.Remove(error));
  EXPECT_EQ(catalog.BytesBetween("binlog.000002", 4, "binlog.000003", 7),
            std::optional<std::uint64_t>(23));
}

TEST(StorageCatalogTest, TotalSizeSumsEveryRecord) {
  StorageCatalog catalog;
  EXPECT_EQ(catalog.TotalSize(), 0u);
  for (unsigned n = 1; n <= 3; ++n) {
    StoredFileRecord record;
    record.name = "binlog.00000" + std::to_string(n);
    record.size = n * 10;
    catalog.Add(record);
  }
  std::string error;
  EXPECT_EQ(catalog.TotalSize(), 60u);
  ASSERT_TRUE(catalog.Close(50, error));
  EXPECT_EQ(catalog.TotalSize(), 80u);
  ASSERT_TRUE(catalog.Remove(error));
  EXPECT_EQ(catalog.TotalSize(), 70u);
  ASSERT_TRUE(catalog.UpdateSize(5, error));
  EXPECT_EQ(catalog.TotalSize(), 25u);
}

}  // namespace
}  // namespace binlog_streamer
