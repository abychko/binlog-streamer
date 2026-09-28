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

#include "storage/cBinlogTailScanner.hpp"

#include "binlog/eEventType.hpp"
#include "binlog/hEventLimits.hpp"
#include "cTempDirectoryFixture.hpp"
#include "gtid/sGtidSource.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

void AppendLittleEndian(std::vector<std::uint8_t> &out, std::uint64_t value,
                        std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

// Local lenenc encoder, kept self-contained rather than shared.
std::vector<std::uint8_t> Lenenc(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  if (value < 251) {
    out.push_back(static_cast<std::uint8_t>(value));
  } else if (value < 65536) {
    out.push_back(0xFC);
    AppendLittleEndian(out, value, 2);
  } else {
    out.push_back(0xFE);
    AppendLittleEndian(out, value, 8);
  }
  return out;
}

// No header/Previous_gtids prefix: Scan() runs directly against bytes
// built here, not a real file.
void AppendEvent(std::vector<std::uint8_t> &file, std::uint8_t type,
                 std::span<const std::uint8_t> body,
                 std::uint32_t nextPosition) {
  const auto eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + body.size());
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

// lt_type deliberately not LOGICAL_TIMESTAMP_TYPECODE (2), so
// hasTransactionLength comes back false.
std::vector<std::uint8_t> GtidBodyWithoutTransactionLength(std::int64_t gno) {
  std::vector<std::uint8_t> body;
  body.push_back(0x01);
  body.insert(body.end(), 16, std::uint8_t{0});
  AppendLittleEndian(body, static_cast<std::uint64_t>(gno), 8);
  body.push_back(0x00);
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

std::filesystem::path WriteFile(const TempDirectoryFixture &fixture,
                                const std::string &name,
                                const std::vector<std::uint8_t> &bytes) {
  const auto path = fixture.Path(name);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return path;
}

TEST(BinlogTailScannerTest,
     TruncatesExactlyAtTheBoundaryAfterAFullGroupAndHalfOfANextOne) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0, /*gno=*/1);
  const std::uint32_t secondGroupStart =
      static_cast<std::uint32_t>(file.size());
  ASSERT_EQ(secondGroupStart, 125u);
  AppendFullGroup(file, secondGroupStart,
                  /*gno=*/2);  // the whole second group ...
  file.resize(secondGroupStart + 69 +
              10);  // ... then cut to just past its GTID event, mid-Query
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);

  const Uuid zeroUuid{};  // GtidBody() above zeroes the SID field
  const auto intervals =
      result.completedGroups.GetIntervals(GtidSource{zeroUuid, ""});
  ASSERT_EQ(intervals.size(), 1u);
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 2);
}

// Real Gtid_tagged_log_event body (checksum stripped) from Percona Server
// 8.4.11-11: GTID 77d1902c-01da-11f1-9a41-908d6e5f6e8d:test_tag:1,
// transaction_length 337.
const std::vector<std::uint8_t> REAL_TAGGED_GTID_BODY{
    0x02, 0x7c, 0x00, 0x00, 0x02, 0x02, 0xee, 0x45, 0x03, 0x41, 0x02,
    0x58, 0x02, 0x69, 0x03, 0x22, 0xc5, 0x03, 0x69, 0x02, 0x82, 0x41,
    0x02, 0x35, 0x02, 0xdc, 0xbe, 0xdc, 0x35, 0x02, 0x04, 0x04, 0x06,
    0x10, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x74, 0x61, 0x67, 0x08, 0x00,
    0x0a, 0x04, 0x0c, 0x7f, 0x00, 0x3e, 0x89, 0x00, 0xfa, 0x5b, 0x06,
    0x10, 0x45, 0x05, 0x12, 0xdb, 0xd0, 0x09};
constexpr std::uint64_t REAL_TAGGED_TRANSACTION_LENGTH = 337;

TEST(BinlogTailScannerTest, CountsATaggedGroupUnderItsTaggedSource) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  // 19 + 62 (GTID) + 19 + 200 (Query) + 19 + 18 (Xid) = 337, the real event's
  // transaction_length.
  AppendEvent(file, static_cast<std::uint8_t>(EventType::GtidTagged),
              REAL_TAGGED_GTID_BODY, 81);
  AppendEvent(file, 2 /* Query */, std::vector<std::uint8_t>(200, 0xAB),
              81 + 219);
  AppendEvent(file, 16 /* Xid */, std::vector<std::uint8_t>(18, 0xCD), 337);
  AppendFullGroup(file, 337,
                  /*gno=*/7);  // an untagged group after it, its own source
  file.resize(337 + 69 + 10);  // cut mid-Query of that second group
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, REAL_TAGGED_TRANSACTION_LENGTH);
  EXPECT_EQ(result.truncatedBytes,
            file.size() - REAL_TAGGED_TRANSACTION_LENGTH);

  Uuid realUuid;
  const std::array<std::uint8_t, 16> realUuidBytes{
      0x77, 0xd1, 0x90, 0x2c, 0x01, 0xda, 0x11, 0xf1,
      0x9a, 0x41, 0x90, 0x8d, 0x6e, 0x5f, 0x6e, 0x8d};
  realUuid.bytes = realUuidBytes;
  const auto tagged =
      result.completedGroups.GetIntervals(GtidSource{realUuid, "test_tag"});
  ASSERT_EQ(tagged.size(), 1u);
  EXPECT_EQ(tagged.front().start, 1);
  EXPECT_EQ(tagged.front().end, 2);
  EXPECT_TRUE(result.completedGroups.GetIntervals(GtidSource{realUuid, ""})
                  .empty());  // not under the untagged source
  EXPECT_TRUE(result.completedGroups.GetIntervals(GtidSource{Uuid{}, ""})
                  .empty());  // the cut group never completed
}

TEST(BinlogTailScannerTest,
     ReportsNoTruncationAndNoGroupsForAFileWithNothingPastTheStartOffset) {
  TempDirectoryFixture fixture;
  const auto path = WriteFile(fixture, "binlog.000001", {});

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 0u);
  EXPECT_EQ(result.truncatedBytes, 0u);
  EXPECT_TRUE(result.completedGroups.IsEmpty());
}

// Not an error: a declared length shorter than the header itself is just
// another unfinished-write shape, trimmed the same as a short header.
TEST(BinlogTailScannerTest,
     StopsCleanlyAtAnEventDeclaringALengthShorterThanItsOwnHeader) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0,
                  /*gno=*/1);  // one clean boundary to truncate back to
  AppendLittleEndian(file, 1700000000, 4);
  file.push_back(2);
  AppendLittleEndian(file, 1, 4);
  AppendLittleEndian(file, 5,
                     4);  // event_length: shorter than EVENT_HEADER_LENGTH (19)
  AppendLittleEndian(file, 5, 4);
  AppendLittleEndian(file, 0, 2);
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);
}

// A GTID event header is the one shape Scan() allocates a buffer for; a
// complete header with a cut-short body must stop cleanly too.
TEST(BinlogTailScannerTest, StopsCleanlyWhenAGtidEventsOwnBodyIsCutShort) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0,
                  /*gno=*/1);  // one clean boundary to truncate back to
  const auto gtidBody = GtidBody(/*gno=*/2, 125);
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid), gtidBody,
              125 + 69);
  ASSERT_EQ(file.size(), 125u + 19u + gtidBody.size());
  file.resize(file.size() -
              10);  // the GTID event's own header is intact, its body is not
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);
  // Only gno=1 is confirmed; the cut-short gno=2 event never reaches
  // completedGroups.
  const auto intervals =
      result.completedGroups.GetIntervals(GtidSource{Uuid{}, ""});
  ASSERT_EQ(intervals.size(), 1u);
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 2);
}

// The one case Scan() refuses outright rather than trim.
TEST(BinlogTailScannerTest,
     RejectsAGtidEventDeclaringABodyLargerThanTheBufferLimit) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendLittleEndian(file, 1700000000, 4);
  file.push_back(static_cast<std::uint8_t>(EventType::Gtid));
  AppendLittleEndian(file, 1, 4);
  const auto eventLength = static_cast<std::uint32_t>(EVENT_HEADER_LENGTH) +
                           MAX_BUFFERED_EVENT_SIZE + 1;
  AppendLittleEndian(file, eventLength,
                     4);  // declares a body one byte over the limit
  AppendLittleEndian(file, 0, 4);
  AppendLittleEndian(file, 0, 2);
  // No actual body bytes follow; Scan() must reject from the header alone.
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  EXPECT_FALSE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                       /*checksumLength=*/0, result, error));
  EXPECT_FALSE(error.empty());
}

// TransactionBoundaryTracker rejecting an otherwise complete event is not an
// error either - reached only when the event is fully on disk, not cut short.
TEST(BinlogTailScannerTest, StopsCleanlyWhenAnEventCrossesTheEndOfItsGroup) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0,
                  /*gno=*/1);  // one clean boundary to truncate back to
  const std::uint32_t secondGroupStart =
      static_cast<std::uint32_t>(file.size());
  ASSERT_EQ(secondGroupStart, 125u);
  // transactionLength=125 makes the group's end 125+125=250; a 100-byte
  // event (194..294) is entirely on disk but crosses that end.
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid),
              GtidBody(/*gno=*/2, 125), secondGroupStart + 69);
  AppendEvent(file, 2 /* Query */, std::vector<std::uint8_t>(81, 0xAB),
              secondGroupStart + 69 + 100);
  ASSERT_EQ(file.size(), secondGroupStart + 69 + 100);
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);
  const auto intervals =
      result.completedGroups.GetIntervals(GtidSource{Uuid{}, ""});
  ASSERT_EQ(intervals.size(),
            1u);  // only gno=1 - gno=2's own group never closed
  EXPECT_EQ(intervals.front().start, 1);
  EXPECT_EQ(intervals.front().end, 2);
}

// A GTID event arriving while the previous group is still open, both fully on
// disk.
TEST(BinlogTailScannerTest, StopsCleanlyWhenAGtidEventInterruptsAnOpenGroup) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0,
                  /*gno=*/1);  // one clean boundary to truncate back to
  const std::uint32_t secondGroupStart =
      static_cast<std::uint32_t>(file.size());
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid),
              GtidBody(/*gno=*/2, 125), secondGroupStart + 69);
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid),
              GtidBody(/*gno=*/3, 125), secondGroupStart + 138);
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);
}

// A fully-decodable GTID event whose hasTransactionLength is false (a source
// too old for the optional tail); the tracker refuses it as a group opener.
TEST(BinlogTailScannerTest, StopsCleanlyWhenAGtidEventHasNoTransactionLength) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0,
                  /*gno=*/1);  // one clean boundary to truncate back to
  const auto shortBody = GtidBodyWithoutTransactionLength(/*gno=*/2);
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid), shortBody, 0);
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);
}

// A body entirely on disk but too short to decode (by construction) is not
// an error either.
TEST(BinlogTailScannerTest, StopsCleanlyWhenAGtidEventsBodyFailsToDecode) {
  TempDirectoryFixture fixture;
  std::vector<std::uint8_t> file;
  AppendFullGroup(file, 0,
                  /*gno=*/1);  // one clean boundary to truncate back to
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid),
              std::vector<std::uint8_t>(10, 0x00), 0);
  const auto path = WriteFile(fixture, "binlog.000001", file);

  TailScanResult result;
  std::string error;
  ASSERT_TRUE(BinlogTailScanner::Scan(path, /*startOffset=*/0,
                                      /*checksumLength=*/0, result, error))
      << error;
  EXPECT_EQ(result.lastBoundary, 125u);
  EXPECT_EQ(result.truncatedBytes, file.size() - 125u);
}

}  // namespace
}  // namespace binlog_streamer
