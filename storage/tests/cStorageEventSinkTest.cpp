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

#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "cFakeTransport.hpp"
#include "cScriptedStreamBuilder.hpp"
#include "gtid/cGtidSet.hpp"
#include "receiver/cEventStreamReader.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <unistd.h>  // ::truncate() - simulates a file already holding several GiB without writing that much
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace binlog_streamer {
namespace {

using test::StorageSinkFixture;

class NullSink : public EventSink {
 public:
  bool OnEventBegin(const EventHeader &, const StreamPosition &) override {
    return true;
  }
  bool OnEventBytes(std::span<const std::uint8_t>) override { return true; }
  bool OnEventEnd() override { return true; }
};

// Counts calls to check that StorageEventSink forwards every event to
// `next`, the same shape EventCounterSink relies on in main.cpp.
class CountingSink : public EventSink {
 public:
  unsigned beginCalls = 0;
  unsigned bytesCalls = 0;
  unsigned endCalls = 0;
  bool OnEventBegin(const EventHeader &, const StreamPosition &) override {
    ++beginCalls;
    return true;
  }
  bool OnEventBytes(std::span<const std::uint8_t>) override {
    ++bytesCalls;
    return true;
  }
  bool OnEventEnd() override {
    ++endCalls;
    return true;
  }
};

std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

std::uint64_t FileSize(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::uint64_t>(size);
}

// Reads only `length` bytes at `offset`, so the past-4-GiB tests below can
// check a few bytes of a multi-GiB sparse file without a same-sized buffer.
std::vector<std::uint8_t> ReadFileAt(const std::filesystem::path &path,
                                     std::uint64_t offset, std::size_t length) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(static_cast<std::streamoff>(offset));
  std::vector<std::uint8_t> buffer(length);
  input.read(reinterpret_cast<char *>(buffer.data()),
             static_cast<std::streamsize>(length));
  return buffer;
}

void AppendLittleEndian(std::vector<std::uint8_t> &out, std::uint64_t value,
                        std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

// Local lenenc encoder, kept separate so this test does not depend on
// bs-protocol.
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

// One raw wire event, handed to a sink directly, bypassing wire parsing.
struct WireEvent {
  EventHeader header;
  std::vector<std::uint8_t> bytes;  // as one OnEventBytes() call would carry it
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

// A valid FDE body: EndPreviousGtids() parses it with
// FormatDescriptionEventCodec to build its StoredFileRecord. Server version
// 8.4.11 implies a checksum trailer (CRC32 descriptor + 4-byte room).
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

// A valid, strictly-decodable PGE body: AddFromEncoding() requires the
// whole buffer consumed, so a CRC32 trailer is appended to match SampleFde().
std::vector<std::uint8_t> SamplePreviousGtids() {
  std::vector<std::uint8_t> body = GtidSet().Encode(/*skipTaggedGtids=*/false);
  body.insert(body.end(), 4, 0x00);
  return body;
}

// SampleFde() with `created` (offset 52) set to a caller-chosen value, for
// resume tests needing two FDEs differing only in this field.
std::vector<std::uint8_t> SampleFdeWithCreated(std::uint32_t created) {
  auto body = SampleFde();
  for (int i = 0; i < 4; ++i)
    body[52 + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(created >> (8 * i));
  return body;
}

struct FreshFileHeader {
  std::uint32_t afterPge = 0;
  // Whole wire event bytes, so a test can build a byte-exact disk
  // expectation. IN_USE_FLAG_OFFSET (21) is byte 17 of fdeEventBytes;
  // Create() forces its bit 0 to 1.
  std::vector<std::uint8_t> fdeEventBytes;
  std::vector<std::uint8_t> pgeEventBytes;
};

FreshFileHeader OpenFreshFile(EventSink &sink, const std::string &fileName,
                              std::uint64_t announcedAt = 0) {
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, fileName), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_TRUE(Drive(sink, rotate, StreamPosition{fileName, announcedAt}));

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

// start is 64-bit, unlike the wire's own 32-bit nextPosition, so a caller
// can place a group past 4 GiB.
DrivenGroup MakeGroup(std::uint64_t start, std::int64_t gno = 1) {
  DrivenGroup group;
  group.gtid = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                         GtidBody(gno, GROUP_TRANSACTION_LENGTH),
                         static_cast<std::uint32_t>(start + 69));
  group.query = MakeEvent(2 /* a Query event - no EventType enumerator of its
                               own, eEventType.hpp */
                          ,
                          std::vector<std::uint8_t>(10, 0xAB),
                          static_cast<std::uint32_t>(start + 69 + 29));
  group.xid = MakeEvent(16 /* Xid event */, std::vector<std::uint8_t>(8, 0xCD),
                        static_cast<std::uint32_t>(start + 69 + 29 + 27));
  return group;
}

bool DriveGroup(EventSink &sink, const DrivenGroup &group,
                std::uint64_t start) {
  if (!Drive(sink, group.gtid, StreamPosition{"unused", start})) return false;
  if (!Drive(sink, group.query, StreamPosition{"unused", start + 69}))
    return false;
  return Drive(sink, group.xid, StreamPosition{"unused", start + 69 + 29});
}

TEST(StorageEventSinkTest,
     WritesTheFullHeaderAndTheFirstGroupThenLeavesTheFileOpen) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const FreshFileHeader header = OpenFreshFile(sink, "binlog.000001");
  ASSERT_EQ(header.afterPge, 116u);  // 4 (magic) + 19+62 (FDE) + 19+12 (PGE)
  const std::uint32_t start = header.afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));

  std::vector<std::uint8_t> expected = {0xfe, 0x62, 0x69,
                                        0x6e};  // BINLOG_MAGIC
  auto fdeOnDisk = header.fdeEventBytes;
  fdeOnDisk[IN_USE_FLAG_OFFSET - 4] |=
      0x01;  // "file in use" bit forced to 1 on creation
             // (BinlogFileWriter::Create)
  expected.insert(expected.end(), fdeOnDisk.begin(), fdeOnDisk.end());
  expected.insert(expected.end(), header.pgeEventBytes.begin(),
                  header.pgeEventBytes.end());
  expected.insert(expected.end(), group.gtid.bytes.begin(),
                  group.gtid.bytes.end());
  expected.insert(expected.end(), group.query.bytes.begin(),
                  group.query.bytes.end());
  expected.insert(expected.end(), group.xid.bytes.begin(),
                  group.xid.bytes.end());

  fixture.Drain();
  const auto onDisk = ReadFile(fixture.Path("binlog.000001"));
  EXPECT_EQ(onDisk, expected);
  EXPECT_EQ(onDisk[IN_USE_FLAG_OFFSET],
            0x01);  // still open - no ROTATE has closed it yet
}

TEST(StorageEventSinkTest, ClosesTheFileOnAGenuineRotateClearingTheInUseBit) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));

  const std::uint32_t groupEnd = start + 125;
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), groupEnd + 30 /* nextPosition is not read for Rotate events; any placeholder is fine */);
  ASSERT_TRUE(Drive(sink, rotate, StreamPosition{"binlog.000001", groupEnd}));

  fixture.Drain();
  const auto onDisk = ReadFile(fixture.Path("binlog.000001"));
  EXPECT_EQ(onDisk.size(), groupEnd + rotate.bytes.size());
  EXPECT_EQ(onDisk[IN_USE_FLAG_OFFSET],
            0x00);  // bit cleared, rest of the byte untouched
                    // (BinlogFileWriter::MarkClosed)
}

// A source stopped or crashed without a genuine ROTATE; the next thing on
// the wire is the next file's announcement.
TEST(StorageEventSinkTest,
     ClosesTheFileTheSourceEndedWithoutARotateAndGoesOnToTheNextOne) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  ASSERT_TRUE(DriveGroup(sink, MakeGroup(start), start));
  const std::uint32_t groupEnd = start + 125;
  const auto stop =
      MakeEvent(3 /* a Stop event */, {}, groupEnd + EVENT_HEADER_LENGTH);
  ASSERT_TRUE(Drive(sink, stop, StreamPosition{"binlog.000001", groupEnd}));

  const std::uint32_t nextStart =
      OpenFreshFile(sink, "binlog.000002", groupEnd + stop.bytes.size())
          .afterPge;
  ASSERT_TRUE(DriveGroup(sink, MakeGroup(nextStart, 2), nextStart));

  fixture.Drain();
  const auto first = ReadFile(fixture.Path("binlog.000001"));
  EXPECT_EQ(first.size(), groupEnd + stop.bytes.size());
  EXPECT_EQ(first[IN_USE_FLAG_OFFSET], 0x00);
  const auto second = ReadFile(fixture.Path("binlog.000002"));
  EXPECT_EQ(second.size(), nextStart + 125u);
  EXPECT_EQ(second[IN_USE_FLAG_OFFSET], 0x01);
  ASSERT_EQ(fixture.Catalog().Size(), 2u);
  EXPECT_FALSE(fixture.Catalog().At(0).inUse);
  EXPECT_EQ(fixture.Catalog().At(0).size, first.size());
  EXPECT_TRUE(fixture.Catalog().At(1).inUse);
}

TEST(StorageEventSinkTest,
     RefusesAnAnnouncementOfTheNextFileInsideAnOpenTransactionGroup) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(Drive(sink, group.gtid, StreamPosition{"binlog.000001", start}));

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000002"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(
      Drive(sink, rotate, StreamPosition{"binlog.000001", start + 69}));
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  EXPECT_NE(sink.LastError().message.find("open transaction group"),
            std::string::npos)
      << sink.LastError().message;
}

TEST(StorageEventSinkTest, RefusesASecondAnnouncementOfTheFileAlreadyOpen) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000001"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"binlog.000001", start}));
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  EXPECT_NE(sink.LastError().message.find("still open"), std::string::npos)
      << sink.LastError().message;
}

// Checks both the on-disk index and the in-RAM catalog against the same
// fixture, not against each other, so they can't merely agree with each other.
TEST(StorageEventSinkTest, RecordsTheNewFileInTheIndexAndTheCatalogOnCreate) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  OpenFreshFile(sink, "binlog.000001");

  std::vector<std::string> indexNames;
  std::string indexError;
  fixture.Drain();
  ASSERT_TRUE(BinlogIndexFile::Load(fixture.Path("binlog.index").string(),
                                    indexNames, indexError))
      << indexError;
  EXPECT_EQ(indexNames, std::vector<std::string>{"binlog.000001"});

  ASSERT_EQ(fixture.Catalog().Size(), 1u);
  const StoredFileRecord record = fixture.Catalog().At(0);
  EXPECT_EQ(record.name, "binlog.000001");
  EXPECT_EQ(record.basename, "binlog");
  EXPECT_EQ(record.number, 1u);
  EXPECT_TRUE(record.inUse);
}

TEST(StorageEventSinkTest, UpdatesTheCatalogRecordOnClose) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));
  const std::uint32_t groupEnd = start + 125;
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), groupEnd + 30);
  ASSERT_TRUE(Drive(sink, rotate, StreamPosition{"binlog.000001", groupEnd}));

  ASSERT_EQ(fixture.Catalog().Size(), 1u);
  const StoredFileRecord record = fixture.Catalog().At(0);
  EXPECT_FALSE(record.inUse);
  fixture.Drain();
  EXPECT_EQ(record.size, ReadFile(fixture.Path("binlog.000001")).size());
}

TEST(StorageEventSinkTest, DrainAndSyncWritesTheOpenGroupWithoutPublishingIt) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const auto start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const auto group = MakeGroup(start);
  ASSERT_TRUE(Drive(sink, group.gtid, {"binlog.000001", start}));
  auto drain = std::async(std::launch::async,
                          [&] { return fixture.Writer().DrainAndSync(); });
  const bool ready =
      drain.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (!ready) fixture.Writer().Stop();
  ASSERT_TRUE(ready)
      << "writer did not drain the unpublished group within five seconds";
  ASSERT_TRUE(drain.get());
  fixture.Drain();
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")).size(),
            start + group.gtid.bytes.size());
  EXPECT_EQ(fixture.Published().Current().position, start);
}

// These file names are reachable as a path component (m_dataDir / fileName)
// without a dedicated check - not merely hypothetical inputs.
TEST(StorageEventSinkTest, RefusesAnEmptyFileNameInRotate) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, ""), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"", 0}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
}

TEST(StorageEventSinkTest, RefusesAFileNameContainingAPathSeparatorInRotate) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "sub/binlog.000001"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"sub/binlog.000001", 0}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  fixture.Drain();
  EXPECT_FALSE(std::filesystem::exists(
      fixture.Directory() / "sub"));  // no escape from data_dir happened
}

TEST(StorageEventSinkTest,
     RefusesAFileNameEscapingTheDataDirectoryViaDotDotInRotate) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "../binlog.000001"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"../binlog.000001", 0}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  fixture.Drain();
  EXPECT_FALSE(std::filesystem::exists(fixture.Directory().parent_path() /
                                       "binlog.000001"));  // no escape happened
}

TEST(StorageEventSinkTest,
     RefusesAFileNameWithoutTheBaseDotDigitsFormInRotate) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "not-a-binlog-name"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"not-a-binlog-name", 0}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
}

// One of four identical MAX_BUFFERED_EVENT_SIZE checks against
// header.eventLength; the check runs in OnEventBegin() before any body byte
// is requested, so this drives it directly with no body at all.
TEST(StorageEventSinkTest,
     RefusesAnArtificialRotateEventDeclaringMoreThanTheBufferLimit) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  EventHeader header;
  header.type = static_cast<std::uint8_t>(EventType::Rotate);
  header.flags = EVENT_FLAG_ARTIFICIAL;
  header.eventLength = static_cast<std::uint32_t>(MAX_BUFFERED_EVENT_SIZE) + 1;
  EXPECT_FALSE(sink.OnEventBegin(header, StreamPosition{"binlog.000001", 0}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
}

TEST(StorageEventSinkTest,
     RefusesAnArtificialRotateNamingAFileThatAlreadyExists) {
  StorageSinkFixture fixture;
  {
    std::ofstream preexisting(fixture.Path("binlog.000005"));
    preexisting << "leftover from an earlier run";
  }
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000005"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"binlog.000005", 0}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::NotEmpty);
}

TEST(StorageEventSinkTest,
     ForwardsEveryEventToTheChainedSinkEvenWhenStorageRefusesIt) {
  StorageSinkFixture fixture;
  {
    std::ofstream preexisting(fixture.Path("binlog.000005"));
    preexisting << "leftover from an earlier run";
  }
  CountingSink next;
  auto &sink = fixture.OpenSink(next);

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000005"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"binlog.000005", 0}));
  // The failure is only discovered once the whole event is in hand (from
  // OnEventEnd()), so `next` must have already seen all three calls by then.
  EXPECT_EQ(next.beginCalls, 1u);
  EXPECT_EQ(next.bytesCalls, 1u);
  EXPECT_EQ(next.endCalls, 1u);
}

TEST(StorageEventSinkTest, RefusesAnEventPositionedPastWhatHasBeenWritten) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  const auto query =
      MakeEvent(2, std::vector<std::uint8_t>(5, 0xAB), start + 10 + 19 + 5);
  EXPECT_FALSE(Drive(
      sink, query,
      StreamPosition{"binlog.000001", start + 10}));  // a 10-byte gap before it
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::GapDetected);
  // Same StorageFailure::GapDetected as the heartbeat branch below, checked
  // by text so a future collapse of the two checks would still be caught.
  EXPECT_NE(sink.LastError().message.find("event position is past"),
            std::string::npos);
}

TEST(StorageEventSinkTest, RefusesAnEventPositionedBehindWhatHasBeenWritten) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  const auto query =
      MakeEvent(2, std::vector<std::uint8_t>(5, 0xAB), start - 1 + 19 + 5);
  EXPECT_FALSE(Drive(sink, query, StreamPosition{"binlog.000001", start - 1}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
}

// A replay lying entirely inside bytes already written reaches the resume
// comparison, which reads the file back - but a file this run created was
// never opened for reading. Refused by name instead.
TEST(StorageEventSinkTest, RefusesAReplayOfBytesThisRunWroteItself) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));

  // The same Query event again, at the position it was written at: its end
  // stays within what has been appended, so no gap check catches it.
  EXPECT_FALSE(
      Drive(sink, group.query, StreamPosition{"binlog.000001", start + 69}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  EXPECT_NE(sink.LastError().message.find("this run created itself"),
            std::string::npos);
}

TEST(StorageEventSinkTest, IgnoresAHeartbeatAtTheWrittenPosition) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  const auto heartbeat =
      MakeEvent(static_cast<std::uint8_t>(EventType::Heartbeat),
                std::vector<std::uint8_t>{'b', 'i', 'n'}, start);
  EXPECT_TRUE(Drive(sink, heartbeat, StreamPosition{"binlog.000001", start}));
  EXPECT_FALSE(sink.HasFailed());
  fixture.Drain();
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")).size(),
            start);  // nothing written for the heartbeat itself
}

TEST(StorageEventSinkTest, RefusesAHeartbeatPastTheWrittenPosition) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  // Checks `position` (EventStreamReader's accumulated count), not
  // header.nextPosition.
  const auto heartbeat =
      MakeEvent(static_cast<std::uint8_t>(EventType::Heartbeat),
                std::vector<std::uint8_t>{'b', 'i', 'n'}, start + 500);
  EXPECT_FALSE(
      Drive(sink, heartbeat, StreamPosition{"binlog.000001", start + 500}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::GapDetected);
  EXPECT_NE(sink.LastError().message.find("a heartbeat announced"),
            std::string::npos);
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

// A tagged GTID opens a group like an untagged one and closes at its own
// transaction_length, proven by the next group's GTID being accepted there.
TEST(StorageEventSinkTest,
     AcceptsATaggedGtidGroupAndClosesItByItsTransactionLength) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  // 19 + 62 (GTID) + 19 + 200 (Query) + 19 + 18 (Xid) = 337.
  const auto gtid = MakeEvent(static_cast<std::uint8_t>(EventType::GtidTagged),
                              REAL_TAGGED_GTID_BODY, start + 81);
  const auto query = MakeEvent(
      2 /* Query */, std::vector<std::uint8_t>(200, 0xAB), start + 81 + 219);
  const auto xid =
      MakeEvent(16 /* Xid */, std::vector<std::uint8_t>(18, 0xCD), start + 337);
  ASSERT_TRUE(Drive(sink, gtid, StreamPosition{"binlog.000001", start}));
  ASSERT_TRUE(Drive(sink, query, StreamPosition{"binlog.000001", start + 81}));
  ASSERT_TRUE(
      Drive(sink, xid, StreamPosition{"binlog.000001", start + 81 + 219}));
  ASSERT_FALSE(sink.HasFailed());

  const DrivenGroup nextGroup =
      MakeGroup(start + REAL_TAGGED_TRANSACTION_LENGTH, /*gno=*/2);
  EXPECT_TRUE(
      DriveGroup(sink, nextGroup, start + REAL_TAGGED_TRANSACTION_LENGTH));
  EXPECT_FALSE(sink.HasFailed()) << sink.LastError().message;
}

TEST(StorageEventSinkTest, RefusesAMalformedTaggedGtidEvent) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  const auto tagged =
      MakeEvent(static_cast<std::uint8_t>(EventType::GtidTagged),
                std::vector<std::uint8_t>(20, 0), start + 20 + 19);
  EXPECT_FALSE(Drive(sink, tagged, StreamPosition{"binlog.000001", start}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  EXPECT_NE(sink.LastError().message.find("tagged GTID"), std::string::npos)
      << sink.LastError().message;
}

TEST(StorageEventSinkTest, RefusesAGenuineRotateInsideAnOpenTransactionGroup) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;

  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(
      Drive(sink, group.gtid,
            StreamPosition{"binlog.000001",
                           start}));  // opens the group, does not close it

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000002"), start + 69 + 30);
  EXPECT_FALSE(
      Drive(sink, rotate, StreamPosition{"binlog.000001", start + 69}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
}

// Mirror of OpenFreshFile() for a file the catalog's last record still shows
// "in use"; returns the offset right after the Previous_gtids_event.
std::uint32_t ResumeExistingFile(EventSink &sink, const std::string &fileName,
                                 std::span<const std::uint8_t> fde) {
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, fileName), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_TRUE(Drive(sink, rotate, StreamPosition{fileName, 0}));

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

  return afterPge;
}

// A file left "in use" by a crash mid-run is reopened for appending; the
// source's resend of its header and groups is verified byte for byte, not
// rewritten.
TEST(StorageEventSinkTest,
     ResumesAnExistingLastFileAndSkipsByteForByteMatchingReplayEvents) {
  StorageSinkFixture fixture;
  NullSink next;

  std::uint32_t start = 0;
  DrivenGroup group1;
  {
    auto &sink = fixture.OpenSink(next);
    start = OpenFreshFile(sink, "binlog.000001").afterPge;
    group1 = MakeGroup(start);
    ASSERT_TRUE(DriveGroup(sink, group1, start));
    // sink goes out of scope without a real ROTATE - the same "still in
    // use" state a crash leaves; group1 is whole, so reusing catalog
    // directly here is equivalent to running StorageRecovery first.
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_EQ(fixture.Catalog().Size(), 1u);
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  auto &resumed = fixture.OpenSink(next);
  ResumeExistingFile(resumed, "binlog.000001", SampleFde());
  ASSERT_TRUE(DriveGroup(resumed, group1,
                         start));  // resent byte for byte - already on disk
  EXPECT_FALSE(resumed.HasFailed());

  const std::uint32_t groupEnd = start + 125;
  fixture.Drain();
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")).size(),
            groupEnd);  // unchanged - nothing duplicated

  // A genuinely new group lands exactly at the true end of file, once
  // replay has caught up to it, and is appended normally.
  const DrivenGroup group2 = MakeGroup(groupEnd, /*gno=*/2);
  ASSERT_TRUE(DriveGroup(resumed, group2, groupEnd));
  fixture.Drain();
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")).size(), groupEnd + 125);
}

TEST(StorageEventSinkTest,
     RefusesAResumedFileWhoseReplayedBytesDoNotMatchDisk) {
  StorageSinkFixture fixture;
  NullSink next;

  std::uint32_t start = 0;
  {
    auto &sink = fixture.OpenSink(next);
    start = OpenFreshFile(sink, "binlog.000001").afterPge;
    const DrivenGroup group1 = MakeGroup(start);
    ASSERT_TRUE(DriveGroup(sink, group1, start));
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }

  auto &resumed = fixture.OpenSink(next);
  const std::uint32_t afterPge =
      ResumeExistingFile(resumed, "binlog.000001", SampleFde());
  ASSERT_EQ(afterPge, start);

  // A different GNO than what is actually on disk at this same offset -
  // storage does not match source history.
  const DrivenGroup mismatched = MakeGroup(start, /*gno=*/99);
  EXPECT_FALSE(
      Drive(resumed, mismatched.gtid, StreamPosition{"binlog.000001", start}));
  ASSERT_TRUE(resumed.HasFailed());
  EXPECT_EQ(resumed.LastError().failure, StorageFailure::Malformed);
}

// `created` is the one field a resumed FDE may differ on: the source zeroes
// it whenever it skips a file's first transaction, as resuming does.
TEST(StorageEventSinkTest,
     ResumingAFileToleratesADifferentFormatDescriptionCreatedField) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    // Built with a non-zero `created`; EndRotateAnnouncement() finds an
    // empty catalog here, so this is still the ordinary create path.
    auto &sink = fixture.OpenSink(next);
    ResumeExistingFile(
        sink, "binlog.000001",
        SampleFdeWithCreated(1700000000));  // as if written at server startup
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  auto &resumed = fixture.OpenSink(next);
  // Resent with created zeroed - exactly the field EqualForResume() ignores.
  ResumeExistingFile(resumed, "binlog.000001", SampleFdeWithCreated(0));
  EXPECT_FALSE(resumed.HasFailed());
}

// Grown with ::truncate() instead of actually writing ~4 GiB: a hole reads
// back as zero on this project's target filesystems (checked on APFS). Only
// the header and the two appended groups are ever compared, never the hole.
TEST(StorageEventSinkTest,
     ResumesAFilePastFourGiBAndClosesAGroupCrossingTheBoundary) {
  StorageSinkFixture fixture;
  NullSink next;

  std::uint32_t afterPge = 0;
  {
    auto &sink = fixture.OpenSink(next);
    afterPge = OpenFreshFile(sink, "binlog.000001").afterPge;
    // sink goes out of scope without a real ROTATE - the same "still in
    // use" state a crash leaves, same as every other resume test above.
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  constexpr std::uint64_t FOUR_GIB = std::uint64_t{1} << 32;
  const std::uint64_t writtenLength = FOUR_GIB - 50;
  fixture.Drain();
  ASSERT_EQ(::truncate(fixture.Path("binlog.000001").c_str(),
                       static_cast<off_t>(writtenLength)),
            0);
  std::string recoveredError;
  ASSERT_TRUE(fixture.Catalog().UpdateSize(writtenLength, recoveredError))
      << recoveredError;

  auto &resumed = fixture.OpenSink(next);
  ResumeExistingFile(resumed, "binlog.000001", SampleFde());

  // A heartbeat right after resume, still behind writtenLength - proves it
  // is not mistaken for a gap before the stream catches up.
  const auto heartbeat =
      MakeEvent(static_cast<std::uint8_t>(EventType::Heartbeat),
                std::vector<std::uint8_t>{'b', 'i', 'n'},
                static_cast<std::uint32_t>(writtenLength));
  ASSERT_TRUE(
      Drive(resumed, heartbeat, StreamPosition{"binlog.000001", afterPge}));
  ASSERT_FALSE(resumed.HasFailed());

  // The stream has now caught up to writtenLength; a group starting there
  // straddles the 4 GiB boundary.
  const DrivenGroup group1 = MakeGroup(writtenLength, /*gno=*/1);
  ASSERT_TRUE(DriveGroup(resumed, group1, writtenLength));
  EXPECT_FALSE(resumed.HasFailed());

  const std::uint64_t group1End = writtenLength + 125;
  const DrivenGroup group2 = MakeGroup(group1End, /*gno=*/2);
  ASSERT_TRUE(DriveGroup(resumed, group2, group1End));
  EXPECT_FALSE(resumed.HasFailed());

  fixture.Drain();
  EXPECT_EQ(FileSize(fixture.Path("binlog.000001")), writtenLength + 250);
  fixture.Drain();
  const auto onDisk = ReadFileAt(fixture.Path("binlog.000001"), writtenLength,
                                 group1.gtid.bytes.size());
  EXPECT_EQ(onDisk, group1.gtid.bytes);
}

// A heartbeat replaying a position behind what's written must not be
// mistaken for a gap just because its wire header's low 32 bits are larger.
TEST(StorageEventSinkTest,
     ToleratesAReplayHeartbeatBehindTheWrittenLengthWhoseLow32BitsAreLarger) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    auto &sink = fixture.OpenSink(next);
    OpenFreshFile(sink, "binlog.000001");
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  constexpr std::uint64_t FOUR_GIB = std::uint64_t{1} << 32;
  const std::uint64_t writtenLength = FOUR_GIB - 10;
  fixture.Drain();
  ASSERT_EQ(::truncate(fixture.Path("binlog.000001").c_str(),
                       static_cast<off_t>(writtenLength)),
            0);
  std::string recoveredError;
  ASSERT_TRUE(fixture.Catalog().UpdateSize(writtenLength, recoveredError))
      << recoveredError;

  DrivenGroup group;
  {
    auto &sink = fixture.OpenSink(next);
    ResumeExistingFile(sink, "binlog.000001", SampleFde());
    group = MakeGroup(writtenLength, /*gno=*/1);
    ASSERT_TRUE(DriveGroup(sink, group, writtenLength));
    ASSERT_FALSE(sink.HasFailed());
    // sink goes out of scope without a real ROTATE - still "in use".
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  fixture.Drain();
  const std::uint64_t sizeAfterFirstAppend =
      FileSize(fixture.Path("binlog.000001"));
  ASSERT_EQ(sizeAfterFirstAppend, writtenLength + 125);

  auto &resumed = fixture.OpenSink(next);
  ResumeExistingFile(resumed, "binlog.000001", SampleFde());

  // header.nextPosition's low bits (4294967286) look past what's written
  // (115) even though writtenLength itself is behind (2^32-10 < 2^32+115);
  // a mutant reading header.nextPosition instead of `position` would refuse
  // this.
  const auto heartbeat =
      MakeEvent(static_cast<std::uint8_t>(EventType::Heartbeat),
                std::vector<std::uint8_t>{'b', 'i', 'n'},
                static_cast<std::uint32_t>(writtenLength));
  ASSERT_TRUE(Drive(resumed, heartbeat,
                    StreamPosition{"binlog.000001", writtenLength}));
  ASSERT_FALSE(resumed.HasFailed());

  // The source resends the same group on reconnect: identical bytes
  // compare against stored history instead of appending again.
  ASSERT_TRUE(DriveGroup(resumed, group, writtenLength));
  EXPECT_FALSE(resumed.HasFailed());
  fixture.Drain();
  EXPECT_EQ(FileSize(fixture.Path("binlog.000001")), sizeAfterFirstAppend);

  const DrivenGroup group2 = MakeGroup(sizeAfterFirstAppend, /*gno=*/2);
  ASSERT_TRUE(DriveGroup(resumed, group2, sizeAfterFirstAppend));
  EXPECT_FALSE(resumed.HasFailed());
  fixture.Drain();
  EXPECT_EQ(FileSize(fixture.Path("binlog.000001")),
            sizeAfterFirstAppend + 125);
  fixture.Drain();
  const auto onDisk =
      ReadFileAt(fixture.Path("binlog.000001"), sizeAfterFirstAppend,
                 group2.gtid.bytes.size());
  EXPECT_EQ(onDisk, group2.gtid.bytes);
}

// Both gap checks must compare full 64-bit position, not the low 32 bits
// (via a cast or header.nextPosition) - a bug there hides under 4 GiB, so
// both branches are tested past it.
TEST(StorageEventSinkTest,
     RefusesAnOrdinaryEventPastTheWrittenLengthPastFourGiB) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    auto &sink = fixture.OpenSink(next);
    OpenFreshFile(sink, "binlog.000001");
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  constexpr std::uint64_t FOUR_GIB = std::uint64_t{1} << 32;
  const std::uint64_t writtenLength = FOUR_GIB - 50;
  fixture.Drain();
  ASSERT_EQ(::truncate(fixture.Path("binlog.000001").c_str(),
                       static_cast<off_t>(writtenLength)),
            0);
  std::string recoveredError;
  ASSERT_TRUE(fixture.Catalog().UpdateSize(writtenLength, recoveredError))
      << recoveredError;

  auto &resumed = fixture.OpenSink(next);
  ResumeExistingFile(resumed, "binlog.000001", SampleFde());

  // claimedPosition is past writtenLength in true 64-bit terms, but would
  // look far behind if compared by low 32 bits alone.
  const std::uint64_t claimedPosition = FOUR_GIB + 10;
  const auto gtid = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                              GtidBody(1, GROUP_TRANSACTION_LENGTH),
                              static_cast<std::uint32_t>(claimedPosition + 69));
  EXPECT_FALSE(
      Drive(resumed, gtid, StreamPosition{"binlog.000001", claimedPosition}));
  ASSERT_TRUE(resumed.HasFailed());
  EXPECT_EQ(resumed.LastError().failure, StorageFailure::GapDetected);
  EXPECT_NE(resumed.LastError().message.find("event position is past"),
            std::string::npos);
}

TEST(StorageEventSinkTest, RefusesAHeartbeatPastTheWrittenLengthPastFourGiB) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    auto &sink = fixture.OpenSink(next);
    OpenFreshFile(sink, "binlog.000001");
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  constexpr std::uint64_t FOUR_GIB = std::uint64_t{1} << 32;
  const std::uint64_t writtenLength = FOUR_GIB - 50;
  fixture.Drain();
  ASSERT_EQ(::truncate(fixture.Path("binlog.000001").c_str(),
                       static_cast<off_t>(writtenLength)),
            0);
  std::string recoveredError;
  ASSERT_TRUE(fixture.Catalog().UpdateSize(writtenLength, recoveredError))
      << recoveredError;

  auto &resumed = fixture.OpenSink(next);
  ResumeExistingFile(resumed, "binlog.000001", SampleFde());

  // header.nextPosition itself carries only 10; reading that field instead
  // of the position would compare 10 with writtenLength and miss the gap.
  const std::uint64_t claimedPosition = FOUR_GIB + 10;
  const auto heartbeat =
      MakeEvent(static_cast<std::uint8_t>(EventType::Heartbeat),
                std::vector<std::uint8_t>{'b', 'i', 'n'},
                static_cast<std::uint32_t>(claimedPosition));
  EXPECT_FALSE(Drive(resumed, heartbeat,
                     StreamPosition{"binlog.000001", claimedPosition}));
  ASSERT_TRUE(resumed.HasFailed());
  EXPECT_EQ(resumed.LastError().failure, StorageFailure::GapDetected);
  EXPECT_NE(resumed.LastError().message.find("a heartbeat announced"),
            std::string::npos);
}

// A rerun after the source moves on must not leave the catalog's last
// record permanently "in use", or the next restart would find two open
// records and refuse to start.
TEST(StorageEventSinkTest,
     ClosesTheAbandonedInUseFileWhenTheSourceMovesToADifferentOne) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    auto &sink = fixture.OpenSink(next);
    const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
    const DrivenGroup group = MakeGroup(start);
    ASSERT_TRUE(DriveGroup(sink, group, start));
    // No real ROTATE - the same "still in use" state a crash leaves.
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);
  fixture.Drain();
  const auto sizeBeforeResume = ReadFile(fixture.Path("binlog.000001")).size();

  // The callback lets main.cpp report the closure immediately rather than
  // only once the whole stream ends; checked here alongside the accessor.
  std::string callbackName;
  std::uint64_t callbackSize = 0;
  unsigned callbackCalls = 0;
  auto &resumed =
      fixture.OpenSink(next, [&](const std::string &name, std::uint64_t size) {
        ++callbackCalls;
        callbackName = name;
        callbackSize = size;
      });
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000002"), 0, EVENT_FLAG_ARTIFICIAL);
  ASSERT_TRUE(Drive(resumed, rotate, StreamPosition{"binlog.000002", 0}));

  ASSERT_EQ(
      fixture.Catalog().Size(),
      1u);  // binlog.000002 is not indexed yet - only once its own header lands
  EXPECT_FALSE(
      fixture.Catalog().At(0).inUse);  // binlog.000001 closed, without ever
                                       // seeing a real ROTATE of its own
  fixture.Drain();
  const auto onDisk = ReadFile(fixture.Path("binlog.000001"));
  EXPECT_EQ(onDisk.size(), sizeBeforeResume);   // no bytes added or removed
  EXPECT_EQ(onDisk[IN_USE_FLAG_OFFSET], 0x00);  // bit cleared
  EXPECT_EQ(callbackCalls, 1u);
  EXPECT_EQ(callbackName, "binlog.000001");
  EXPECT_EQ(callbackSize, sizeBeforeResume);

  // The new sink now proceeds to build binlog.000002 normally.
  const auto fde = SampleFde();
  const std::uint32_t afterFde =
      4 + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + fde.size());
  const auto fdeEvent = MakeEvent(
      static_cast<std::uint8_t>(EventType::FormatDescription), fde, afterFde);
  EXPECT_TRUE(Drive(resumed, fdeEvent, StreamPosition{"binlog.000002", 4}));
  EXPECT_FALSE(resumed.HasFailed());
}

// File numbering within one base name only ever goes forward; an earlier
// number is evidence the source was replaced, refused before storage's last
// file is touched.
TEST(StorageEventSinkTest,
     RefusesAnArtificialRotateNamingAnEarlierNumberedFileOfTheSameBase) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    auto &sink = fixture.OpenSink(next);
    const std::uint32_t start = OpenFreshFile(sink, "binlog.000005").afterPge;
    const DrivenGroup group = MakeGroup(start);
    ASSERT_TRUE(DriveGroup(sink, group, start));
    // No real ROTATE - the same "still in use" state a crash leaves.
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);
  fixture.Drain();
  const auto sizeBeforeAttempt = ReadFile(fixture.Path("binlog.000005")).size();

  auto &resumed = fixture.OpenSink(next);
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000003"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(resumed, rotate, StreamPosition{"binlog.000003", 0}));
  ASSERT_TRUE(resumed.HasFailed());
  EXPECT_EQ(resumed.LastError().failure, StorageFailure::Malformed);

  // Storage's last file is untouched, and the named file was never created.
  ASSERT_EQ(fixture.Catalog().Size(), 1u);
  EXPECT_TRUE(fixture.Catalog().At(0).inUse);
  fixture.Drain();
  const auto onDisk = ReadFile(fixture.Path("binlog.000005"));
  EXPECT_EQ(onDisk.size(), sizeBeforeAttempt);
  EXPECT_EQ(onDisk[IN_USE_FLAG_OFFSET], 0x01);
  fixture.Drain();
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000003")));
}

// The same refusal while this run holds the file open: the checks run
// before the file is closed, so a refused rotation leaves it in use and the
// next run resumes it instead of meeting "already exists".
TEST(StorageEventSinkTest,
     LeavesTheOpenFileInUseWhenItRefusesAnEarlierNumberedRotate) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000005").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));
  const std::uint32_t groupEnd = start + 125;
  fixture.Drain();
  const auto sizeBeforeAttempt = ReadFile(fixture.Path("binlog.000005")).size();

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000003"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"binlog.000005", groupEnd}));
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);

  ASSERT_EQ(fixture.Catalog().Size(), 1u);
  EXPECT_TRUE(fixture.Catalog().At(0).inUse);
  fixture.Drain();
  const auto onDisk = ReadFile(fixture.Path("binlog.000005"));
  EXPECT_EQ(onDisk.size(), sizeBeforeAttempt);
  EXPECT_EQ(onDisk[IN_USE_FLAG_OFFSET], 0x01);
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000003")));
}

// A different base name is a legitimate new sequence, not compared against
// storage's last file number; it takes the ordinary "source moved on" path.
TEST(StorageEventSinkTest,
     AllowsAnArtificialRotateNamingADifferentBaseEvenWithALowerApparentNumber) {
  StorageSinkFixture fixture;
  NullSink next;

  {
    auto &sink = fixture.OpenSink(next);
    const std::uint32_t start = OpenFreshFile(sink, "binlog.000005").afterPge;
    const DrivenGroup group = MakeGroup(start);
    ASSERT_TRUE(DriveGroup(sink, group, start));
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  auto &resumed = fixture.OpenSink(next);
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "mysql-bin.000001"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_TRUE(Drive(resumed, rotate, StreamPosition{"mysql-bin.000001", 0}));
  EXPECT_FALSE(resumed.HasFailed());
  EXPECT_FALSE(fixture.Catalog().At(0).inUse);  // binlog.000005 closed as
                                                // abandoned, the ordinary path
}

// Same Gtid/Query/Xid shape as MakeGroup(), but as protocol-framed wire
// bytes via ScriptedStreamBuilder, driving EventStreamReader itself.
void PushScriptedGroup(test::ScriptedStreamBuilder &builder,
                       std::uint64_t start, std::int64_t gno) {
  builder.PushEvent(static_cast<std::uint8_t>(EventType::Gtid),
                    GtidBody(gno, GROUP_TRANSACTION_LENGTH), 0,
                    static_cast<std::uint32_t>(start + 69));
  builder.PushEvent(
      2 /* Query - no EventType enumerator of its own, eEventType.hpp */,
      std::vector<std::uint8_t>(10, 0xAB), 0,
      static_cast<std::uint32_t>(start + 69 + 29));
  builder.PushEvent(16 /* Xid */, std::vector<std::uint8_t>(8, 0xCD), 0,
                    static_cast<std::uint32_t>(start + 69 + 29 + 27));
}

// The resume preamble every scenario below scripts identically, built
// against the file OpenFreshFile() already created on disk.
void PushScriptedResumePreamble(test::ScriptedStreamBuilder &builder,
                                const std::string &fileName) {
  builder.PushRotate(4, fileName, /*artificial=*/true, /*checksumLength=*/0);
  const auto fde = SampleFde();
  const std::uint32_t afterFde =
      4 + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + fde.size());
  // timestamp=1700000000 matches MakeEvent()'s default: the resume
  // comparison checks every Common-Header byte except flags/created, so
  // this must agree byte for byte, not just its body.
  builder.PushEvent(static_cast<std::uint8_t>(EventType::FormatDescription),
                    fde, /*checksumLength=*/0, afterFde,
                    /*flags=*/0, /*serverId=*/1, /*timestamp=*/1700000000);
  const auto pge = SamplePreviousGtids();
  const std::uint32_t afterPge =
      afterFde + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + pge.size());
  builder.PushEvent(static_cast<std::uint8_t>(EventType::PreviousGtids), pge,
                    /*checksumLength=*/0, afterPge,
                    /*flags=*/0, /*serverId=*/1, /*timestamp=*/1700000000);
}

// End-to-end: EventStreamReader's low-32-bit header check must agree with
// the 64-bit positions StorageEventSink trusts, past 4 GiB.
TEST(StorageEventSinkTest,
     ReaderAndStorageResumePastFourGiBAcrossAGroupBoundary) {
  StorageSinkFixture fixture;
  {
    NullSink next;
    auto &seed = fixture.OpenSink(next);
    OpenFreshFile(seed, "binlog.000001");
    // seed goes out of scope without a real ROTATE - still "in use".
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  constexpr std::uint64_t FOUR_GIB = std::uint64_t{1} << 32;
  const std::uint64_t writtenLength = FOUR_GIB - 50;
  fixture.Drain();
  ASSERT_EQ(::truncate(fixture.Path("binlog.000001").c_str(),
                       static_cast<off_t>(writtenLength)),
            0);
  std::string recoveredError;
  ASSERT_TRUE(fixture.Catalog().UpdateSize(writtenLength, recoveredError))
      << recoveredError;

  test::ScriptedStreamBuilder builder;
  PushScriptedResumePreamble(builder, "binlog.000001");
  builder.PushHeartbeatV2(
      "binlog.000001", writtenLength, /*checksumLength=*/0,
      /*nextPosition=*/static_cast<std::uint32_t>(writtenLength));
  PushScriptedGroup(builder, writtenLength, /*gno=*/1);
  PushScriptedGroup(builder, writtenLength + 125, /*gno=*/2);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  StreamReaderOptions options;
  options.checksumLength = 0;
  // The starting position is overwritten by the scripted artificial
  // ROTATE's own body before any check runs; its value is never observed.
  EventStreamReader reader(transport, sink, StreamPosition{"binlog.000001", 0},
                           options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(result.heartbeats, 1u);
  fixture.Drain();
  EXPECT_EQ(FileSize(fixture.Path("binlog.000001")), writtenLength + 250);
}

// The mirror scenario with no truncate: a heartbeat claiming a position near
// 4 GiB is a real gap, but StorageEventSink only sees it via the position
// before the *next* event, so the gap only reaches storage on the second
// heartbeat.
TEST(StorageEventSinkTest,
     ReaderAndStorageDetectAGapAfterAHeartbeatPastTheWrittenLength) {
  StorageSinkFixture fixture;
  {
    NullSink next;
    auto &seed = fixture.OpenSink(next);
    OpenFreshFile(seed, "binlog.000001");
    fixture.CloseSink();
    std::string recoveredError;
    fixture.Drain();
    ASSERT_TRUE(fixture.Catalog().UpdateSize(
        FileSize(fixture.Path(
            fixture.Catalog().At(fixture.Catalog().Size() - 1).name)),
        recoveredError))
        << recoveredError;
  }
  ASSERT_TRUE(fixture.Catalog().At(0).inUse);

  constexpr std::uint64_t FOUR_GIB = std::uint64_t{1} << 32;
  const std::uint64_t claimedPosition = FOUR_GIB - 50;

  test::ScriptedStreamBuilder builder;
  PushScriptedResumePreamble(builder, "binlog.000001");
  builder.PushHeartbeatV2(
      "binlog.000001", claimedPosition, /*checksumLength=*/0,
      /*nextPosition=*/static_cast<std::uint32_t>(claimedPosition));
  builder.PushHeartbeatV2(
      "binlog.000001", claimedPosition, /*checksumLength=*/0,
      /*nextPosition=*/static_cast<std::uint32_t>(claimedPosition));

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  NullSink next;
  auto &sink = fixture.OpenSink(next);
  StreamReaderOptions options;
  options.checksumLength = 0;
  EventStreamReader reader(transport, sink, StreamPosition{"binlog.000001", 0},
                           options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::StoppedBySink);
  ASSERT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::GapDetected);
  EXPECT_NE(sink.LastError().message.find("a heartbeat announced"),
            std::string::npos);
}

// Polls continuously for the whole run, not only once after each group, so
// a briefly mid-group published position would not go unnoticed. Every
// group has the same fixed length, so any non-multiple offset means mid-group.
TEST(StorageEventSinkTest, PublishedPositionNeverPointsInsideAnOpenGroup) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t headerEnd = OpenFreshFile(sink, "binlog.000001").afterPge;

  std::atomic<bool> stop{false};
  std::atomic<bool> sawMidGroup{false};
  std::thread watcher([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      const PublishedPosition published = fixture.Published().Current();
      if (published.fileName != "binlog.000001" ||
          published.position < headerEnd)
        continue;
      if ((published.position - headerEnd) % GROUP_TRANSACTION_LENGTH != 0)
        sawMidGroup.store(true, std::memory_order_relaxed);
    }
  });

  constexpr int GROUPS = 300;
  std::uint32_t cursor = headerEnd;
  for (int i = 0; i < GROUPS && !sawMidGroup.load(std::memory_order_relaxed);
       ++i) {
    const DrivenGroup group = MakeGroup(cursor, /*gno=*/i + 1);
    ASSERT_TRUE(DriveGroup(sink, group, cursor));
    cursor += static_cast<std::uint32_t>(GROUP_TRANSACTION_LENGTH);
  }

  stop.store(true, std::memory_order_relaxed);
  watcher.join();

  EXPECT_FALSE(sawMidGroup.load());
  EXPECT_EQ(fixture.Published().Current().fileName, "binlog.000001");
  EXPECT_EQ(fixture.Published().Current().position, cursor);
}

// The waiter's target is groupEnd; closing the file appends the ROTATE
// bytes before publishing the final size, so it ends up strictly past the
// target, still waking the waiter.
TEST(StorageEventSinkTest,
     ClosingTheFileWakesAWaiterHoldingItsLastGroupBoundaryAsTarget) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));
  const std::uint32_t groupEnd =
      start + static_cast<std::uint32_t>(GROUP_TRANSACTION_LENGTH);
  ASSERT_EQ(fixture.Published().Current().position, groupEnd);

  std::atomic<bool> started{false};
  WaitOutcome outcome = WaitOutcome::TimedOut;
  std::thread waiter([&] {
    started.store(true, std::memory_order_release);
    outcome = fixture.Published().Wait(
        PublishedPosition{"binlog.000001", groupEnd}, std::chrono::seconds(5));
  });
  while (!started.load(std::memory_order_acquire)) {
  }

  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), groupEnd + 30);
  ASSERT_TRUE(Drive(sink, rotate, StreamPosition{"binlog.000001", groupEnd}));
  waiter.join();

  EXPECT_EQ(outcome, WaitOutcome::Advanced);
}

// A waiter on the closed file's final position must wake once the next
// file's header becomes durable, not only once its first group completes -
// this scenario never writes a group into binlog.000002 before the wake.
TEST(StorageEventSinkTest,
     WritingTheNextFilesHeaderWakesAWaiterAtThePreviousFilesClosedEnd) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start);
  ASSERT_TRUE(DriveGroup(sink, group, start));
  const std::uint32_t groupEnd =
      start + static_cast<std::uint32_t>(GROUP_TRANSACTION_LENGTH);
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), groupEnd + 30);
  ASSERT_TRUE(Drive(sink, rotate, StreamPosition{"binlog.000001", groupEnd}));
  const PublishedPosition closedEnd = fixture.Published().Current();
  ASSERT_EQ(closedEnd.fileName, "binlog.000001");

  std::atomic<bool> started{false};
  WaitOutcome outcome = WaitOutcome::TimedOut;
  std::thread waiter([&] {
    started.store(true, std::memory_order_release);
    outcome = fixture.Published().Wait(closedEnd, std::chrono::seconds(5));
  });
  while (!started.load(std::memory_order_acquire)) {
  }

  OpenFreshFile(sink, "binlog.000002");  // no group written into it - the
                                         // header alone has to wake the waiter
  waiter.join();

  EXPECT_EQ(outcome, WaitOutcome::Advanced);
  EXPECT_EQ(fixture.Published().Current().fileName, "binlog.000002");
}

// The bytes a file holds once the whole header and `groups` groups of
// MakeGroup() shape have reached it - what every restart test below
// expects to find on disk, each event exactly once.
std::vector<std::uint8_t> ExpectedFile(const FreshFileHeader &header,
                                       const std::vector<DrivenGroup> &groups) {
  std::vector<std::uint8_t> expected = {0xfe, 0x62, 0x69, 0x6e};
  auto fdeOnDisk = header.fdeEventBytes;
  fdeOnDisk[IN_USE_FLAG_OFFSET - 4] |= 0x01;
  expected.insert(expected.end(), fdeOnDisk.begin(), fdeOnDisk.end());
  expected.insert(expected.end(), header.pgeEventBytes.begin(),
                  header.pgeEventBytes.end());
  for (const auto &group : groups) {
    expected.insert(expected.end(), group.gtid.bytes.begin(),
                    group.gtid.bytes.end());
    expected.insert(expected.end(), group.query.bytes.begin(),
                    group.query.bytes.end());
    expected.insert(expected.end(), group.xid.bytes.begin(),
                    group.xid.bytes.end());
  }
  return expected;
}

// A stream can end anywhere; the one that replaces it always starts over
// at the beginning of the file it resumes, so the events already stored
// arrive a second time and are compared, not written again.
TEST(StorageEventSinkTest, ANewStreamGoesOnWithTheFileTheLostOneLeftOpen) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const FreshFileHeader header = OpenFreshFile(sink, "binlog.000001");
  const std::uint32_t start = header.afterPge;
  const DrivenGroup first = MakeGroup(start, /*gno=*/1);
  ASSERT_TRUE(DriveGroup(sink, first, start));
  fixture.Drain();  // what the lost stream leaves behind on disk

  sink.RestartStream();

  const FreshFileHeader resent = OpenFreshFile(sink, "binlog.000001");
  ASSERT_EQ(resent.afterPge, header.afterPge);
  ASSERT_TRUE(DriveGroup(sink, first, start));
  const std::uint32_t secondStart =
      start + static_cast<std::uint32_t>(GROUP_TRANSACTION_LENGTH);
  const DrivenGroup second = MakeGroup(secondStart, /*gno=*/2);
  ASSERT_TRUE(DriveGroup(sink, second, secondStart));

  fixture.Drain();
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")),
            ExpectedFile(header, {first, second}));
  EXPECT_EQ(fixture.Published().Current().position,
            secondStart + GROUP_TRANSACTION_LENGTH);
}

// The transaction the lost stream was in the middle of is still open, and
// the source sends it again from its own first event.
TEST(StorageEventSinkTest, ATransactionCutInHalfIsFinishedByTheNewStream) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const FreshFileHeader header = OpenFreshFile(sink, "binlog.000001");
  const std::uint32_t start = header.afterPge;
  const DrivenGroup group = MakeGroup(start, /*gno=*/1);
  ASSERT_TRUE(Drive(sink, group.gtid, StreamPosition{"unused", start}));
  ASSERT_TRUE(Drive(sink, group.query, StreamPosition{"unused", start + 69}));
  fixture.Drain();
  // Nothing of an unfinished transaction is handed to a replica.
  ASSERT_EQ(fixture.Published().Current().position, start);

  sink.RestartStream();

  OpenFreshFile(sink, "binlog.000001");
  ASSERT_TRUE(DriveGroup(sink, group, start));

  fixture.Drain();
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")),
            ExpectedFile(header, {group}));
  EXPECT_EQ(fixture.Published().Current().position,
            start + GROUP_TRANSACTION_LENGTH);
}

// The bytes of one event can be cut in half as easily as a transaction:
// the first of them are stored, and the event arrives again whole.
TEST(StorageEventSinkTest, AnEventCutInHalfIsFinishedWhereItStopped) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const FreshFileHeader header = OpenFreshFile(sink, "binlog.000001");
  const std::uint32_t start = header.afterPge;
  const DrivenGroup group = MakeGroup(start, /*gno=*/1);
  ASSERT_TRUE(Drive(sink, group.gtid, StreamPosition{"unused", start}));
  ASSERT_TRUE(sink.OnEventBegin(group.query.header,
                                StreamPosition{"unused", start + 69}));
  ASSERT_TRUE(sink.OnEventBytes(
      std::span<const std::uint8_t>(group.query.bytes).first(12)));
  fixture.Drain();
  ASSERT_EQ(FileSize(fixture.Path("binlog.000001")), start + 69 + 12);

  sink.RestartStream();

  OpenFreshFile(sink, "binlog.000001");
  ASSERT_TRUE(DriveGroup(sink, group, start));

  fixture.Drain();
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")),
            ExpectedFile(header, {group}));
}

// Whatever the new stream sends, storage compares it against what it
// holds: a source that answers with something else is refused here as it
// is on a resume after a restart.
TEST(StorageEventSinkTest, ANewStreamSendingOtherBytesIsRefused) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  ASSERT_TRUE(DriveGroup(sink, MakeGroup(start, /*gno=*/1), start));
  fixture.Drain();

  sink.RestartStream();

  OpenFreshFile(sink, "binlog.000001");
  EXPECT_FALSE(DriveGroup(sink, MakeGroup(start, /*gno=*/7), start));
  EXPECT_TRUE(sink.HasFailed());
}

// A new stream beginning at another file means the source moved past the
// one storage holds - which it cannot have done with a transaction of it
// left unfinished.
TEST(StorageEventSinkTest,
     ANewStreamBeginningPastAnUnfinishedTransactionIsRefused) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const DrivenGroup group = MakeGroup(start, /*gno=*/1);
  ASSERT_TRUE(Drive(sink, group.gtid, StreamPosition{"unused", start}));
  fixture.Drain();

  sink.RestartStream();

  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000002"), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_FALSE(Drive(sink, rotate, StreamPosition{"binlog.000002", 0}));
  EXPECT_TRUE(sink.HasFailed());
}

// With no file open there is nothing to resume: the new stream opens the
// file it announces, exactly as a first stream would.
TEST(StorageEventSinkTest, ANewStreamAfterARotationOpensTheFileItAnnounces) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const std::uint32_t start = OpenFreshFile(sink, "binlog.000001").afterPge;
  const std::uint32_t groupEnd =
      start + static_cast<std::uint32_t>(GROUP_TRANSACTION_LENGTH);
  ASSERT_TRUE(DriveGroup(sink, MakeGroup(start, /*gno=*/1), start));
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), groupEnd + 30);
  ASSERT_TRUE(Drive(sink, rotate, StreamPosition{"binlog.000001", groupEnd}));
  fixture.Drain();

  sink.RestartStream();

  const FreshFileHeader header = OpenFreshFile(sink, "binlog.000002");
  const DrivenGroup group = MakeGroup(header.afterPge, /*gno=*/2);
  ASSERT_TRUE(DriveGroup(sink, group, header.afterPge));

  fixture.Drain();
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000002")),
            ExpectedFile(header, {group}));
  EXPECT_EQ(fixture.Catalog().Size(), 2u);
}

// The same event, but arriving in several chunks the way a large one
// does off the wire: once its remainder starts being appended, the
// chunks after it must go on being appended. Comparing them again would
// read back bytes this very event has just written, which the writer has
// not necessarily put on disk yet.
TEST(StorageEventSinkTest, AnEventCutInHalfGoesOnBeingAppendedChunkByChunk) {
  StorageSinkFixture fixture;
  NullSink next;
  auto &sink = fixture.OpenSink(next);

  const FreshFileHeader header = OpenFreshFile(sink, "binlog.000001");
  const std::uint32_t start = header.afterPge;
  DrivenGroup group = MakeGroup(start, /*gno=*/1);
  // Distinct body bytes, so a chunk compared at the wrong offset cannot
  // match by accident.
  std::vector<std::uint8_t> body(10);
  for (std::size_t i = 0; i < body.size(); ++i)
    body[i] = static_cast<std::uint8_t>(0x40 + i);
  group.query = MakeEvent(2 /* Query */, body,
                          static_cast<std::uint32_t>(start + 69 + 29));

  ASSERT_TRUE(Drive(sink, group.gtid, StreamPosition{"unused", start}));
  ASSERT_TRUE(sink.OnEventBegin(group.query.header,
                                StreamPosition{"unused", start + 69}));
  ASSERT_TRUE(sink.OnEventBytes(
      std::span<const std::uint8_t>(group.query.bytes).first(12)));
  fixture.Drain();
  ASSERT_EQ(FileSize(fixture.Path("binlog.000001")), start + 69 + 12);

  sink.RestartStream();

  OpenFreshFile(sink, "binlog.000001");
  ASSERT_TRUE(Drive(sink, group.gtid, StreamPosition{"unused", start}));
  ASSERT_TRUE(sink.OnEventBegin(group.query.header,
                                StreamPosition{"unused", start + 69}));
  const std::span<const std::uint8_t> resent(group.query.bytes);
  for (std::size_t offset = 0; offset < resent.size(); offset += 8)
    ASSERT_TRUE(sink.OnEventBytes(resent.subspan(
        offset, std::min<std::size_t>(8, resent.size() - offset))))
        << "chunk at " << offset;
  ASSERT_TRUE(sink.OnEventEnd());
  ASSERT_TRUE(
      Drive(sink, group.xid, StreamPosition{"unused", start + 69 + 29}));

  fixture.Drain();
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(ReadFile(fixture.Path("binlog.000001")),
            ExpectedFile(header, {group}));
  EXPECT_EQ(fixture.Published().Current().position,
            start + GROUP_TRANSACTION_LENGTH);
}

}  // namespace
}  // namespace binlog_streamer
