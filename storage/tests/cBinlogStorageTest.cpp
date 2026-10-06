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

#include "storage/cBinlogStorage.hpp"

#include "cTempDirectoryFixture.hpp"
#include "gtid/cGtidSet.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageReader.hpp"
#include "storage/cStorageRecovery.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

void WriteFreshFile(const std::filesystem::path &path) {
  std::vector<std::uint8_t> fdeBody(57, 0x00);
  fdeBody[0] = 3;
  const std::string version = "5.5.62";
  for (std::size_t i = 0; i < version.size(); ++i)
    fdeBody[2 + i] = static_cast<std::uint8_t>(version[i]);
  fdeBody[56] = 19;

  const auto pgeBody = GtidSet().Encode(/*skipTaggedGtids=*/false);

  std::vector<std::uint8_t> file{0xfe, 0x62, 0x69, 0x6e};

  const std::uint32_t fdeEventLength =
      static_cast<std::uint32_t>(19 + fdeBody.size());
  std::vector<std::uint8_t> fdeHeader(19, 0x00);
  fdeHeader[4] = 15;
  fdeHeader[9] = static_cast<std::uint8_t>(fdeEventLength);
  fdeHeader[13] = static_cast<std::uint8_t>(4 + fdeEventLength);
  fdeHeader[17] = 0x01;  // LOG_EVENT_BINLOG_IN_USE_F
  file.insert(file.end(), fdeHeader.begin(), fdeHeader.end());
  file.insert(file.end(), fdeBody.begin(), fdeBody.end());

  const std::uint32_t pgeEventLength =
      static_cast<std::uint32_t>(19 + pgeBody.size());
  std::vector<std::uint8_t> pgeHeader(19, 0x00);
  pgeHeader[4] = 35;
  pgeHeader[9] = static_cast<std::uint8_t>(pgeEventLength);
  file.insert(file.end(), pgeHeader.begin(), pgeHeader.end());
  file.insert(file.end(), pgeBody.begin(), pgeBody.end());

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(file.data()),
            static_cast<std::streamsize>(file.size()));
}

std::uint64_t WriteFreshFileWithGarbageTail(const std::filesystem::path &path) {
  WriteFreshFile(path);
  std::error_code sizeError;
  const std::uint64_t headerLength =
      std::filesystem::file_size(path, sizeError);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  const std::vector<std::uint8_t> garbage(10, 0xAB);
  out.write(reinterpret_cast<const char *>(garbage.data()),
            static_cast<std::streamsize>(garbage.size()));
  return headerLength;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

TEST(BinlogStorageTest, OpensAFreshDataDirectory) {
  TempDirectoryFixture fixture;
  BinlogStorage storage;
  StorageOpenFailure failure = StorageOpenFailure::AccessProblem;
  std::string error;
  ASSERT_TRUE(storage.Open(fixture.Directory(), failure, error)) << error;
  EXPECT_EQ(storage.Catalog().Size(), 0u);
  EXPECT_TRUE(std::filesystem::exists(fixture.Path("binlog.index")));
}

TEST(BinlogStorageTest, RefusesASecondInstanceOnTheSameDataDirectory) {
  TempDirectoryFixture fixture;
  BinlogStorage first;
  StorageOpenFailure firstFailure = StorageOpenFailure::AccessProblem;
  std::string firstError;
  ASSERT_TRUE(first.Open(fixture.Directory(), firstFailure, firstError))
      << firstError;

  BinlogStorage second;
  StorageOpenFailure secondFailure = StorageOpenFailure::AccessProblem;
  std::string secondError;
  EXPECT_FALSE(second.Open(fixture.Directory(), secondFailure, secondError));
  EXPECT_FALSE(secondError.empty());
  EXPECT_EQ(secondFailure, StorageOpenFailure::StorageProblem);
}

TEST(BinlogStorageTest,
     RefusesASecondInstanceEvenAfterTheIndexFileHasBeenRewritten) {
  TempDirectoryFixture fixture;
  BinlogStorage first;
  StorageOpenFailure firstFailure = StorageOpenFailure::AccessProblem;
  std::string firstError;
  ASSERT_TRUE(first.Open(fixture.Directory(), firstFailure, firstError))
      << firstError;

  WriteFreshFile(fixture.Path("binlog.000001"));
  std::string appendError;
  ASSERT_TRUE(BinlogIndexFile::Append(fixture.Path("binlog.index").string(),
                                      "binlog.000001", appendError))
      << appendError;

  BinlogStorage second;
  StorageOpenFailure secondFailure = StorageOpenFailure::AccessProblem;
  std::string secondError;
  EXPECT_FALSE(second.Open(fixture.Directory(), secondFailure, secondError));
  EXPECT_EQ(secondFailure, StorageOpenFailure::StorageProblem);
  EXPECT_EQ(second.Catalog().Size(), 0u);
}

TEST(BinlogStorageTest, RefusesAReadOnlyDataDirectoryEvenWithAnExistingIndex) {
  if (geteuid() == 0) GTEST_SKIP() << "chmod does not restrict the root user";

  TempDirectoryFixture fixture;
  {
    BinlogStorage first;
    StorageOpenFailure firstFailure = StorageOpenFailure::AccessProblem;
    std::string firstError;
    ASSERT_TRUE(first.Open(fixture.Directory(), firstFailure, firstError))
        << firstError;
  }

  ASSERT_EQ(chmod(fixture.Directory().c_str(), 0500), 0)
      << std::strerror(errno);

  BinlogStorage second;
  StorageOpenFailure secondFailure = StorageOpenFailure::StorageProblem;
  std::string secondError;
  const bool opened =
      second.Open(fixture.Directory(), secondFailure, secondError);

  chmod(fixture.Directory().c_str(), 0700);

  EXPECT_FALSE(opened);
  EXPECT_EQ(secondFailure, StorageOpenFailure::AccessProblem);
  EXPECT_NE(secondError.find(fixture.Directory().string()), std::string::npos)
      << secondError;
}

TEST(BinlogStorageTest, ALaterInstanceCanOpenOnceTheEarlierOneIsGone) {
  TempDirectoryFixture fixture;
  {
    BinlogStorage first;
    StorageOpenFailure firstFailure = StorageOpenFailure::AccessProblem;
    std::string firstError;
    ASSERT_TRUE(first.Open(fixture.Directory(), firstFailure, firstError))
        << firstError;
  }

  BinlogStorage second;
  StorageOpenFailure secondFailure = StorageOpenFailure::AccessProblem;
  std::string secondError;
  EXPECT_TRUE(second.Open(fixture.Directory(), secondFailure, secondError))
      << secondError;
}

TEST(BinlogStorageTest, SeedsThePublishedPositionFromRecoveredHistory) {
  TempDirectoryFixture fixture;
  const std::uint64_t headerLength =
      WriteFreshFileWithGarbageTail(fixture.Path("binlog.000001"));
  std::string appendError;
  ASSERT_TRUE(BinlogIndexFile::Append(fixture.Path("binlog.index").string(),
                                      "binlog.000001", appendError))
      << appendError;

  BinlogStorage storage;
  StorageOpenFailure openFailure = StorageOpenFailure::AccessProblem;
  std::string openError;
  ASSERT_TRUE(storage.Open(fixture.Directory(), openFailure, openError))
      << openError;

  StorageStartState state;
  std::string recoverError;
  ASSERT_TRUE(StorageRecovery::Recover(fixture.Directory(), storage.Catalog(),
                                       state, recoverError))
      << recoverError;
  ASSERT_FALSE(state.empty);
  ASSERT_EQ(state.lastFileName, "binlog.000001");
  ASSERT_EQ(state.lastFileLength, headerLength);

  storage.SeedPublished(state);

  const PublishedPosition current = storage.Published().Current();
  EXPECT_EQ(current.fileName, "binlog.000001");
  EXPECT_EQ(current.position, headerLength);
}

TEST(BinlogStorageTest,
     ALiveReaderSeesOnlyTheSeededHistoryImmediatelyAfterOpenResumed) {
  TempDirectoryFixture fixture;
  const std::uint64_t headerLength =
      WriteFreshFileWithGarbageTail(fixture.Path("binlog.000001"));
  std::string appendError;
  ASSERT_TRUE(BinlogIndexFile::Append(fixture.Path("binlog.index").string(),
                                      "binlog.000001", appendError))
      << appendError;

  BinlogStorage storage;
  StorageOpenFailure openFailure = StorageOpenFailure::AccessProblem;
  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      storage.OpenResumed(fixture.Directory(), state, openFailure, error))
      << error;
  ASSERT_FALSE(state.empty);
  ASSERT_EQ(state.lastFileLength, headerLength);

  StorageReader reader(fixture.Directory(), storage.Catalog(),
                       storage.Published());
  std::string openError;
  const auto cursor = reader.Open("binlog.000001", openError);
  ASSERT_TRUE(cursor) << openError;

  {
    std::ofstream extra(fixture.Path("binlog.000001"),
                        std::ios::binary | std::ios::app);
    extra << "extra";
  }

  std::vector<std::uint8_t> buffer(headerLength);
  std::string readError;
  const std::size_t bytesRead = reader.Read(*cursor, 0, buffer, readError);
  EXPECT_EQ(bytesRead, headerLength) << readError;
  const auto onDisk = ReadFile(fixture.Path("binlog.000001"));
  ASSERT_GE(onDisk.size(), headerLength);
  EXPECT_TRUE(std::equal(buffer.begin(), buffer.end(), onDisk.begin()));

  EXPECT_EQ(reader.Read(*cursor, headerLength, buffer, readError), 0u);
  EXPECT_TRUE(readError.empty()) << readError;
}

}  // namespace
}  // namespace binlog_streamer
