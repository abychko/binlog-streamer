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

#include "storage/cStorageRecovery.hpp"

#include "binlog/eEventType.hpp"
#include "cTempDirectoryFixture.hpp"
#include "gtid/cGtidSet.hpp"
#include "gtid/sGtidSource.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

void AppendLittleEndian(std::vector<std::uint8_t> &out, std::uint64_t value,
                        std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::vector<std::uint8_t> Lenenc(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  if (value < 251) {
    out.push_back(static_cast<std::uint8_t>(value));
  } else {
    out.push_back(0xFE);
    AppendLittleEndian(out, value, 8);
  }
  return out;
}

void AppendEvent(std::vector<std::uint8_t> &file, std::uint8_t type,
                 std::span<const std::uint8_t> body,
                 std::uint32_t nextPosition) {
  const auto eventLength = static_cast<std::uint32_t>(19 + body.size());
  AppendLittleEndian(file, 1700000000, 4);
  file.push_back(type);
  AppendLittleEndian(file, 1, 4);
  AppendLittleEndian(file, eventLength, 4);
  AppendLittleEndian(file, nextPosition, 4);
  AppendLittleEndian(file, 0, 2);
  file.insert(file.end(), body.begin(), body.end());
}

std::vector<std::uint8_t> GtidBody(std::int64_t gno,
                                   std::uint64_t transactionLength) {
  std::vector<std::uint8_t> body;
  body.push_back(0x01);
  body.insert(body.end(), 16, std::uint8_t{0});
  AppendLittleEndian(body, static_cast<std::uint64_t>(gno), 8);
  body.push_back(0x02);
  AppendLittleEndian(body, 1, 8);
  AppendLittleEndian(body, 2, 8);
  AppendLittleEndian(body, 0x0001'2345'6789ULL, 7);
  const auto encodedLength = Lenenc(transactionLength);
  body.insert(body.end(), encodedLength.begin(), encodedLength.end());
  return body;
}

// GTID(69) + Query(29) + Xid(27) = 125; transaction_length covers the whole
// group including its own opening GTID event.
void AppendFullGroup(std::vector<std::uint8_t> &file, std::uint32_t start,
                     std::int64_t gno) {
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid),
              GtidBody(gno, 125), start + 69);
  AppendEvent(file, 2 /* Query */, std::vector<std::uint8_t>(10, 0xAB),
              start + 69 + 29);
  AppendEvent(file, 16 /* Xid */, std::vector<std::uint8_t>(8, 0xCD),
              start + 69 + 29 + 27);
}

// A minimal but valid stored file: magic + FDE (server 5.5.62, no checksum
// trailer) + previousGtids encoded as the Previous_gtids_event, plus
// whatever tail bytes the caller appends. Returns the header length.
std::uint64_t WriteFileWithTailAndPreviousGtids(
    const std::filesystem::path &path, bool inUse, const GtidSet &previousGtids,
    const std::vector<std::uint8_t> &tail) {
  std::vector<std::uint8_t> fdeBody(57, 0x00);
  fdeBody[0] = 3;
  const std::string version = "5.5.62";
  for (std::size_t i = 0; i < version.size(); ++i)
    fdeBody[2 + i] = static_cast<std::uint8_t>(version[i]);
  fdeBody[56] = 19;

  const auto pgeBody = previousGtids.Encode(/*skipTaggedGtids=*/false);

  std::vector<std::uint8_t> file{0xfe, 0x62, 0x69, 0x6e};  // BINLOG_MAGIC

  const auto fdeEventLength = static_cast<std::uint32_t>(19 + fdeBody.size());
  std::vector<std::uint8_t> fdeHeader(19, 0x00);
  fdeHeader[4] = 15;
  fdeHeader[9] = static_cast<std::uint8_t>(fdeEventLength);
  fdeHeader[13] = static_cast<std::uint8_t>(4 + fdeEventLength);
  if (inUse) fdeHeader[17] = 0x01;  // LOG_EVENT_BINLOG_IN_USE_F
  file.insert(file.end(), fdeHeader.begin(), fdeHeader.end());
  file.insert(file.end(), fdeBody.begin(), fdeBody.end());

  const auto pgeEventLength = static_cast<std::uint32_t>(19 + pgeBody.size());
  std::vector<std::uint8_t> pgeHeader(19, 0x00);
  pgeHeader[4] = 35;
  pgeHeader[9] = static_cast<std::uint8_t>(pgeEventLength);
  file.insert(file.end(), pgeHeader.begin(), pgeHeader.end());
  file.insert(file.end(), pgeBody.begin(), pgeBody.end());

  const std::uint64_t headerLength = file.size();
  file.insert(file.end(), tail.begin(), tail.end());

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(file.data()),
            static_cast<std::streamsize>(file.size()));
  return headerLength;
}

// The common case among the tests below: an empty Previous_gtids_event.
std::uint64_t WriteFileWithTail(const std::filesystem::path &path, bool inUse,
                                const std::vector<std::uint8_t> &tail) {
  return WriteFileWithTailAndPreviousGtids(path, inUse, GtidSet{}, tail);
}

StoredFileRecord MakeRecord(const std::string &name, std::uint64_t number,
                            std::uint64_t size, std::uint64_t headerLength,
                            bool inUse, GtidSet previousGtids = {}) {
  StoredFileRecord record;
  record.name = name;
  record.basename = "binlog";
  record.number = number;
  record.size = size;
  record.headerLength = headerLength;
  record.checksumAlgorithm = "UNDEF";  // 5.5.62 has no checksum trailer
  record.inUse = inUse;
  record.previousGtids = std::move(previousGtids);
  return record;
}

TEST(StorageRecoveryTest, ReportsEmptyForAnEmptyCatalog) {
  TempDirectoryFixture fixture;
  StorageCatalog catalog;
  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error))
      << error;
  EXPECT_TRUE(state.empty);
}

TEST(StorageRecoveryTest, RefusesAnInUseFileThatIsNotTheLastOne) {
  TempDirectoryFixture fixture;
  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, 200, 116, /*inUse=*/true));
  catalog.Add(MakeRecord("binlog.000002", 2, 200, 116, /*inUse=*/false));

  StorageStartState state;
  std::string error;
  EXPECT_FALSE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error));
  EXPECT_FALSE(error.empty());
}

TEST(StorageRecoveryTest,
     TruncatesAnInUseLastFileAndReportsTheRecoveredStartSet) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> tail;
  AppendFullGroup(tail, 0, /*gno=*/1);
  const std::uint32_t secondGroupStart =
      static_cast<std::uint32_t>(tail.size());
  AppendFullGroup(tail, secondGroupStart, /*gno=*/2);
  tail.resize(secondGroupStart + 69 + 10);  // cut group 2 short, mid-Query

  const auto path = fixture.Path("binlog.000001");
  const std::uint64_t headerLength =
      WriteFileWithTail(path, /*inUse=*/true, tail);
  const std::uint64_t originalSize = headerLength + tail.size();

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, originalSize, headerLength,
                         /*inUse=*/true));

  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error))
      << error;
  EXPECT_FALSE(state.empty);
  EXPECT_EQ(state.lastFileName, "binlog.000001");
  EXPECT_EQ(state.lastFileLength, headerLength + 125);

  const Uuid zeroUuid{};
  const auto intervals = state.startSet.GetIntervals(GtidSource{zeroUuid, ""});
  ASSERT_EQ(intervals.size(), 1u);  // only the first, fully-received group
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 2);

  EXPECT_EQ(std::filesystem::file_size(path),
            headerLength + 125);  // truncated on disk
  ASSERT_EQ(catalog.Size(), 1u);
  EXPECT_EQ(catalog.At(0).size,
            headerLength + 125);  // and the in-RAM record brought back in sync
}

// previousGtids must actually reach state.startSet, not just completedGroups:
// a dropped previousGtids would leave only [5,6) here, a dropped
// completedGroups only [1,5).
TEST(
    StorageRecoveryTest,
    MergesANonEmptyPreviousGtidsWithTheScannedCompletedGroupsForTheSameSource) {
  TempDirectoryFixture fixture;
  GtidSet previousGtids;
  previousGtids.AddInterval(GtidSource{Uuid{}, ""}, 1, 5);  // GNOs 1-4

  std::vector<std::uint8_t> tail;
  AppendFullGroup(tail, 0,
                  /*gno=*/5);  // adjacent to previousGtids' own upper bound

  const auto path = fixture.Path("binlog.000001");
  const std::uint64_t headerLength = WriteFileWithTailAndPreviousGtids(
      path, /*inUse=*/false, previousGtids, tail);
  const std::uint64_t originalSize = headerLength + tail.size();

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, originalSize, headerLength,
                         /*inUse=*/false, previousGtids));

  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error))
      << error;

  const auto intervals = state.startSet.GetIntervals(GtidSource{Uuid{}, ""});
  ASSERT_EQ(intervals.size(), 1u);  // merged into one contiguous range
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 6);
}

// A closed last file always ends exactly on a boundary (its last event is
// the ROTATE that closed it), so a nonzero truncatedBytes there is refused
// rather than silently trimmed, unlike an "in use" file.
TEST(StorageRecoveryTest, RefusesAClosedLastFileWithAnUnrecognizedTail) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> tail;
  AppendFullGroup(tail, 0, /*gno=*/1);
  tail.push_back(0xAB);  // one stray byte past the last complete boundary

  const auto path = fixture.Path("binlog.000001");
  const std::uint64_t headerLength =
      WriteFileWithTail(path, /*inUse=*/false, tail);
  const std::uint64_t originalSize = headerLength + tail.size();

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, originalSize, headerLength,
                         /*inUse=*/false));

  StorageStartState state;
  std::string error;
  EXPECT_FALSE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(std::filesystem::file_size(path),
            originalSize);  // refused before anything was touched
}

// TransactionBoundaryTracker rejecting a fully-present event past a closed
// file's last boundary is the same "unrecognized tail" shape as the
// stray-byte test above, so it ends in a refusal here too.
TEST(StorageRecoveryTest,
     RefusesAClosedLastFileWhereAnEventCrossesItsGroupsEnd) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> tail;
  AppendFullGroup(tail, 0, /*gno=*/1);
  const std::uint32_t secondGroupStart =
      static_cast<std::uint32_t>(tail.size());
  AppendEvent(tail, static_cast<std::uint8_t>(EventType::Gtid),
              GtidBody(/*gno=*/2, 125), secondGroupStart + 69);
  AppendEvent(tail, 2 /* Query */, std::vector<std::uint8_t>(81, 0xAB),
              secondGroupStart + 69 + 100);

  const auto path = fixture.Path("binlog.000001");
  const std::uint64_t headerLength =
      WriteFileWithTail(path, /*inUse=*/false, tail);
  const std::uint64_t originalSize = headerLength + tail.size();

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, originalSize, headerLength,
                         /*inUse=*/false));

  StorageStartState state;
  std::string error;
  EXPECT_FALSE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(std::filesystem::file_size(path),
            originalSize);  // refused before anything was touched
}

TEST(StorageRecoveryTest, ScansAClosedLastFileWithoutTruncatingIt) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> tail;
  AppendFullGroup(tail, 0, /*gno=*/1);
  AppendFullGroup(tail, 125,
                  /*gno=*/2);  // both groups complete - nothing to truncate

  const auto path = fixture.Path("binlog.000001");
  const std::uint64_t headerLength =
      WriteFileWithTail(path, /*inUse=*/false, tail);
  const std::uint64_t originalSize = headerLength + tail.size();

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, originalSize, headerLength,
                         /*inUse=*/false));

  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      StorageRecovery::Recover(fixture.Directory(), catalog, state, error))
      << error;
  EXPECT_EQ(state.lastFileName, "binlog.000001");
  EXPECT_EQ(state.lastFileLength, originalSize);

  const Uuid zeroUuid{};
  const auto intervals = state.startSet.GetIntervals(GtidSource{zeroUuid, ""});
  ASSERT_EQ(intervals.size(), 1u);  // both groups share the same (zeroed) UUID
                                    // and merge into one interval
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 3);

  EXPECT_EQ(std::filesystem::file_size(path),
            originalSize);                      // untouched on disk
  EXPECT_EQ(catalog.At(0).size, originalSize);  // untouched in the catalog
}

// A relay that lost its stream asks the same question Recover() answers at
// start-up, and gets the same boundary - but the tail stays where it is:
// the source sends those bytes again and storage compares them.
TEST(StorageRecoveryTest, ResumePointReportsTheBoundaryAndTruncatesNothing) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> tail;
  AppendFullGroup(tail, 0, /*gno=*/1);
  const std::uint32_t secondGroupStart =
      static_cast<std::uint32_t>(tail.size());
  AppendFullGroup(tail, secondGroupStart, /*gno=*/2);
  tail.resize(secondGroupStart + 69 + 10);  // cut group 2 short, mid-Query

  const auto path = fixture.Path("binlog.000001");
  const std::uint64_t headerLength =
      WriteFileWithTail(path, /*inUse=*/true, tail);
  const std::uint64_t originalSize = headerLength + tail.size();

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, originalSize, headerLength,
                         /*inUse=*/true));

  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      StorageRecovery::ResumePoint(fixture.Directory(), catalog, state, error))
      << error;
  EXPECT_FALSE(state.empty);
  EXPECT_EQ(state.lastFileName, "binlog.000001");
  EXPECT_EQ(state.lastFileLength, headerLength + 125);

  const Uuid zeroUuid{};
  const auto intervals = state.startSet.GetIntervals(GtidSource{zeroUuid, ""});
  ASSERT_EQ(intervals.size(), 1u);  // only the first, fully-received group
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 2);

  EXPECT_EQ(std::filesystem::file_size(path), originalSize);  // left alone
  ASSERT_EQ(catalog.Size(), 1u);
  EXPECT_EQ(catalog.At(0).size, originalSize);
}

// Nothing stored yet: the next dump starts where a first one would.
TEST(StorageRecoveryTest, ResumePointReportsAnEmptyCatalogAsEmpty) {
  TempDirectoryFixture fixture;
  const StorageCatalog catalog;

  StorageStartState state;
  std::string error;
  ASSERT_TRUE(
      StorageRecovery::ResumePoint(fixture.Directory(), catalog, state, error))
      << error;
  EXPECT_TRUE(state.empty);
  EXPECT_TRUE(state.lastFileName.empty());
}

}  // namespace
}  // namespace binlog_streamer
