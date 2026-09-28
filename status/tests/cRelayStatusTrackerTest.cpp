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

#include "status/cRelayStatusTracker.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <functional>
#include <map>

namespace binlog_streamer {
namespace {

// Two files of known sizes, the second one published up to a point.
class FakeStorageFacts : public StorageFacts {
 public:
  std::size_t Files() const override { return sizes.size(); }
  std::uint64_t Bytes() const override {
    std::uint64_t total = 0;
    for (const auto &[name, size] : sizes) total += size;
    return total;
  }
  std::uint64_t MaxBytes() const override { return 4000; }
  MemoryStatus Memory() const override { return {2048, 1024, 1}; }
  void Published(std::string &file, std::uint64_t &position) const override {
    file = publishedFile;
    position = publishedPosition;
    if (afterPublished) afterPublished();
  }
  std::optional<std::uint64_t> Offset(const std::string &file,
                                      std::uint64_t position) const override {
    if (!sizes.contains(file)) return std::nullopt;
    std::uint64_t before = 0;
    for (const auto &[name, size] : sizes) {
      if (name == file) break;
      before += size;
    }
    return before + position;
  }
  std::optional<std::uint64_t> Distance(
      const std::string &fromFile, std::uint64_t fromPosition,
      const std::string &toFile, std::uint64_t toPosition) const override {
    const auto from = Offset(fromFile, fromPosition);
    const auto to = Offset(toFile, toPosition);
    if (!from || !to || *to < *from) return std::nullopt;
    return *to - *from;
  }

  std::map<std::string, std::uint64_t> sizes{{"binlog.000001", 1000},
                                             {"binlog.000002", 600}};
  std::string publishedFile = "binlog.000002";
  std::uint64_t publishedPosition = 400;
  // Runs once the published position has been read: what moves on here
  // moves between the status's reads.
  std::function<void()> afterPublished;
};

std::uint64_t NowSeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TEST(RelayStatusTrackerTest, StartsWithNothingKnown) {
  RelayStatusTracker tracker("relay", "1.0", 10, nullptr);
  const RelayStatus status = tracker.Snapshot();
  EXPECT_EQ(status.name, "relay");
  EXPECT_EQ(status.version, "1.0");
  EXPECT_EQ(status.state, RelayState::Starting);
  EXPECT_LE(status.startedAt, status.now);
  EXPECT_LE(status.now, NowSeconds());
  EXPECT_LE(status.uptimeSeconds, 1u);
  EXPECT_EQ(status.maxConnections, 10u);
  EXPECT_FALSE(status.source.connected);
  EXPECT_FALSE(status.source.since.has_value());
  EXPECT_FALSE(status.source.clock.has_value());
  EXPECT_FALSE(status.source.behindSeconds.has_value());
  EXPECT_EQ(status.storage.files, 0u);
  EXPECT_EQ(status.storage.maxBytes, 0u);
  EXPECT_EQ(status.memory.maxBytes, 0u);
  EXPECT_FALSE(status.storage.behindBytes.has_value());
  EXPECT_TRUE(status.replicas.empty());
}

TEST(RelayStatusTrackerTest, SourceGoesThroughAttemptsConnectionAndLoss) {
  FakeStorageFacts storage;
  RelayStatusTracker tracker("relay", "1.0", 10, &storage);
  tracker.SetSourceAddress("source.example:3306");
  tracker.SourceAttempt(3);
  RelayStatus status = tracker.Snapshot();
  EXPECT_EQ(status.state, RelayState::Starting);
  EXPECT_EQ(status.source.address, "source.example:3306");
  EXPECT_EQ(status.source.attempt, 3u);

  const std::uint64_t sourceClock = 1'700'000'100;
  tracker.SourceConnected(7, "uuid-7", "8.4.11", true, "zstd", sourceClock);
  tracker.Source().SetFile("binlog.000002", 4);
  tracker.Source().Advance(500, sourceClock - 30);
  status = tracker.Snapshot();
  EXPECT_EQ(status.state, RelayState::Serving);
  EXPECT_TRUE(status.source.connected);
  EXPECT_EQ(status.source.attempt, 0u);
  EXPECT_EQ(status.source.serverId, 7u);
  EXPECT_EQ(status.source.serverUuid, "uuid-7");
  EXPECT_EQ(status.source.version, "8.4.11");
  EXPECT_TRUE(status.source.tls);
  EXPECT_EQ(status.source.compression, "zstd");
  ASSERT_TRUE(status.source.since.has_value());
  EXPECT_LE(*status.source.since, NowSeconds());
  ASSERT_TRUE(status.source.clock.has_value());
  EXPECT_GE(*status.source.clock, sourceClock);
  EXPECT_LE(*status.source.clock, sourceClock + 1);
  EXPECT_EQ(status.source.seen.file, "binlog.000002");
  EXPECT_EQ(status.source.seen.position, 500u);
  ASSERT_TRUE(status.source.behindSeconds.has_value());
  EXPECT_GE(*status.source.behindSeconds, 30u);
  EXPECT_LE(*status.source.behindSeconds, 31u);
  // Received up to 500, published up to 400: 100 bytes not yet published.
  EXPECT_EQ(status.storage.files, 2u);
  EXPECT_EQ(status.storage.bytes, 1600u);
  EXPECT_EQ(status.storage.maxBytes, 4000u);
  EXPECT_EQ(status.memory.maxBytes, 2048u);
  EXPECT_EQ(status.memory.bytes, 1024u);
  EXPECT_EQ(status.memory.files, 1u);
  EXPECT_EQ(status.storage.file, "binlog.000002");
  EXPECT_EQ(status.storage.position, 400u);
  EXPECT_EQ(status.storage.behindBytes, std::optional<std::uint64_t>(100));

  // A heartbeat: the source has nothing more, so the lag is zero whatever
  // the last event's timestamp was.
  tracker.Source().Idle(500);
  status = tracker.Snapshot();
  EXPECT_EQ(status.source.behindSeconds, std::optional<std::uint64_t>(0));
  EXPECT_TRUE(status.source.seen.idle);

  // A file storage does not hold yet: the received position cannot be
  // placed, and the byte lag is unknown rather than wrong.
  tracker.Source().SetFile("binlog.000003", 4);
  status = tracker.Snapshot();
  EXPECT_FALSE(status.storage.behindBytes.has_value());

  tracker.SourceLost();
  status = tracker.Snapshot();
  EXPECT_EQ(status.state, RelayState::Reconnecting);
  EXPECT_FALSE(status.source.connected);
  EXPECT_FALSE(status.source.clock.has_value());
  EXPECT_FALSE(status.source.behindSeconds.has_value());
  // What the source reported about itself stays, and so does where the
  // stream stood.
  EXPECT_EQ(status.source.serverUuid, "uuid-7");
  EXPECT_EQ(status.source.seen.file, "binlog.000003");
}

TEST(RelayStatusTrackerTest, DiskBytesCountTheOpenFileAsReceived) {
  // The catalog knows the open file at 100 bytes; 500 have been received
  // into it, so 1500 are on disk, not 1100.
  FakeStorageFacts storage;
  storage.sizes["binlog.000002"] = 100;
  RelayStatusTracker tracker("relay", "1.0", 10, &storage);
  tracker.SourceConnected(7, "uuid-7", "8.4.11", false, "none", 1'700'000'100);
  tracker.Source().SetFile("binlog.000002", 4);
  tracker.Source().Advance(500, 1'700'000'090);
  EXPECT_EQ(tracker.Snapshot().storage.bytes, 1500u);
  // A closed file's size stands; nothing received past it changes it.
  tracker.Source().SetFile("binlog.000001", 4);
  tracker.Source().Advance(10, 1'700'000'090);
  EXPECT_EQ(tracker.Snapshot().storage.bytes, 1100u);
}

TEST(RelayStatusTrackerTest, ReplicasAreListedWhileRegistered) {
  FakeStorageFacts storage;
  RelayStatusTracker tracker("relay", "1.0", 10, &storage);
  const std::uint64_t sourceClock = 1'700'000'100;
  tracker.SourceConnected(7, "uuid-7", "8.4.11", false, "none", sourceClock);

  auto first = tracker.RegisterReplica(
      ReplicaFacts{"10.0.0.1:41000", "replica1", true, "zstd", "", ""});
  auto second = tracker.RegisterReplica(
      ReplicaFacts{"10.0.0.2:41001", "replica2", false, "none", "", ""});
  RelayStatus status = tracker.Snapshot();
  ASSERT_EQ(status.replicas.size(), 2u);
  EXPECT_EQ(status.replicas[0].facts.address, "10.0.0.1:41000");
  EXPECT_EQ(status.replicas[0].facts.user, "replica1");
  EXPECT_TRUE(status.replicas[0].facts.tls);
  EXPECT_EQ(status.replicas[0].facts.compression, "zstd");
  EXPECT_LE(status.replicas[0].since, NowSeconds());
  EXPECT_EQ(status.replicas[0].state, ReplicaState::Connected);
  EXPECT_EQ(status.replicas[0].reportHost, "");
  EXPECT_FALSE(status.replicas[0].behindBytes.has_value());

  first->SetReportHost("replica1.example");
  EXPECT_EQ(tracker.Snapshot().replicas[0].reportHost, "replica1.example");
  EXPECT_FALSE(status.replicas[0].behindSeconds.has_value());

  // Catching up from the first file: 1000 + 400 published, 700 sent.
  first->SetDumping(true);
  first->Progress().SetFile("binlog.000001", 4);
  first->Progress().Advance(700, sourceClock - 3600);
  status = tracker.Snapshot();
  EXPECT_EQ(status.replicas[0].state, ReplicaState::CatchingUp);
  EXPECT_EQ(status.replicas[0].sent.file, "binlog.000001");
  EXPECT_EQ(status.replicas[0].sent.position, 700u);
  EXPECT_EQ(status.replicas[0].behindBytes, std::optional<std::uint64_t>(700));
  ASSERT_TRUE(status.replicas[0].behindSeconds.has_value());
  EXPECT_GE(*status.replicas[0].behindSeconds, 3600u);
  EXPECT_LE(*status.replicas[0].behindSeconds, 3601u);

  // Sent everything published and waiting: streaming, nothing behind.
  first->Progress().SetFile("binlog.000002", 4);
  first->Progress().Advance(400, sourceClock - 1);
  first->Progress().Idle(400);
  status = tracker.Snapshot();
  EXPECT_EQ(status.replicas[0].state, ReplicaState::Streaming);
  EXPECT_EQ(status.replicas[0].behindBytes, std::optional<std::uint64_t>(0));
  EXPECT_EQ(status.replicas[0].behindSeconds, std::optional<std::uint64_t>(0));

  // A file purged from under the replica: the byte lag is unknown.
  storage.sizes.erase("binlog.000001");
  second->SetDumping(true);
  second->Progress().SetFile("binlog.000001", 4);
  second->Progress().Advance(50, sourceClock - 10);
  status = tracker.Snapshot();
  EXPECT_FALSE(status.replicas[1].behindBytes.has_value());
  EXPECT_TRUE(status.replicas[1].behindSeconds.has_value());

  tracker.UnregisterReplica(first);
  status = tracker.Snapshot();
  ASSERT_EQ(status.replicas.size(), 1u);
  EXPECT_EQ(status.replicas[0].facts.user, "replica2");
  // Unregistering twice, or something never registered, changes nothing.
  tracker.UnregisterReplica(first);
  EXPECT_EQ(tracker.Snapshot().replicas.size(), 1u);
}

TEST(RelayStatusTrackerTest, ReplicaThatSendsMoreDuringASnapshotStaysKnown) {
  // A streaming replica is at the published edge; while the status reads,
  // more is published and sent. Read after the published position, its
  // own would be ahead of it and the byte lag lost.
  FakeStorageFacts storage;
  RelayStatusTracker tracker("relay", "1.0", 10, &storage);
  auto replica = tracker.RegisterReplica(
      ReplicaFacts{"10.0.0.1:1", "r", false, "none", "", ""});
  replica->SetDumping(true);
  replica->Progress().SetFile("binlog.000002", 4);
  replica->Progress().Advance(400, 1'700'000'000);
  replica->Progress().Idle(400);
  storage.afterPublished = [&] {
    storage.publishedPosition = 500;
    replica->Progress().Advance(500, 1'700'000'001);
  };
  const RelayStatus status = tracker.Snapshot();
  EXPECT_EQ(status.replicas[0].behindBytes, std::optional<std::uint64_t>(0));
}

TEST(RelayStatusTrackerTest, ReplicaLagIsUnknownWithoutTheSourceClock) {
  FakeStorageFacts storage;
  RelayStatusTracker tracker("relay", "1.0", 10, &storage);
  auto replica = tracker.RegisterReplica(
      ReplicaFacts{"10.0.0.1:1", "r", false, "none", "", ""});
  replica->SetDumping(true);
  replica->Progress().SetFile("binlog.000002", 4);
  replica->Progress().Advance(100, 1'700'000'000);
  const RelayStatus status = tracker.Snapshot();
  EXPECT_EQ(status.replicas[0].state, ReplicaState::CatchingUp);
  EXPECT_EQ(status.replicas[0].behindBytes, std::optional<std::uint64_t>(300));
  EXPECT_FALSE(status.replicas[0].behindSeconds.has_value());
}

}  // namespace
}  // namespace binlog_streamer
