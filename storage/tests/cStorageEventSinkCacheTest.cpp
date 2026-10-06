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

#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "cStorageSinkFixture.hpp"
#include "cache/hCacheDefaults.hpp"
#include "storage/cBinlogStorage.hpp"
#include "storage/cStorageEventSink.hpp"
#include "storage/cStorageReader.hpp"
#include "storage/hStorageDefaults.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <numeric>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace binlog_streamer {
namespace {
constexpr auto S = CACHE_SEGMENT_SIZE;
class AcceptingSink : public EventSink {
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
  body.push_back(0x01);
  body.insert(body.end(), 4, 0x00);
  return body;
}

std::vector<std::uint8_t> SamplePreviousGtids() {
  std::vector<std::uint8_t> body = GtidSet().Encode(/*skipTaggedGtids=*/false);
  body.insert(body.end(), 4, 0x00);
  return body;
}

struct FreshFileHeader {
  std::uint32_t afterPge = 0;
  std::vector<std::uint8_t> fdeEventBytes;
  std::vector<std::uint8_t> pgeEventBytes;
};

FreshFileHeader OpenFreshFile(EventSink &sink, const std::string &fileName,
                              bool withChecksum = true) {
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, fileName), 0, EVENT_FLAG_ARTIFICIAL);
  EXPECT_TRUE(Drive(sink, rotate, StreamPosition{fileName, 0}));

  auto fde = SampleFde();
  if (!withChecksum) fde[57] = 0;
  const std::uint32_t afterFde =
      4 + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + fde.size());
  const auto fdeEvent = MakeEvent(
      static_cast<std::uint8_t>(EventType::FormatDescription), fde, afterFde);
  EXPECT_TRUE(Drive(sink, fdeEvent, StreamPosition{fileName, 4}));

  const auto pge =
      withChecksum ? SamplePreviousGtids() : GtidSet().Encode(false);
  const std::uint32_t afterPge =
      afterFde + static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + pge.size());
  const auto pgeEvent = MakeEvent(
      static_cast<std::uint8_t>(EventType::PreviousGtids), pge, afterPge);
  EXPECT_TRUE(Drive(sink, pgeEvent, StreamPosition{fileName, afterFde}));

  return FreshFileHeader{afterPge, fdeEvent.bytes, pgeEvent.bytes};
}

std::vector<std::uint8_t> DiskBytes(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return {};
  std::vector<std::uint8_t> out(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(out.data()),
            static_cast<std::streamsize>(out.size()));
  return out;
}

bool WithinDeadline(test::StorageSinkFixture &fixture,
                    const std::function<bool()> &operation) {
  auto task = std::async(std::launch::async, operation);
  if (task.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    fixture.Stop();
    fixture.Writer().Stop();
    ADD_FAILURE() << "sink or writer did not finish within five seconds";
    task.wait();
    return false;
  }
  return task.get();
}

std::vector<std::uint8_t> ReadAll(StorageReader &reader,
                                  const FileCursor &cursor, std::uint64_t start,
                                  std::size_t length, std::string &error) {
  std::vector<std::uint8_t> out(length);
  std::size_t done = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (done < length) {
    const auto n = reader.Read(
        cursor, start + done,
        std::span(out).subspan(done, std::min(S, length - done)), error);
    if (!error.empty()) return {};
    done += n;
    if (n == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        error = "reader stalled";
        return {};
      }
      reader.WaitForNewEvents({cursor.FileName(), start + done},
                              std::chrono::milliseconds(5),
                              WaitStyle::PollFirst);
    }
  }
  return out;
}

std::vector<WireEvent> Group(std::uint64_t start, std::size_t queryBodySize,
                             std::int64_t gno = 1) {
  std::size_t total = 69 + EVENT_HEADER_LENGTH + queryBodySize;
  for (int i = 0; i < 3; ++i)
    total = EVENT_HEADER_LENGTH + GtidBody(gno, total).size() +
            EVENT_HEADER_LENGTH + queryBodySize;
  auto gtid = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                        GtidBody(gno, total), 0);
  auto query = MakeEvent(2, std::vector<std::uint8_t>(queryBodySize, 0x93),
                         static_cast<std::uint32_t>(start + total));
  return {std::move(gtid), std::move(query)};
}

bool DrivePortions(EventSink &sink, const std::vector<WireEvent> &group,
                   std::uint64_t offset) {
  for (const auto &event : group) {
    if (!sink.OnEventBegin(event.header, {"binlog.000001", offset}))
      return false;
    for (std::size_t done = 0; done < event.bytes.size();) {
      const auto portion =
          std::span(event.bytes)
              .subspan(done, std::min(S, event.bytes.size() - done));
      if (!sink.OnEventBytes(portion)) return false;
      done += portion.size();
    }
    if (!sink.OnEventEnd()) return false;
    offset += event.bytes.size();
  }
  return true;
}

TEST(StorageEventSinkCacheTest,
     RegistersCacheBeforeCatalogAndCachesTheHeaderAcrossRotation) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  std::string error;
  std::size_t beginCalls = 0;
  test::SinkOptions options;
  options.cacheSize = 2 * S;
  options.cacheHooks.afterBeginFile = [&] {
    EXPECT_EQ(fixture.Catalog().Size(), beginCalls);
    ++beginCalls;
  };
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  fixture.Drain();
  auto disk = DiskBytes(fixture.Path("binlog.000001"));
  std::vector<std::uint8_t> cached(header.afterPge);
  ASSERT_EQ(std::get<CacheReadResult::Copied>(
                fixture.Cache().Read("binlog.000001", 0, cached).value)
                .n,
            cached.size());
  EXPECT_EQ(cached, disk);
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), 0);
  ASSERT_TRUE(Drive(sink, rotate, {"binlog.000001", header.afterPge}));
  OpenFreshFile(sink, "binlog.000002");
  EXPECT_EQ(beginCalls, 2u);
  fixture.Drain();
  disk = DiskBytes(fixture.Path("binlog.000001"));
  disk.resize(header.afterPge);
  ASSERT_EQ(std::get<CacheReadResult::Copied>(
                fixture.Cache().Read("binlog.000001", 0, cached).value)
                .n,
            cached.size());
  EXPECT_EQ(cached[IN_USE_FLAG_OFFSET] ^ disk[IN_USE_FLAG_OFFSET],
            EVENT_FLAG_BINLOG_IN_USE);
  EXPECT_NE(cached[IN_USE_FLAG_OFFSET] & EVENT_FLAG_BINLOG_IN_USE, 0);
  cached[IN_USE_FLAG_OFFSET] = disk[IN_USE_FLAG_OFFSET];
  EXPECT_EQ(cached, disk);
}

TEST(StorageEventSinkCacheTest,
     SplitsLargeBufferedGtidAndStopsWithoutStorageFailure) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  options.cacheSize = 2 * S;
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  auto body = GtidBody(1, S + 113);
  body.resize(S + 113 - EVENT_HEADER_LENGTH, 0x35);
  const auto event = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid), body,
                               header.afterPge + S + 113);
  ASSERT_TRUE(WithinDeadline(fixture, [&] {
    return Drive(sink, event, {"binlog.000001", header.afterPge});
  }));
  EXPECT_EQ(fixture.Cache().Counters().appended,
            header.afterPge + event.bytes.size());
  std::vector<std::uint8_t> out(event.bytes.size());
  EXPECT_EQ(
      std::get<CacheReadResult::Copied>(
          fixture.Cache().Read("binlog.000001", header.afterPge, out).value)
          .n,
      out.size());
  EXPECT_EQ(out, event.bytes);
  fixture.Drain();
  const auto query = MakeEvent(2, std::vector<std::uint8_t>(10, 0x71),
                               header.afterPge + S + 142);
  ASSERT_TRUE(sink.OnEventBegin(query.header,
                                {"binlog.000001", header.afterPge + S + 113}));
  const auto before = fixture.Cache().Counters();
  fixture.Stop();
  EXPECT_FALSE(sink.OnEventBytes(query.bytes));
  EXPECT_FALSE(sink.HasFailed());
  EXPECT_EQ(fixture.Cache().Counters(), before);
}

TEST(StorageEventSinkCacheTest,
     PublishesAndReadsTheEntireFileBeforeWriterStarts) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  options.startWriter = false;
  auto &sink = fixture.OpenSink(next, nullptr, options);
  std::uint64_t end = 0;
  ASSERT_TRUE(WithinDeadline(fixture, [&] {
    const auto header = OpenFreshFile(sink, "binlog.000001");
    const auto group = Group(header.afterPge, 37);
    end = header.afterPge + group[0].bytes.size() + group[1].bytes.size();
    return DrivePortions(sink, group, header.afterPge);
  }));
  EXPECT_EQ(fixture.Published().Current().position, end);
  EXPECT_FALSE(fixture.Catalog().At(0).onDisk);
  EXPECT_FALSE(std::filesystem::exists(fixture.Path("binlog.000001")));
  StorageReader reader(fixture.Directory(), fixture.Catalog(),
                       fixture.Published(), &fixture.Cache());
  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;
  const auto bytes = ReadAll(reader, *cursor, 0, end, error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(reader.Counters().readFromCache, end);
  EXPECT_EQ(reader.Counters().readFromDisk, 0u);
  fixture.StartWriter();
  fixture.Drain();
  EXPECT_EQ(bytes, DiskBytes(fixture.Path("binlog.000001")));
  EXPECT_TRUE(fixture.Catalog().At(0).onDisk);
}

TEST(StorageEventSinkCacheTest,
     StreamsAThirtyTwoMiBGroupThroughFourSlotsWithoutPublishingInsideIt) {
  test::StorageSinkFixture fixture;
  class WatchingSink : public AcceptingSink {
   public:
    PublishedPositionTracker *published = nullptr;
    std::uint64_t boundary = 0;
    bool valid = true;
    bool OnEventBegin(const EventHeader &, const StreamPosition &) override {
      if (boundary) valid = valid && published->Current().position == boundary;
      return true;
    }
  } next;
  next.published = &fixture.Published();
  test::SinkOptions options;
  options.cacheSize = 4 * S;
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  next.boundary = header.afterPge;
  const auto group = Group(header.afterPge, 32 * S - 96);
  ASSERT_TRUE(WithinDeadline(
      fixture, [&] { return DrivePortions(sink, group, header.afterPge); }));
  EXPECT_TRUE(next.valid);
  EXPECT_EQ(fixture.Published().Current().position, header.afterPge + 32 * S);
  EXPECT_LE(fixture.Cache().Counters().occupied, 4u);
  StorageReader reader(fixture.Directory(), fixture.Catalog(),
                       fixture.Published(), &fixture.Cache());
  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;
  auto bytes = ReadAll(reader, *cursor, header.afterPge, 32 * S, error);
  ASSERT_TRUE(error.empty()) << error;
  std::vector<std::uint8_t> expected = group[0].bytes;
  expected.insert(expected.end(), group[1].bytes.begin(), group[1].bytes.end());
  EXPECT_EQ(bytes, expected);
  EXPECT_GT(reader.Counters().readFromDisk, 0u);
  EXPECT_GT(reader.Counters().readFromCache, 0u);
  EXPECT_GT(reader.Counters().seamCrossings, 0u);
}

TEST(StorageEventSinkCacheTest, ReportsBackgroundFailureAtTheNextEvent) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  options.writerHooks.beforeWrite = [](std::string &error) {
    error = "injected body failure";
    return false;
  };
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  const auto group = Group(header.afterPge, 37);
  // Avoid a scheduling race between the two event beginnings and the failure.
  auto first = group[0];
  ASSERT_TRUE(Drive(sink, first, {"binlog.000001", header.afterPge}));
  fixture.Writer().Wake();
  ASSERT_TRUE(WithinDeadline(fixture, [&] {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!fixture.Writer().Failed() && std::chrono::steady_clock::now() < end)
      std::this_thread::yield();
    return fixture.Writer().Failed();
  }));
  EXPECT_FALSE(sink.OnEventBegin(
      group[1].header,
      {"binlog.000001", header.afterPge + first.bytes.size()}));
  EXPECT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::WriteFailed);
  EXPECT_NE(sink.LastError().message.find("injected body failure"),
            std::string::npos);
}

TEST(StorageEventSinkCacheTest,
     AbortsAWaitingSinkAsWriteFailedRatherThanCleanStop) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  options.cacheSize = 2 * S;
  std::binary_semaphore writing{0}, release{0};
  options.writerHooks.beforeWrite = [&](std::string &error) {
    writing.release();
    if (!release.try_acquire_for(std::chrono::seconds(5)))
      error = "failure hook timed out";
    else
      error = "injected waiting failure";
    return false;
  };
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  const auto group = Group(header.afterPge, 4 * S);
  auto producer = std::async(std::launch::async, [&] {
    return DrivePortions(sink, group, header.afterPge);
  });
  EXPECT_TRUE(writing.try_acquire_for(std::chrono::seconds(5)));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (fixture.Cache().Counters().spaceWaits == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_GT(fixture.Cache().Counters().spaceWaits, 0u);
  release.release();
  const bool ready =
      producer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (!ready) {
    fixture.Stop();
    fixture.Writer().Stop();
  }
  ASSERT_TRUE(ready) << "failed writer did not release the blocked sink";
  EXPECT_FALSE(producer.get());
  EXPECT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::WriteFailed);
  EXPECT_NE(sink.LastError().message.find("injected waiting failure"),
            std::string::npos);
}

TEST(StorageEventSinkCacheTest,
     DrainsAcceptedOpenGroupBytesEvenAfterMalformedInput) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  std::binary_semaphore sleeping{0};
  std::atomic<bool> observeSleep{false};
  options.writerHooks.beforeSleep = [&] {
    if (observeSleep.exchange(false)) sleeping.release();
  };
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  fixture.Drain();
  observeSleep.store(true);
  fixture.Writer().Wake();
  ASSERT_TRUE(sleeping.try_acquire_for(std::chrono::seconds(5)));
  // This incomplete group never wakes the sleeping writer. Only shutdown's
  // drain can write it; merely stopping the worker must lose these bytes.
  const auto group = Group(header.afterPge, 37);
  ASSERT_TRUE(Drive(sink, group[0], {"binlog.000001", header.afterPge}));
  const auto rotate = MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                                RotateBody(4, "binlog.000002"), 0);
  EXPECT_FALSE(sink.OnEventBegin(
      rotate.header,
      {"binlog.000001", header.afterPge + group[0].bytes.size()}));
  EXPECT_TRUE(sink.HasFailed());
  EXPECT_EQ(sink.LastError().failure, StorageFailure::Malformed);
  fixture.CloseSink();
  fixture.Drain();
  EXPECT_EQ(DiskBytes(fixture.Path("binlog.000001")).size(),
            header.afterPge + group[0].bytes.size());
}

TEST(StorageEventSinkCacheTest,
     StopsAWaitingSinkWithinOneHundredFiftyMillisecondsWithoutFailure) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  options.startWriter = false;
  options.cacheSize = 2 * S;
  auto &sink = fixture.OpenSink(next, nullptr, options);
  const auto header = OpenFreshFile(sink, "binlog.000001");
  const auto group = Group(header.afterPge, 4 * S);
  auto producer = std::async(std::launch::async, [&] {
    return DrivePortions(sink, group, header.afterPge);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (fixture.Cache().Counters().spaceWaits == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_GT(fixture.Cache().Counters().spaceWaits, 0u);
  const auto start = std::chrono::steady_clock::now();
  fixture.Stop();
  const bool ready = producer.wait_for(std::chrono::milliseconds(150)) ==
                     std::future_status::ready;
  if (!ready) fixture.Cache().Abort();
  ASSERT_TRUE(ready) << "stop was not observed within 150 ms";
  EXPECT_FALSE(producer.get());
  EXPECT_LT(std::chrono::steady_clock::now() - start,
            std::chrono::milliseconds(150));
  EXPECT_FALSE(sink.HasFailed());
}

TEST(StorageEventSinkCacheTest,
     StreamsAHeaderLargerThanTheCacheWithoutRewritingItsDiskPrefix) {
  test::StorageSinkFixture fixture;
  AcceptingSink next;
  test::SinkOptions options;
  options.cacheSize = 2 * S;
  auto &sink = fixture.OpenSink(next, nullptr, options);
  std::vector<std::uint8_t> pge;
  constexpr std::uint64_t SOURCES = 80000;
  AppendLittleEndian(pge, SOURCES, 8);
  for (std::uint64_t i = 1; i <= SOURCES; ++i) {
    AppendLittleEndian(pge, i, 8);
    AppendLittleEndian(pge, 0, 8);
    AppendLittleEndian(pge, 1, 8);
    AppendLittleEndian(pge, 1, 8);
    AppendLittleEndian(pge, 2, 8);
  }
  pge.insert(pge.end(), 4, 0);
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, "binlog.000001"), 0, EVENT_FLAG_ARTIFICIAL);
  const auto fde = MakeEvent(
      static_cast<std::uint8_t>(EventType::FormatDescription), SampleFde(), 85);
  const auto previous = MakeEvent(
      static_cast<std::uint8_t>(EventType::PreviousGtids), pge,
      static_cast<std::uint32_t>(85 + EVENT_HEADER_LENGTH + pge.size()));
  const auto headerEnd = 4 + fde.bytes.size() + previous.bytes.size();
  const auto group = Group(headerEnd, 37);
  ASSERT_TRUE(WithinDeadline(fixture, [&] {
    return Drive(sink, rotate, {"binlog.000001", 0}) &&
           Drive(sink, fde, {"binlog.000001", 4}) &&
           Drive(sink, previous, {"binlog.000001", 85}) &&
           DrivePortions(sink, group, headerEnd);
  }));
  fixture.Drain();
  StorageReader reader(fixture.Directory(), fixture.Catalog(),
                       fixture.Published(), &fixture.Cache());
  std::string error;
  auto cursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(cursor) << error;
  const auto total = fixture.Published().Current().position;
  auto bytes = ReadAll(reader, *cursor, 0, total, error);
  ASSERT_TRUE(error.empty()) << error;
  fixture.Drain();
  EXPECT_EQ(bytes, DiskBytes(fixture.Path("binlog.000001")));
  EXPECT_GT(reader.Counters().readFromCache, 0u);
  EXPECT_GT(reader.Counters().readFromDisk, 0u);
}

TEST(StorageEventSinkCacheTest,
     EightReadersCrossTheRecoveredDiskPrefixIntoNewCachedBytes) {
  test::TempDirectoryFixture directory;
  AcceptingSink next;
  std::string error;
  StorageOpenFailure failure{};
  std::uint64_t recoveredLength = 0;
  {
    std::atomic<bool> stop{false};
    BinlogStorage storage;
    ASSERT_TRUE(storage.Open(directory.Directory(), failure, error)) << error;
    ASSERT_TRUE(storage.ReserveCache(4 * S, std::chrono::seconds(0), stop,
                                     failure, error))
        << error;
    StorageWriter writer(directory.Directory(), *storage.Cache(),
                         storage.Catalog());
    StorageEventSink sink(directory.Directory(), 0, next, storage.Catalog(),
                          *storage.Cache(), writer, &storage.Published());
    writer.Start();
    const auto header = OpenFreshFile(sink, "binlog.000001", false);
    ASSERT_TRUE(
        DrivePortions(sink, Group(header.afterPge, S), header.afterPge));
    ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
    recoveredLength = storage.Published().Current().position;
  }
  const auto oldBytes = DiskBytes(directory.Path("binlog.000001"));
  ASSERT_EQ(oldBytes.size(), recoveredLength);
  std::atomic<bool> stop{false};
  BinlogStorage storage;
  StorageStartState state;
  ASSERT_TRUE(storage.OpenResumed(directory.Directory(), state, failure, error))
      << error;
  storage.SeedPublished(state);
  ASSERT_EQ(storage.Published().Current().position, recoveredLength);
  ASSERT_TRUE(storage.ReserveCache(4 * S, std::chrono::seconds(0), stop,
                                   failure, error))
      << error;
  StorageWriter writer(directory.Directory(), *storage.Cache(),
                       storage.Catalog());
  StorageEventSink sink(directory.Directory(), 0, next, storage.Catalog(),
                        *storage.Cache(), writer, &storage.Published());
  writer.Start();
  OpenFreshFile(sink, "binlog.000001", false);
  const auto group = Group(recoveredLength, S, 2);
  std::vector<std::uint8_t> expected = oldBytes;
  for (const auto &event : group)
    expected.insert(expected.end(), event.bytes.begin(), event.bytes.end());
  const auto hash = [](std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) {
      value ^= byte;
      value *= 1099511628211ULL;
    }
    return value;
  };
  std::vector<std::unique_ptr<StorageReader>> readers;
  std::vector<std::unique_ptr<FileCursor>> cursors;
  for (std::size_t i = 0; i < 8; ++i) {
    readers.push_back(std::make_unique<StorageReader>(
        directory.Directory(), storage.Catalog(), storage.Published(),
        storage.Cache()));
    auto cursor = readers.back()->Open("binlog.000001", error);
    ASSERT_TRUE(cursor) << error;
    cursors.push_back(std::make_unique<FileCursor>(std::move(*cursor)));
  }
  std::vector<std::future<bool>> tasks;
  for (std::size_t i = 0; i < 8; ++i)
    tasks.push_back(std::async(std::launch::async, [&, i] {
      std::string readError;
      const auto offset = i * 73;
      const auto bytes = ReadAll(*readers[i], *cursors[i], offset,
                                 expected.size() - offset, readError);
      const auto reference = std::span(expected).subspan(offset);
      EXPECT_TRUE(readError.empty()) << readError;
      EXPECT_EQ(bytes.size(), reference.size());
      EXPECT_EQ(std::accumulate(bytes.begin(), bytes.end(), std::uint64_t{0}),
                std::accumulate(reference.begin(), reference.end(),
                                std::uint64_t{0}));
      EXPECT_EQ(hash(bytes), hash(reference));
      EXPECT_GT(readers[i]->Counters().readFromDisk, 0u);
      EXPECT_GT(readers[i]->Counters().readFromCache, 0u);
      EXPECT_GT(readers[i]->Counters().seamCrossings, 0u);
      return readError.empty() && bytes.size() == reference.size();
    }));
  auto producer = std::async(std::launch::async, [&] {
    return DrivePortions(sink, group, recoveredLength);
  });
  const bool ready =
      producer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (!ready) {
    stop.store(true);
    writer.Stop();
  }
  EXPECT_TRUE(ready) << "resumed producer stalled";
  EXPECT_TRUE(producer.get());
  for (auto &task : tasks) {
    EXPECT_EQ(task.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_TRUE(task.get());
  }
  ASSERT_TRUE(writer.DrainAndSync()) << writer.LastError();
  EXPECT_EQ(DiskBytes(directory.Path("binlog.000001")), expected);
}

TEST(StorageEventSinkCacheTest,
     ReportsSpacePressureOnceOutsideCacheAndWriterLocks) {
  test::TempDirectoryFixture directory;
  std::atomic<bool> stop{false};
  StorageCatalog catalog;
  std::string error;
  auto cache = EventCache::Reserve(2 * S, stop, error);
  ASSERT_TRUE(cache) << error;
  std::size_t reports = 0;
  std::thread::id callbackThread;
  StorageWriter *activeWriter = nullptr;
  StorageWriter writer(directory.Directory(), *cache, catalog, {}, [&] {
    callbackThread = std::this_thread::get_id();
    ++reports;
    (void)cache->Counters();
    activeWriter->Wake();
  });
  activeWriter = &writer;
  AcceptingSink next;
  StorageEventSink sink(directory.Directory(), 0, next, catalog, *cache,
                        writer);
  writer.Start();
  const auto header = OpenFreshFile(sink, "binlog.000001");
  auto producer = std::async(std::launch::async, [&] {
    const auto id = std::this_thread::get_id();
    const bool result =
        DrivePortions(sink, Group(header.afterPge, 16 * S), header.afterPge);
    EXPECT_EQ(callbackThread, id);
    return result;
  });
  const bool ready =
      producer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (!ready) {
    stop.store(true);
    writer.Stop();
  }
  ASSERT_TRUE(ready) << "pressure callback deadlocked";
  ASSERT_TRUE(producer.get());
  EXPECT_GT(cache->Counters().spaceWaits, 1u);
  EXPECT_EQ(reports, 1u);
  ASSERT_TRUE(writer.DrainAndSync());
}

}  // namespace
}  // namespace binlog_streamer
