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
#include "storage/cBinlogFileWriter.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <fstream>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> SampleFde() {
  std::vector<std::uint8_t> fde(24, 0);
  for (std::size_t i = 0; i < fde.size(); ++i)
    fde[i] = static_cast<std::uint8_t>(0x10 + i);
  fde[17] = 0xA0;
  fde[18] = 0x00;
  return fde;
}

std::vector<std::uint8_t> SamplePreviousGtids() {
  return {0xDE, 0xAD, 0xBE, 0xEF};
}

TEST(BinlogFileWriterTest, RoundTripsWholeFileAcrossCreateAppendAndReopen) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  const auto fde = SampleFde();
  const auto previousGtids = SamplePreviousGtids();
  const std::vector<std::uint8_t> firstPayload = {'h', 'e', 'l', 'l', 'o'};

  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Create(path.string(), fde, previousGtids, error)) << error;
  ASSERT_TRUE(writer.Append(firstPayload, error)) << error;
  ASSERT_TRUE(writer.Flush(error)) << error;
  ASSERT_TRUE(writer.Sync(error)) << error;
  EXPECT_EQ(writer.Size(),
            4 + fde.size() + previousGtids.size() + firstPayload.size());

  const std::vector<std::uint8_t> secondPayload = {'w', 'o', 'r', 'l', 'd'};
  BinlogFileWriter reopened;
  ASSERT_TRUE(reopened.OpenExisting(path.string(), error)) << error;
  EXPECT_EQ(reopened.Size(), writer.Size());
  ASSERT_TRUE(reopened.Append(secondPayload, error)) << error;
  ASSERT_TRUE(reopened.Flush(error)) << error;
  ASSERT_TRUE(reopened.Sync(error)) << error;

  std::vector<std::uint8_t> expected = {0xfe, 0x62, 0x69, 0x6e};
  expected.insert(expected.end(), fde.begin(), fde.end());
  expected[4 + 17] = 0xA1;
  expected.insert(expected.end(), previousGtids.begin(), previousGtids.end());
  expected.insert(expected.end(), firstPayload.begin(), firstPayload.end());
  expected.insert(expected.end(), secondPayload.begin(), secondPayload.end());

  EXPECT_EQ(ReadFile(path), expected);
  EXPECT_EQ(reopened.Size(), expected.size());
}

TEST(BinlogFileWriterTest, ChunkLargerThanBufferBypassesTheBuffer) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(
      writer.Create(path.string(), SampleFde(), SamplePreviousGtids(), error))
      << error;

  // Strictly larger than the buffer, not equal to it: exactly
  // WRITE_BUFFER_SIZE would also auto-flush, making the direct-write path
  // indistinguishable from the buffered one at that boundary.
  std::vector<std::uint8_t> largeChunk(WRITE_BUFFER_SIZE + 1000);
  for (std::size_t i = 0; i < largeChunk.size(); ++i)
    largeChunk[i] = static_cast<std::uint8_t>(i);
  ASSERT_TRUE(writer.Append(largeChunk, error)) << error;

  const std::size_t headerLength =
      4 + SampleFde().size() + SamplePreviousGtids().size();
  const auto onDisk = ReadFile(path);
  ASSERT_EQ(onDisk.size(), headerLength + largeChunk.size());
  const std::vector<std::uint8_t> tail(
      onDisk.begin() + static_cast<std::ptrdiff_t>(headerLength), onDisk.end());
  EXPECT_EQ(tail, largeChunk);

  ASSERT_TRUE(writer.Sync(error)) << error;
}

TEST(BinlogFileWriterTest, InterleavedSmallAndLargeChunksPreserveOrder) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(
      writer.Create(path.string(), SampleFde(), SamplePreviousGtids(), error))
      << error;

  // small (buffered) -> large (direct) -> small (buffered): the buffer
  // must flush before the direct write or "large" lands out of order.
  const std::vector<std::uint8_t> firstSmall = {1, 2, 3, 4, 5};
  std::vector<std::uint8_t> large(WRITE_BUFFER_SIZE + 1000);
  for (std::size_t i = 0; i < large.size(); ++i)
    large[i] = static_cast<std::uint8_t>(0x80 + (i % 100));
  const std::vector<std::uint8_t> secondSmall = {9, 8, 7};

  ASSERT_TRUE(writer.Append(firstSmall, error)) << error;
  ASSERT_TRUE(writer.Append(large, error)) << error;
  ASSERT_TRUE(writer.Append(secondSmall, error)) << error;
  ASSERT_TRUE(writer.Flush(error)) << error;
  ASSERT_TRUE(writer.Sync(error)) << error;

  const auto onDisk = ReadFile(path);
  const std::size_t headerLength =
      4 + SampleFde().size() + SamplePreviousGtids().size();
  std::vector<std::uint8_t> expectedBody = firstSmall;
  expectedBody.insert(expectedBody.end(), large.begin(), large.end());
  expectedBody.insert(expectedBody.end(), secondSmall.begin(),
                      secondSmall.end());
  const std::vector<std::uint8_t> actualBody(
      onDisk.begin() + static_cast<std::ptrdiff_t>(headerLength), onDisk.end());
  EXPECT_EQ(actualBody, expectedBody);
}

TEST(BinlogFileWriterTest, MarkClosedChangesExactlyOneByte) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(
      writer.Create(path.string(), SampleFde(), SamplePreviousGtids(), error))
      << error;
  ASSERT_TRUE(writer.Append(std::vector<std::uint8_t>{1, 2, 3}, error))
      << error;
  ASSERT_TRUE(writer.Flush(error)) << error;
  ASSERT_TRUE(writer.Sync(error)) << error;

  const auto before = ReadFile(path);
  ASSERT_TRUE(writer.MarkClosed(error)) << error;
  const auto after = ReadFile(path);

  ASSERT_EQ(before.size(), after.size());
  std::size_t differences = 0;
  std::size_t differingIndex = 0;
  for (std::size_t i = 0; i < before.size(); ++i) {
    if (before[i] != after[i]) {
      ++differences;
      differingIndex = i;
    }
  }
  EXPECT_EQ(differences, 1u);
  EXPECT_EQ(differingIndex, IN_USE_FLAG_OFFSET);
  EXPECT_EQ(before[IN_USE_FLAG_OFFSET], 0xA1);
  EXPECT_EQ(after[IN_USE_FLAG_OFFSET], 0xA0);
}

TEST(BinlogFileWriterTest,
     TruncateShortensTheFileAndSubsequentAppendDoesNotLeaveAGap) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(
      writer.Create(path.string(), SampleFde(), SamplePreviousGtids(), error))
      << error;
  ASSERT_TRUE(
      writer.Append(std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}, error))
      << error;
  ASSERT_TRUE(writer.Flush(error)) << error;
  const std::uint64_t fullLength = writer.Size();

  const std::uint64_t shortLength = fullLength - 4;
  ASSERT_TRUE(writer.Truncate(shortLength, error)) << error;
  EXPECT_EQ(writer.Size(), shortLength);
  EXPECT_EQ(ReadFile(path).size(), shortLength);

  const std::vector<std::uint8_t> appended = {0xAA, 0xBB};
  ASSERT_TRUE(writer.Append(appended, error)) << error;
  ASSERT_TRUE(writer.Flush(error)) << error;

  const auto onDisk = ReadFile(path);
  ASSERT_EQ(onDisk.size(), shortLength + appended.size());
  const std::vector<std::uint8_t> tail(
      onDisk.end() - static_cast<std::ptrdiff_t>(appended.size()),
      onDisk.end());
  EXPECT_EQ(tail, appended);
}

TEST(BinlogFileWriterTest, TruncatePastTheCurrentLengthFails) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(
      writer.Create(path.string(), SampleFde(), SamplePreviousGtids(), error))
      << error;
  ASSERT_TRUE(writer.Append(std::vector<std::uint8_t>{1, 2, 3}, error))
      << error;
  ASSERT_TRUE(writer.Flush(error)) << error;
  const std::uint64_t currentLength = writer.Size();

  // One byte past what has actually been written: ftruncate(2) would
  // otherwise zero-fill the gap instead of reporting an error.
  EXPECT_FALSE(writer.Truncate(currentLength + 1, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(writer.Size(), currentLength);
  EXPECT_EQ(ReadFile(path).size(), currentLength);
}

TEST(BinlogFileWriterTest, TruncateToZeroEmptiesTheFile) {
  TempDirectoryFixture fixture;
  const auto path = fixture.Path("binlog.000001");
  BinlogFileWriter writer;
  std::string error;
  ASSERT_TRUE(
      writer.Create(path.string(), SampleFde(), SamplePreviousGtids(), error))
      << error;
  ASSERT_TRUE(writer.Append(std::vector<std::uint8_t>{1, 2, 3}, error))
      << error;
  ASSERT_TRUE(writer.Flush(error)) << error;

  ASSERT_TRUE(writer.Truncate(0, error)) << error;
  EXPECT_EQ(writer.Size(), 0u);
  EXPECT_TRUE(ReadFile(path).empty());

  const std::vector<std::uint8_t> appended = {0xCC};
  ASSERT_TRUE(writer.Append(appended, error)) << error;
  ASSERT_TRUE(writer.Flush(error)) << error;
  EXPECT_EQ(ReadFile(path), appended);
}

}  // namespace
}  // namespace binlog_streamer
