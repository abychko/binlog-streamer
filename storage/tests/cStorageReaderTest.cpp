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

#include "cStorageSinkFixture.hpp"
#include "storage/cStorageReader.hpp"

#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "cTempDirectoryFixture.hpp"
#include "gtid/cGtidSet.hpp"
#include "receiver/iEventSink.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageEventSink.hpp"
#include "storage/sStoredFileRecord.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

void WriteFile(const std::filesystem::path &path, std::string_view content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

TEST(StorageReaderTest, OpenPinsTheFileAndRefusesRemovalWhileTheCursorIsAlive) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "abc");
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;
  EXPECT_EQ(cursor->FileName(), "binlog.000001");

  std::string removeError;
  EXPECT_FALSE(catalog.Remove(removeError));
  cursor.reset();
  EXPECT_TRUE(catalog.Remove(removeError)) << removeError;
}

// The exact wording matches StorageCatalog::Pin()'s own message; Open()
// does not wrap or replace it.
TEST(StorageReaderTest, OpenRefusesAFileNotInTheCatalog) {
  TempDirectoryFixture fixture;
  StorageCatalog catalog;
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  const auto cursor = reader.Open("binlog.000001", error);
  EXPECT_FALSE(cursor);
  EXPECT_EQ(error, "binlog.000001 is not in the storage catalog");
}

// Distinct from "not in the catalog at all": Pin() succeeds here, and it is
// Open()'s own open(2) call that must fail and release the pin it just took.
TEST(StorageReaderTest, OpenRefusesAFileListedInTheCatalogButMissingFromDisk) {
  TempDirectoryFixture fixture;
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";  // never written under fixture.Directory()
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  const auto cursor = reader.Open("binlog.000001", error);
  EXPECT_FALSE(cursor);
  EXPECT_NE(error.find(fixture.Path("binlog.000001").string()),
            std::string::npos)
      << error;

  // The pin Open() took before open(2) failed must not be left behind.
  std::string removeError;
  EXPECT_TRUE(catalog.Remove(removeError)) << removeError;
}

// Kills the mutant where Read() is bounded by on-disk size or the request
// instead of the published position: more bytes are on disk than published.
TEST(StorageReaderTest,
     ReadIsLimitedByThePublishedPositionNotByHowManyBytesAreOnDiskOrRequested) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"),
            "0123456789");  // 10 bytes actually on disk
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);
  PublishedPositionTracker published;
  published.Advance("binlog.000001",
                    6);  // only the first 6 bytes are published
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::vector<std::uint8_t> buffer(
      20, 0xAA);  // room for far more than either 10 (disk) or 6 (published)
  const std::size_t bytesRead = reader.Read(*cursor, 0, buffer, error);
  ASSERT_EQ(bytesRead, 6u) << error;
  EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + 6), "012345");
  EXPECT_EQ(buffer[6], 0xAA);  // untouched - a mutant reading past the boundary
                               // would overwrite this
}

TEST(StorageReaderTest,
     ReadReturnsZeroBytesExactlyAtThePublishedBoundaryWithoutAnError) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "0123456789");
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);
  PublishedPositionTracker published;
  published.Advance("binlog.000001", 6);
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::vector<std::uint8_t> buffer(20);
  EXPECT_EQ(reader.Read(*cursor, 6, buffer, error), 0u);
  EXPECT_TRUE(error.empty()) << error;
}

TEST(StorageReaderTest, ReadRefusesAnOffsetPastThePublishedBoundary) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "0123456789");
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);
  PublishedPositionTracker published;
  published.Advance("binlog.000001", 6);
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::vector<std::uint8_t> buffer(20);
  EXPECT_EQ(reader.Read(*cursor, 7, buffer, error), 0u);
  EXPECT_EQ(
      error,
      "binlog.000001: read offset 7 is past what may be read right now (6)");
}

// The other Boundary() case: published has moved past a closed file
// entirely, so its final catalog size is the boundary.
TEST(
    StorageReaderTest,
    ReadUsesTheCatalogLengthOnceAFileIsClosedAndPublishedHasMovedToADifferentFile) {
  TempDirectoryFixture fixture;
  WriteFile(
      fixture.Path("binlog.000001"),
      "0123456789");  // 10 bytes - this file's own final, immutable length
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  record.size = 10;
  catalog.Add(record);
  PublishedPositionTracker published;
  published.Advance("binlog.000002",
                    0);  // published has moved on to a later file entirely
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::vector<std::uint8_t> buffer(20, 0xAA);
  const std::size_t bytesRead = reader.Read(*cursor, 0, buffer, error);
  ASSERT_EQ(bytesRead, 10u) << error;
  EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + 10), "0123456789");

  EXPECT_EQ(reader.Read(*cursor, 10, buffer, error),
            0u);  // exactly at the end of file - not an error
  EXPECT_TRUE(error.empty()) << error;
}

TEST(StorageReaderTest,
     NextReportsNotYetAvailableWhenNothingFollowsCurrentYet) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "x");
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  record.size =
      1;  // closed (inUse defaults to false) at its own true, single-byte size
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::unique_ptr<FileCursor> next;
  EXPECT_EQ(reader.Next(*cursor, 1, next, error),
            NextFileOutcome::NotYetAvailable);
  EXPECT_FALSE(next);
}

// Kills the mutant where Next() returns Found without checking that
// current is actually done.
TEST(StorageReaderTest, NextWithholdsFoundUntilCurrentIsClosedAndFullyRead) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"),
            "0123456789");  // 10 bytes, its own true size once closed
  WriteFile(fixture.Path("binlog.000002"), "y");
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name = "binlog.000001";
  first.inUse = true;  // still open
  first.size = 6;      // matches the offset below - still not enough on its own
                       // while inUse is true
  catalog.Add(first);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::unique_ptr<FileCursor> next;
  EXPECT_EQ(reader.Next(*cursor, 6, next, error),
            NextFileOutcome::NotYetAvailable);
  EXPECT_FALSE(next);

  // A genuine close followed by a second file's header landing in the
  // catalog - the shape a real relay leaves behind.
  ASSERT_TRUE(catalog.Close(/*finalSize=*/10, error)) << error;
  StoredFileRecord second;
  second.name = "binlog.000002";
  catalog.Add(second);

  // Closed now, but the read offset (6) has not caught up to the final size
  // (10) yet.
  EXPECT_EQ(reader.Next(*cursor, 6, next, error),
            NextFileOutcome::NotYetAvailable);
  EXPECT_FALSE(next);

  // Catches the offset up; Boundary() now takes the on-disk length (10),
  // not the published position.
  std::vector<std::uint8_t> buffer(10);
  const std::size_t bytesRead = reader.Read(*cursor, 6, buffer, error);
  ASSERT_EQ(bytesRead, 4u) << error;

  ASSERT_EQ(reader.Next(*cursor, 10, next, error), NextFileOutcome::Found)
      << error;
  ASSERT_TRUE(next);
  EXPECT_EQ(next->FileName(), "binlog.000002");
}

// Kills the mutant where Next() does not pin the file it moves to, proven
// through Remove()'s own refusal.
TEST(StorageReaderTest, NextMovesToTheFollowingFileOnceItExistsAndPinsIt) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "x");
  WriteFile(fixture.Path("binlog.000002"), "y");
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name = "binlog.000001";
  first.size = 1;  // closed at its own true, single-byte size
  catalog.Add(first);
  StoredFileRecord second;
  second.name = "binlog.000002";
  catalog.Add(second);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::unique_ptr<FileCursor> next;
  ASSERT_EQ(reader.Next(*cursor, 1, next, error), NextFileOutcome::Found)
      << error;
  ASSERT_TRUE(next);
  EXPECT_EQ(next->FileName(), "binlog.000002");

  cursor.reset();  // binlog.000001 is unpinned now
  EXPECT_TRUE(catalog.Remove(error)) << error;
  EXPECT_FALSE(catalog.Remove(error));  // binlog.000002 is still pinned by next
  EXPECT_EQ(error, "binlog.000002 is pinned by 1 reader(s)");
}

// Kills the mutant where an offset past a closed file's final size is
// folded into NotDone (told to wait) instead of refused outright.
TEST(StorageReaderTest, NextFailsWhenOffsetIsPastTheClosedFilesFinalSize) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"),
            "0123456789");  // 10 bytes, its own true size once closed
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  record.size = 10;  // closed (inUse defaults to false) at its own true size
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;

  std::unique_ptr<FileCursor> next;
  EXPECT_EQ(reader.Next(*cursor, 11, next, error), NextFileOutcome::Failed);
  EXPECT_FALSE(next);
  EXPECT_EQ(error, "binlog.000001: offset 11 is past its own final size (10)");
}

TEST(StorageReaderTest,
     WaitForNewEventsDelegatesToThePublishedPositionTracker) {
  StorageCatalog catalog;
  PublishedPositionTracker published;
  published.Advance("binlog.000001", 300);
  StorageReader reader(std::filesystem::path{}, catalog, published);

  const auto outcome =
      reader.WaitForNewEvents(PublishedPosition{"binlog.000001", 241},
                              std::chrono::milliseconds(0), WaitStyle::Block);
  EXPECT_EQ(outcome, WaitOutcome::Advanced);
}

TEST(StorageReaderTest, PublishedReturnsTheTrackersCurrentPosition) {
  StorageCatalog catalog;
  PublishedPositionTracker published;
  published.Advance("binlog.000001", 116);
  StorageReader reader(std::filesystem::path{}, catalog, published);

  const PublishedPosition current = reader.Published();
  EXPECT_EQ(current.fileName, "binlog.000001");
  EXPECT_EQ(current.position, 116u);
}

TEST(StorageReaderTest, FindStartFileDelegatesToTheCatalog) {
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";  // previousGtids left default-constructed:
                                  // empty, a subset of anything
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(std::filesystem::path{}, catalog, published);

  const auto found = reader.FindStartFile(GtidSet{});
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, "binlog.000001");
}

// --- writer-and-reader-in-different-threads scenario ---

class NullSink : public EventSink {
 public:
  bool OnEventBegin(const EventHeader &, const StreamPosition &) override {
    return true;
  }
  bool OnEventBytes(std::span<const std::uint8_t>) override { return true; }
  bool OnEventEnd() override { return true; }
};

void AppendLittleEndian(std::vector<std::uint8_t> &out, std::uint64_t value,
                        std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

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

struct WireEvent {
  EventHeader header;
  std::vector<std::uint8_t> bytes;
};

WireEvent MakeEvent(std::uint8_t type, std::span<const std::uint8_t> body,
                    std::uint32_t nextPosition, std::uint16_t flags = 0) {
  WireEvent event;
  event.header.timestamp = flags == EVENT_FLAG_ARTIFICIAL ? 0 : 1700000000;
  event.header.type = type;
  event.header.serverId = 1;
  event.header.eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + body.size());
  event.header.nextPosition = nextPosition;
  event.header.flags = flags;

  event.bytes.reserve(event.header.eventLength);
  AppendLittleEndian(event.bytes, event.header.timestamp, 4);
  event.bytes.push_back(event.header.type);
  AppendLittleEndian(event.bytes, event.header.serverId, 4);
  AppendLittleEndian(event.bytes, event.header.eventLength, 4);
  AppendLittleEndian(event.bytes, event.header.nextPosition, 4);
  AppendLittleEndian(event.bytes, event.header.flags, 2);
  event.bytes.insert(event.bytes.end(), body.begin(), body.end());
  return event;
}

bool Drive(EventSink &sink, const WireEvent &event,
           const StreamPosition &position) {
  if (!sink.OnEventBegin(event.header, position)) return false;
  if (!sink.OnEventBytes(event.bytes)) return false;
  return sink.OnEventEnd();
}

std::vector<std::uint8_t> RotateBody(std::uint64_t position,
                                     const std::string &fileName) {
  std::vector<std::uint8_t> body;
  AppendLittleEndian(body, position, 8);
  body.insert(body.end(), fileName.begin(), fileName.end());
  return body;
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

std::vector<std::uint8_t> SampleFde() {
  std::vector<std::uint8_t> body(57, 0x00);
  body[0] = 4;
  const std::string version = "8.4.11";
  for (std::size_t i = 0; i < version.size(); ++i)
    body[2 + i] = static_cast<std::uint8_t>(version[i]);
  body[56] = 19;
  body.push_back(0x01);  // BINLOG_CHECKSUM_ALG_CRC32
  body.insert(body.end(), 4, 0x00);
  return body;
}

std::vector<std::uint8_t> SamplePreviousGtids() {
  std::vector<std::uint8_t> body = GtidSet().Encode(/*skipTaggedGtids=*/false);
  body.insert(body.end(), 4, 0x00);
  return body;
}

// Same formula OpenFreshFile() uses internally (magic + FDE + PGE), not a
// separately hand-counted literal.
std::uint32_t FreshFileHeaderLength() {
  return 4 +
         static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + SampleFde().size()) +
         static_cast<std::uint32_t>(EVENT_HEADER_LENGTH +
                                    SamplePreviousGtids().size());
}

struct FreshFileHeader {
  std::uint32_t afterPge = 0;
  // Whole wire event bytes, so callers can reconstruct on-disk bytes
  // without re-deriving them; Create() forces the FDE's own "in use" bit to 1.
  std::vector<std::uint8_t> fdeEventBytes;
  std::vector<std::uint8_t> pgeEventBytes;
};

FreshFileHeader OpenFreshFile(EventSink &sink, const std::string &fileName) {
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, fileName), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_TRUE(Drive(sink, rotate, StreamPosition{fileName, 0}));

  const auto fde = SampleFde();
  const std::uint32_t afterFde =
      4 + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + fde.size());
  const auto fdeEvent = MakeEvent(
      static_cast<std::uint8_t>(EventType::FormatDescription), fde, afterFde);
  EXPECT_TRUE(Drive(sink, fdeEvent, StreamPosition{fileName, 4}));

  const auto pge = SamplePreviousGtids();
  const std::uint32_t afterPge =
      afterFde + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + pge.size());
  const auto pgeEvent = MakeEvent(
      static_cast<std::uint8_t>(EventType::PreviousGtids), pge, afterPge);
  EXPECT_TRUE(Drive(sink, pgeEvent, StreamPosition{fileName, afterFde}));

  return FreshFileHeader{afterPge, fdeEvent.bytes, pgeEvent.bytes};
}

// GTID(69) + Query(29) + Xid(27) = 125; transaction_length covers the whole
// group including its own opening GTID event.
constexpr std::uint64_t GROUP_TRANSACTION_LENGTH = 125;

struct DrivenGroup {
  WireEvent gtid;
  WireEvent query;
  WireEvent xid;
};

DrivenGroup MakeGroup(std::uint64_t start, std::int64_t gno) {
  DrivenGroup group;
  group.gtid = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                         GtidBody(gno, GROUP_TRANSACTION_LENGTH),
                         static_cast<std::uint32_t>(start + 69));
  group.query =
      MakeEvent(2 /* a Query event */, std::vector<std::uint8_t>(10, 0xAB),
                static_cast<std::uint32_t>(start + 69 + 29));
  group.xid = MakeEvent(16 /* Xid event */, std::vector<std::uint8_t>(8, 0xCD),
                        static_cast<std::uint32_t>(start + 69 + 29 + 27));
  return group;
}

// Positions computed from each event's byte length, not a fixed 69/29/27
// shape, so this driver also works for large groups.
bool DriveGroup(EventSink &sink, const DrivenGroup &group,
                std::uint64_t start) {
  if (!Drive(sink, group.gtid, StreamPosition{"unused", start})) return false;
  const std::uint64_t afterGtid = start + group.gtid.bytes.size();
  if (!Drive(sink, group.query, StreamPosition{"unused", afterGtid}))
    return false;
  const std::uint64_t afterQuery = afterGtid + group.query.bytes.size();
  return Drive(sink, group.xid, StreamPosition{"unused", afterQuery});
}

// Same as DriveGroup(), with a pause before each of the last two events;
// StorageEventSink only publishes at GroupEnd, so published stays unchanged.
bool DriveGroupWithPauses(EventSink &sink, const DrivenGroup &group,
                          std::uint64_t start) {
  if (!Drive(sink, group.gtid, StreamPosition{"unused", start})) return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const std::uint64_t afterGtid = start + group.gtid.bytes.size();
  if (!Drive(sink, group.query, StreamPosition{"unused", afterGtid}))
    return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const std::uint64_t afterQuery = afterGtid + group.query.bytes.size();
  return Drive(sink, group.xid, StreamPosition{"unused", afterQuery});
}

// transactionLength must equal the group's own true byte length. Computed in
// two steps since the GTID event's body length depends on
// transactionLength's lenenc width; a placeholder past 65536 keeps that width
// stable.
DrivenGroup MakeGroupWithLargeQuery(std::uint64_t start, std::int64_t gno,
                                    std::size_t largeQueryBodySize) {
  const std::size_t queryEventLength = EVENT_HEADER_LENGTH + largeQueryBodySize;
  const std::size_t xidEventLength = EVENT_HEADER_LENGTH + 8;
  const std::size_t gtidEventLength =
      EVENT_HEADER_LENGTH +
      GtidBody(gno, /*width placeholder*/ 1'000'000).size();
  const std::uint64_t transactionLength =
      gtidEventLength + queryEventLength + xidEventLength;

  DrivenGroup group;
  group.gtid = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                         GtidBody(gno, transactionLength),
                         static_cast<std::uint32_t>(start + gtidEventLength));
  group.query = MakeEvent(
      2 /* a Query event */,
      std::vector<std::uint8_t>(largeQueryBodySize, 0xAB),
      static_cast<std::uint32_t>(start + gtidEventLength + queryEventLength));
  group.xid =
      MakeEvent(16 /* Xid event */, std::vector<std::uint8_t>(8, 0xCD),
                static_cast<std::uint32_t>(start + gtidEventLength +
                                           queryEventLength + xidEventLength));
  return group;
}

// A real StorageEventSink writes a synthetic stream while a StorageReader
// follows it on another thread, checking every Read() against Published()
// right after it returns; a deadline turns a stalled Next() into a failure.
TEST(
    StorageReaderTest,
    ReaderFollowsARealWriterAcrossAPausedGroupAndARotationCheckingEveryReadAgainstPublished) {
  test::StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  StorageReader reader(fixture.Directory(), fixture.Catalog(),
                       fixture.Published(), &fixture.Cache());

  const FreshFileHeader headerA = OpenFreshFile(sink, "binlog.000001");
  const std::uint32_t startA = headerA.afterPge;
  std::string openError;
  auto cursor = reader.Open("binlog.000001", openError);
  ASSERT_TRUE(cursor) << openError;

  const DrivenGroup group1 = MakeGroup(startA, /*gno=*/1);
  const std::uint64_t afterGroup1 = startA + group1.gtid.bytes.size() +
                                    group1.query.bytes.size() +
                                    group1.xid.bytes.size();
  const DrivenGroup group2 = MakeGroupWithLargeQuery(
      afterGroup1, /*gno=*/2, /*largeQueryBodySize=*/70000);
  const std::uint64_t groupsEnd = afterGroup1 + group2.gtid.bytes.size() +
                                  group2.query.bytes.size() +
                                  group2.xid.bytes.size();
  // Built here so its byte size can feed expectedTotal without re-deriving it.
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"),
                                static_cast<std::uint32_t>(groupsEnd + 30));
  const std::uint64_t sizeAfterClose = groupsEnd + rotate.bytes.size();

  // Filled in by the writer thread; read only after writer.join()
  // (happens-before).
  FreshFileHeader headerB;
  DrivenGroup group3{};
  std::uint32_t startB = 0;

  std::atomic<bool> writerOk{true};
  std::thread writer([&] {
    if (!DriveGroupWithPauses(sink, group1, startA)) {
      writerOk = false;
      return;
    }
    if (!DriveGroupWithPauses(sink, group2, afterGroup1)) {
      writerOk = false;
      return;
    }
    if (!Drive(sink, rotate, StreamPosition{"binlog.000001", groupsEnd})) {
      writerOk = false;
      return;
    }
    // A short pause so the reader can observe file A closed with no
    // successor indexed yet before file B's header lands.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    headerB = OpenFreshFile(sink, "binlog.000002");
    startB = headerB.afterPge;
    group3 = MakeGroup(startB, /*gno=*/3);
    if (!DriveGroup(sink, group3, startB)) writerOk = false;
  });

  std::vector<std::uint8_t> collected;
  bool readerOk = true;
  std::string readerError;
  std::thread reader_thread([&] {
    auto current = std::move(
        cursor);  // sole owner from here on - this thread alone touches it
    std::uint64_t offset = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Short enough to wake up and retry Read() before a large, still
    // unpublished write's own group finishes publishing.
    constexpr auto POLL_TIMEOUT = std::chrono::milliseconds(5);
    // File A's final size plus file B's size once group3 lands, from the
    // same helpers that build the stream itself.
    const std::size_t expectedTotal =
        sizeAfterClose + FreshFileHeaderLength() + 125;
    while (collected.size() < expectedTotal) {
      if (std::chrono::steady_clock::now() > deadline) {
        readerOk = false;
        readerError =
            "reader stalled before collecting the expected byte count";
        return;
      }
      std::vector<std::uint8_t> buffer(
          131072);  // room for the large group's own Query event in one call
      std::string error;
      const std::size_t bytesRead =
          reader.Read(*current, offset, buffer, error);
      if (!error.empty()) {
        readerOk = false;
        readerError = error;
        return;
      }
      // Read right after Read() returns, not before: published only
      // moves forward, so this is a valid upper bound for whatever
      // boundary Read() used internally.
      const PublishedPosition afterRead = reader.Published();
      if (current->FileName() == afterRead.fileName &&
          offset + bytesRead > afterRead.position) {
        readerOk = false;
        readerError =
            "read past the published position: " + current->FileName() +
            " offset " + std::to_string(offset) + "+" +
            std::to_string(bytesRead) + " > published " +
            std::to_string(afterRead.position);
        return;
      }
      if (bytesRead > 0) {
        collected.insert(collected.end(), buffer.begin(),
                         buffer.begin() + bytesRead);
        offset += bytesRead;
        continue;
      }

      // At the boundary now: still being written (wait) or already
      // superseded (move on). A short timeout lets the per-read check
      // above land inside the window of a large, unpublished write.
      if (reader.Published().fileName == current->FileName()) {
        reader.WaitForNewEvents(PublishedPosition{current->FileName(), offset},
                                POLL_TIMEOUT, WaitStyle::PollFirst);
        continue;
      }
      std::unique_ptr<FileCursor> nextCursor;
      std::string nextError;
      const NextFileOutcome outcome =
          reader.Next(*current, offset, nextCursor, nextError);
      if (outcome == NextFileOutcome::Found) {
        current = std::move(nextCursor);
        offset = 0;
        continue;
      }
      if (outcome == NextFileOutcome::NotYetAvailable) {
        // current not yet closed and fully read; the deterministic
        // case is exercised directly by
        // NextWithholdsFoundUntilCurrentIsClosedAndFullyRead above.
        reader.WaitForNewEvents(PublishedPosition{current->FileName(), offset},
                                POLL_TIMEOUT, WaitStyle::PollFirst);
        continue;
      }
      readerOk = false;
      readerError = nextError;
      return;
    }
  });

  writer.join();
  reader_thread.join();

  EXPECT_TRUE(writerOk.load());
  ASSERT_TRUE(readerOk) << readerError;

  // File A's "in use" bit is ambiguous (MarkClosed() timing vs. the
  // reader's first Read()) so it is masked out; file B's bit is always 1.
  constexpr std::size_t FLAGS_OFFSET_IN_EVENT =
      17;  // Common-Header flags field, relative to one event's own start
  constexpr std::size_t FILE_A_IN_USE_BYTE_INDEX =
      4 + FLAGS_OFFSET_IN_EVENT;  // magic(4) + flags offset, file A

  std::vector<std::uint8_t> expected{0xfe, 0x62, 0x69, 0x6e};  // BINLOG_MAGIC
  const auto appendBytes = [&expected](const std::vector<std::uint8_t> &bytes) {
    expected.insert(expected.end(), bytes.begin(), bytes.end());
  };
  appendBytes(headerA.fdeEventBytes);
  appendBytes(headerA.pgeEventBytes);
  appendBytes(group1.gtid.bytes);
  appendBytes(group1.query.bytes);
  appendBytes(group1.xid.bytes);
  appendBytes(group2.gtid.bytes);
  appendBytes(group2.query.bytes);
  appendBytes(group2.xid.bytes);
  appendBytes(rotate.bytes);
  expected.insert(expected.end(),
                  {0xfe, 0x62, 0x69,
                   0x6e});  // BINLOG_MAGIC - file B's own header starts here
  auto fdeBytesB = headerB.fdeEventBytes;
  fdeBytesB[FLAGS_OFFSET_IN_EVENT] |=
      0x01;  // Create() always sets this bit; file B is never closed in this
             // test
  appendBytes(fdeBytesB);
  appendBytes(headerB.pgeEventBytes);
  appendBytes(group3.gtid.bytes);
  appendBytes(group3.query.bytes);
  appendBytes(group3.xid.bytes);

  ASSERT_EQ(collected.size(), expected.size());
  collected[FILE_A_IN_USE_BYTE_INDEX] = 0;
  expected[FILE_A_IN_USE_BYTE_INDEX] = 0;
  EXPECT_EQ(collected, expected);

  // Independently: the reader's bytes match what is on disk, with the
  // same file A ambiguous byte masked out.
  fixture.Drain();
  auto onDisk = ReadFile(fixture.Path("binlog.000001"));
  fixture.Drain();
  const auto fileBOnDisk = ReadFile(fixture.Path("binlog.000002"));
  onDisk.insert(onDisk.end(), fileBOnDisk.begin(), fileBOnDisk.end());
  ASSERT_EQ(onDisk.size(), expected.size());
  onDisk[FILE_A_IN_USE_BYTE_INDEX] = 0;
  EXPECT_EQ(collected, onDisk);
}

}  // namespace
}  // namespace binlog_streamer
