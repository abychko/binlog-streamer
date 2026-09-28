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

// End to end over a real loopback socket and the relay's own client-side
// receiver, not mocks: what arrives must match the stored files byte for
// byte, except where a source alters an event too.

#include "server/cReplicaListener.hpp"

#include "binlog/cCrc32.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "gtid/cGtidSet.hpp"
#include "net/cTcpTransport.hpp"
#include "net/cWakeupPipe.hpp"
#include "receiver/cEventStreamReader.hpp"
#include "receiver/cReplicaSession.hpp"
#include "receiver/eStreamEndReason.hpp"
#include "receiver/sStreamResult.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageReader.hpp"

#include "binlog/eEventType.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace binlog_streamer {
namespace {

constexpr char SOURCE_UUID[] = "3e11fa47-71ca-11e1-9e33-c80aa9429562";
constexpr char RELAY_UUID[] = "8a94f357-aab4-11df-86ab-c80aa9429562";
constexpr std::uint32_t SOURCE_SERVER_ID = 7;
constexpr std::uint32_t RELAY_SERVER_ID = 1001;
constexpr std::size_t FLAGS_OFFSET = 17;
constexpr std::size_t CREATED_OFFSET = EVENT_HEADER_LENGTH + 2 + 50;

void AppendLE(std::vector<std::uint8_t> &out, std::uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void WriteChecksum(std::vector<std::uint8_t> &event) {
  const std::size_t covered = event.size() - CHECKSUM_LENGTH;
  const std::uint32_t crc =
      Crc32::Compute(std::span<const std::uint8_t>(event.data(), covered));
  for (std::size_t i = 0; i < CHECKSUM_LENGTH; ++i)
    event[covered + i] = static_cast<std::uint8_t>(crc >> (8 * i));
}

// position is where the event starts; its header carries where it ends
// (position + length), as a stored event does.
std::vector<std::uint8_t> MakeEvent(std::uint8_t type,
                                    const std::vector<std::uint8_t> &body,
                                    std::uint64_t position) {
  const std::size_t length =
      EVENT_HEADER_LENGTH + body.size() + CHECKSUM_LENGTH;
  std::vector<std::uint8_t> event;
  AppendLE(event, 1700000000, 4);
  event.push_back(type);
  AppendLE(event, SOURCE_SERVER_ID, 4);
  AppendLE(event, length, 4);
  AppendLE(event, position + length, 4);
  AppendLE(event, 0, 2);
  event.insert(event.end(), body.begin(), body.end());
  event.resize(length);
  WriteChecksum(event);
  return event;
}

std::vector<std::uint8_t> FormatDescriptionBody(std::uint32_t created) {
  std::vector<std::uint8_t> body;
  AppendLE(body, 4, 2);
  const std::string version = "8.4.11";
  body.insert(body.end(), version.begin(), version.end());
  body.resize(2 + 50, 0);
  AppendLE(body, created, 4);
  body.push_back(static_cast<std::uint8_t>(EVENT_HEADER_LENGTH));
  body.resize(body.size() + 41,
              0);  // post-header lengths, not looked at by anything here
  body.push_back(1);
  return body;
}

struct BuiltFile {
  std::vector<std::uint8_t> bytes;
  std::uint64_t headerLength =
      0;  // magic, Format_description and Previous_gtids
  std::vector<std::uint64_t> transactionEnds;
};

std::array<std::uint8_t, 16> SourceUuidBytes() {
  std::array<std::uint8_t, 16> bytes{};
  std::size_t index = 0;
  const std::string text = SOURCE_UUID;
  for (std::size_t i = 0; i + 1 < text.size() && index < bytes.size();) {
    if (text[i] == '-') {
      ++i;
      continue;
    }
    bytes[index++] =
        static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16));
    i += 2;
  }
  return bytes;
}

std::vector<std::uint8_t> GtidBody(std::int64_t gno) {
  std::vector<std::uint8_t> body{0x00};
  const auto uuid = SourceUuidBytes();
  body.insert(body.end(), uuid.begin(), uuid.end());
  AppendLE(body, static_cast<std::uint64_t>(gno), 8);
  body.push_back(2);  // LOGICAL_TIMESTAMP_TYPECODE
  AppendLE(body, static_cast<std::uint64_t>(gno - 1), 8);
  AppendLE(body, static_cast<std::uint64_t>(gno), 8);
  return body;
}

struct Transaction {
  std::int64_t gno = 0;
  std::size_t payloadSize = 0;
};

std::vector<std::uint8_t> TransactionBytes(const Transaction &transaction,
                                           std::uint64_t position) {
  std::vector<std::uint8_t> bytes =
      MakeEvent(33, GtidBody(transaction.gno), position);
  const std::vector<std::uint8_t> payload = MakeEvent(
      2,
      std::vector<std::uint8_t>(transaction.payloadSize,
                                static_cast<std::uint8_t>(transaction.gno)),
      position + bytes.size());
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

BuiltFile BuildFile(std::uint32_t created, const GtidSet &previousGtids,
                    const std::vector<Transaction> &transactions, bool inUse) {
  BuiltFile file;
  file.bytes = {0xFE, 'b', 'i', 'n'};
  auto append = [&file](const std::vector<std::uint8_t> &event) {
    file.bytes.insert(file.bytes.end(), event.begin(), event.end());
  };
  std::vector<std::uint8_t> description =
      MakeEvent(15, FormatDescriptionBody(created), file.bytes.size());
  // A source computes this checksum with the flag clear and sets the flag
  // afterwards, which is why the flag can be cleared again without one.
  if (inUse)
    description[FLAGS_OFFSET] |=
        static_cast<std::uint8_t>(EVENT_FLAG_BINLOG_IN_USE);
  append(description);
  append(MakeEvent(35, previousGtids.Encode(false), file.bytes.size()));
  file.headerLength = file.bytes.size();
  for (const Transaction &transaction : transactions) {
    file.transactionEnds.push_back(
        file.bytes.size() +
        TransactionBytes(transaction, file.bytes.size()).size());
    append(TransactionBytes(transaction, file.bytes.size()));
  }
  return file;
}

IpAddress Loopback() {
  IpAddress address;
  address.family = AddressFamily::Ipv4;
  address.bytes = {127, 0, 0, 1};
  return address;
}

std::uint16_t FindFreePort() {
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(probe, reinterpret_cast<struct sockaddr *>(&address), sizeof(address));
  socklen_t length = sizeof(address);
  getsockname(probe, reinterpret_cast<struct sockaddr *>(&address), &length);
  const std::uint16_t port = ntohs(address.sin_port);
  close(probe);
  return port;
}

class FixedServerState : public ServerState {
 public:
  explicit FixedServerState(std::string checksum)
      : m_checksum(std::move(checksum)) {}
  std::string GtidPurged() const override { return ""; }
  std::string GtidExecuted() const override {
    return "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9";
  }
  std::string SourceVersion() const override { return "8.4.11"; }
  std::string BinlogChecksum() const override { return m_checksum; }
  std::optional<PreviousGtidsEvent> PreviousGtids(
      const std::string &) const override {
    return std::nullopt;
  }

 private:
  std::string m_checksum;
};

class CollectingSink : public EventSink {
 public:
  explicit CollectingSink(std::size_t expectedBytes)
      : m_expectedBytes(expectedBytes) {}

  bool OnEventBegin(const EventHeader &header,
                    const StreamPosition &) override {
    const bool heartbeat =
        header.type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
        header.type == static_cast<std::uint8_t>(EventType::HeartbeatV2);
    m_artificial = heartbeat || (header.flags & EVENT_FLAG_ARTIFICIAL) != 0;
    m_rotate = m_artificial &&
               header.type == static_cast<std::uint8_t>(EventType::Rotate);
    if (m_rotate) rotates.emplace_back();
    if (heartbeat) {
      heartbeatPositions.push_back(header.nextPosition);
      heartbeatTimes.push_back(std::chrono::steady_clock::now());
      return heartbeatsWanted == 0 ||
             heartbeatPositions.size() < heartbeatsWanted ||
             stored.size() < m_expectedBytes;
    }
    if (m_artificial) {
      ++artificialEvents;
      artificialServerId = header.serverId;
    }
    return true;
  }
  bool OnEventBytes(std::span<const std::uint8_t> bytes) override {
    if (!m_artificial && stored.empty() && !bytes.empty())
      startedAt = std::chrono::steady_clock::now();
    if (!m_artificial) stored.insert(stored.end(), bytes.begin(), bytes.end());
    if (!m_artificial && stored.size() >= m_expectedBytes &&
        completedAt == std::chrono::steady_clock::time_point{})
      completedAt = std::chrono::steady_clock::now();
    if (m_rotate)
      rotates.back().insert(rotates.back().end(), bytes.begin(), bytes.end());
    return true;
  }
  bool OnEventEnd() override {
    return stored.size() < m_expectedBytes ||
           heartbeatPositions.size() < heartbeatsWanted;
  }

  std::vector<std::uint8_t> stored;
  std::vector<std::uint32_t> heartbeatPositions;
  std::vector<std::chrono::steady_clock::time_point> heartbeatTimes;
  std::size_t heartbeatsWanted = 0;
  // When the first stored byte arrived.
  std::chrono::steady_clock::time_point startedAt{};
  // When the last expected byte arrived.
  std::chrono::steady_clock::time_point completedAt{};
  int artificialEvents = 0;
  std::uint32_t artificialServerId = 0;
  std::vector<std::vector<std::uint8_t>>
      rotates;  // each synthesized rotate, header included

 private:
  std::size_t m_expectedBytes;
  bool m_artificial = false;
  bool m_rotate = false;
};

constexpr int StyleBit(WaitStyle style) {
  return style == WaitStyle::Block ? 2 : 1;
}

// Simulates what a dump sees when a file is about to be closed: storage
// as-is, except once told to it reports the published position as moved
// while nothing is readable yet. Also notes each way of waiting the dump
// asked for, as StyleBit()s.
class UnreadableTailReader : public BinlogStorageReader {
 public:
  UnreadableTailReader(BinlogStorageReader &storage,
                       const std::atomic<bool> &movedButUnreadable,
                       std::atomic<int> &waitStyles)
      : m_storage(storage),
        m_movedButUnreadable(movedButUnreadable),
        m_waitStyles(waitStyles) {}

  std::optional<std::string> FindStartFile(
      const GtidSet &replicaSet) const override {
    return m_storage.FindStartFile(replicaSet);
  }
  std::unique_ptr<FileCursor> Open(const std::string &fileName,
                                   std::string &error) override {
    return m_storage.Open(fileName, error);
  }
  std::size_t Read(const FileCursor &cursor, std::uint64_t offset,
                   std::span<std::uint8_t> out, std::string &error) override {
    return m_storage.Read(cursor, offset, out, error);
  }
  NextFileOutcome Next(const FileCursor &current, std::uint64_t offset,
                       std::unique_ptr<FileCursor> &next,
                       std::string &error) override {
    return m_storage.Next(current, offset, next, error);
  }
  WaitOutcome WaitForNewEvents(const PublishedPosition &target,
                               std::chrono::milliseconds timeout,
                               WaitStyle style) override {
    m_waitStyles.fetch_or(StyleBit(style));
    if (!m_movedButUnreadable.load())
      return m_storage.WaitForNewEvents(target, timeout, style);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return WaitOutcome::Advanced;
  }
  PublishedPosition Published() const override { return m_storage.Published(); }

 private:
  BinlogStorageReader &m_storage;
  const std::atomic<bool> &m_movedButUnreadable;
  std::atomic<int> &m_waitStyles;
};

class DumpSenderLoopbackTest : public ::testing::Test {
 protected:
  std::filesystem::path dataDir;
  std::atomic<bool> movedButUnreadable{false};  // see UnreadableTailReader
  std::atomic<int> waitStyles{0};               // see UnreadableTailReader
  std::atomic<bool> stopRequested{false};
  WakeupPipe wakeupPipe;
  StorageCatalog catalog;
  PublishedPositionTracker published;
  GtidSet firstFileGtids;  // what the first file adds: the second one's
                           // Previous_gtids
  BuiltFile first;
  BuiltFile second;

  void SetUp() override {
    std::string error;
    ASSERT_TRUE(wakeupPipe.Open(error)) << error;
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    dataDir =
        std::filesystem::temp_directory_path() /
        ("dump-sender-test-" + std::to_string(getpid()) + "-" + info->name());
    std::filesystem::remove_all(dataDir);
    std::filesystem::create_directories(dataDir);

    ASSERT_TRUE(
        firstFileGtids.AddFromText(std::string(SOURCE_UUID) + ":1-3", error))
        << error;
    // One body is larger than a protocol packet can carry, so its event
    // travels as two.
    first = BuildFile(1700000123, GtidSet(),
                      {{1, 100}, {2, 17UL * 1024UL * 1024UL}, {3, 300}}, false);
    second = BuildFile(0, firstFileGtids, {{4, 50}, {5, 60}}, true);
    Store("binlog.000001", 1, first, GtidSet(), false);
    Store("binlog.000002", 2, second, firstFileGtids, true);
    published.Advance("binlog.000002", second.bytes.size());
  }
  void TearDown() override { std::filesystem::remove_all(dataDir); }

  void Store(const std::string &name, std::uint64_t number,
             const BuiltFile &file, const GtidSet &previousGtids, bool inUse) {
    std::ofstream(dataDir / name, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.bytes.data()),
               static_cast<std::streamsize>(file.bytes.size()));
    StoredFileRecord record;
    record.name = name;
    record.basename = "binlog";
    record.number = number;
    record.size = inUse ? 0 : file.bytes.size();
    record.headerLength = file.headerLength;
    record.serverId = SOURCE_SERVER_ID;
    record.checksumAlgorithm = "CRC32";
    record.inUse = inUse;
    record.previousGtids = previousGtids;
    catalog.Add(record);
  }

  struct Client {
    std::string uuid = "11111111-2222-3333-4444-555555555555";
    std::uint32_t serverId = 2002;
    bool registerAsReplica = true;
    std::chrono::seconds heartbeatPeriod = std::chrono::seconds(30);
    std::function<void()> whileReceiving;
    std::size_t checksumLength = CHECKSUM_LENGTH;
  };

  std::string relayChecksum =
      "CRC32";  // what the relay reports as @@global.binlog_checksum
  std::chrono::microseconds sendLinger{0};  // server.send_linger

  std::uint16_t listenPort = 0;
  StreamResult lastStream;
  ReplicaListener *activeListener = nullptr;

  // Safe to call from a second thread: reports failures with ADD_FAILURE
  // rather than an ASSERT_* that would only abort the calling thread.
  StreamResult RunClient(const Client &client, const std::string &replicaGtids,
                         CollectingSink &sink) {
    StreamResult failed;
    failed.reason = StreamEndReason::MalformedStream;

    SourceSettings source;
    source.host = "127.0.0.1";
    source.port = listenPort;
    source.user = "repl";
    source.password = "s3cret";
    ServerSettings downstream;
    downstream.serverId = client.serverId;
    TcpTransport transport;
    ReplicaSessionOptions sessionOptions;
    sessionOptions.heartbeatPeriod = client.heartbeatPeriod;
    sessionOptions.registerAsReplica = client.registerAsReplica;
    ReplicaSession session(transport, source, downstream, client.uuid,
                           "binlog-streamer", "0.20.0", sessionOptions);
    const SessionResult prepared = session.Run();
    if (prepared.outcome != SessionOutcome::Registered) {
      ADD_FAILURE() << "preparation failed: " << prepared.message;
      return failed;
    }

    GtidSet replicaSet;
    std::string error;
    if (!replicaGtids.empty() && !replicaSet.AddFromText(replicaGtids, error)) {
      ADD_FAILURE() << error;
      return failed;
    }
    const auto dumpFailure = session.StartDump(replicaSet);
    if (dumpFailure.has_value()) {
      ADD_FAILURE() << dumpFailure->message;
      return failed;
    }

    StreamReaderOptions options;
    options.sequenceId = session.NextSequenceId();
    options.checksumLength = client.checksumLength;
    options.readTimeout = std::chrono::milliseconds(10000);
    EventStreamReader streamReader(transport, sink, StreamPosition{}, options);
    std::thread background;
    if (client.whileReceiving) background = std::thread(client.whileReceiving);
    const StreamResult streamed = streamReader.Run();
    if (background.joinable()) background.join();
    transport.Close();
    return streamed;
  }

  void Receive(const Client &client, const std::string &replicaGtids,
               std::size_t expectedBytes, CollectingSink &sink) {
    ReplicaSettings settings;
    settings.listenAddress = Loopback();
    settings.listenPort = FindFreePort();
    ReplicaClient account;
    account.user = "repl";
    account.password = "s3cret";
    account.hosts = {AddressRange{Loopback(), 32}};
    settings.clients = {account};
    FixedServerState state(relayChecksum);
    StorageReader storage(dataDir, catalog, published);
    UnreadableTailReader reader(storage, movedButUnreadable, waitStyles);
    ReplicaListener listener(
        settings, DEFAULT_MAX_CONNECTIONS, &stopRequested, &wakeupPipe, {},
        ServerIdentity{RELAY_SERVER_ID, RELAY_UUID, "test relay",
                       "binlog-streamer", "0.20.0"},
        &state, &reader, nullptr, nullptr, sendLinger);
    std::string error;
    ASSERT_TRUE(listener.Start(error)) << error;
    listenPort = settings.listenPort;
    activeListener = &listener;

    lastStream = RunClient(client, replicaGtids, sink);
    EXPECT_EQ(sink.stored.size(), expectedBytes) << lastStream.message;

    stopRequested.store(true);
    listener.Stop();
    activeListener = nullptr;
  }

  void Receive(const std::string &replicaGtids, std::size_t expectedBytes,
               CollectingSink &sink,
               std::chrono::seconds heartbeatPeriod = std::chrono::seconds(30),
               const std::function<void()> &whileReceiving = {}) {
    Client client;
    client.heartbeatPeriod = heartbeatPeriod;
    client.whileReceiving = whileReceiving;
    Receive(client, replicaGtids, expectedBytes, sink);
  }
};

TEST_F(DumpSenderLoopbackTest,
       AReplicaWithNothingReceivesEveryStoredFileByteForByte) {
  std::vector<std::uint8_t> expected(first.bytes.begin() + 4,
                                     first.bytes.end());
  std::vector<std::uint8_t> secondEvents(second.bytes.begin() + 4,
                                         second.bytes.end());
  secondEvents[FLAGS_OFFSET] &= static_cast<std::uint8_t>(
      ~EVENT_FLAG_BINLOG_IN_USE);  // the one change a source makes too
  expected.insert(expected.end(), secondEvents.begin(), secondEvents.end());

  CollectingSink sink(expected.size());
  Receive("", expected.size(), sink);

  EXPECT_TRUE(sink.stored == expected);
  // one synthesized rotate ahead of each of the two files
  EXPECT_EQ(sink.artificialEvents, 2);
  EXPECT_EQ(sink.artificialServerId, RELAY_SERVER_ID);
}

TEST_F(
    DumpSenderLoopbackTest,
    ARotateAfterAChecksummedFileCarriesAChecksumEvenForAReplicaThatAskedForNone) {
  // mysqlbinlog asks for no checksums and then reads every event after a
  // CRC32 Format_description as checksummed: a rotate without one would
  // lose the last four bytes of the file name it announces.
  relayChecksum = "NONE";
  std::vector<std::uint8_t> expected(first.bytes.begin() + 4,
                                     first.bytes.end());
  std::vector<std::uint8_t> secondEvents(second.bytes.begin() + 4,
                                         second.bytes.end());
  secondEvents[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  expected.insert(expected.end(), secondEvents.begin(), secondEvents.end());

  CollectingSink sink(expected.size());
  Client client;
  client.checksumLength = 0;
  Receive(client, "", expected.size(), sink);

  ASSERT_EQ(sink.rotates.size(), 2U);
  const std::string firstName = "binlog.000001";
  const std::string secondName = "binlog.000002";
  // Ahead of the first file nothing says the stream is checksummed yet.
  ASSERT_EQ(sink.rotates[0].size(), EVENT_HEADER_LENGTH + 8 + firstName.size());
  EXPECT_EQ(std::string(sink.rotates[0].end() - firstName.size(),
                        sink.rotates[0].end()),
            firstName);
  // Ahead of the second one it is, and the checksum is right.
  const std::vector<std::uint8_t> &rotate = sink.rotates[1];
  ASSERT_EQ(rotate.size(),
            EVENT_HEADER_LENGTH + 8 + secondName.size() + CHECKSUM_LENGTH);
  const std::size_t covered = rotate.size() - CHECKSUM_LENGTH;
  EXPECT_EQ(std::string(rotate.begin() + EVENT_HEADER_LENGTH + 8,
                        rotate.begin() + covered),
            secondName);
  std::uint32_t carried = 0;
  for (std::size_t i = 0; i < CHECKSUM_LENGTH; ++i)
    carried |= static_cast<std::uint32_t>(rotate[covered + i]) << (8 * i);
  EXPECT_EQ(carried, Crc32::Compute(std::span<const std::uint8_t>(rotate.data(),
                                                                  covered)));
}

TEST_F(
    DumpSenderLoopbackTest,
    ATransactionTheReplicaHasIsReplacedByAHeartbeatAndTheCreationTimeZeroed) {
  std::vector<std::uint8_t> expected(
      first.bytes.begin() + 4,
      first.bytes.begin() + static_cast<std::ptrdiff_t>(first.headerLength));
  expected.insert(expected.end(),
                  first.bytes.begin() +
                      static_cast<std::ptrdiff_t>(first.transactionEnds[0]),
                  first.bytes.end());
  const std::size_t descriptionLength =
      EVENT_HEADER_LENGTH + FormatDescriptionBody(0).size() + CHECKSUM_LENGTH;
  std::vector<std::uint8_t> description(
      expected.begin(),
      expected.begin() + static_cast<std::ptrdiff_t>(descriptionLength));
  for (std::size_t i = 0; i < 4; ++i) description[CREATED_OFFSET + i] = 0;
  WriteChecksum(description);
  std::copy(description.begin(), description.end(), expected.begin());

  CollectingSink sink(expected.size());
  Receive(std::string(SOURCE_UUID) + ":1", expected.size(), sink);

  EXPECT_TRUE(sink.stored == expected);
  ASSERT_FALSE(sink.heartbeatPositions.empty());
  EXPECT_EQ(sink.heartbeatPositions.front(), first.transactionEnds[0]);
}

TEST_F(DumpSenderLoopbackTest,
       AReplicaAheadOfTheRelayWaitsOnHeartbeatsAndGoesOnOnceTheRelayCatchesUp) {
  const Transaction fresh{101, 80};
  const std::vector<std::uint8_t> freshBytes =
      TransactionBytes(fresh, second.bytes.size());
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  expected.insert(expected.end(), freshBytes.begin(), freshBytes.end());

  CollectingSink sink(expected.size());
  sink.heartbeatsWanted =
      3;  // one for what was left out, and idle ones a second apart
  Receive(std::string(SOURCE_UUID) + ":1-100", expected.size(), sink,
          std::chrono::seconds(1), [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            std::ofstream(dataDir / "binlog.000002",
                          std::ios::binary | std::ios::app)
                .write(reinterpret_cast<const char *>(freshBytes.data()),
                       static_cast<std::streamsize>(freshBytes.size()));
            published.Advance("binlog.000002",
                              second.bytes.size() + freshBytes.size());
          });

  EXPECT_TRUE(sink.stored == expected);
  ASSERT_GE(sink.heartbeatPositions.size(), 3u);
  EXPECT_EQ(sink.heartbeatPositions.front(),
            second.bytes.size());  // everything stored so far was left out
  EXPECT_EQ(sink.heartbeatPositions[1],
            second.bytes.size());  // and nothing moved while it waited
}

TEST_F(DumpSenderLoopbackTest,
       ASkippedTransactionOfManyMegabytesIsPassedOverAndWhatFollowsArrives) {
  std::vector<std::uint8_t> expected(
      first.bytes.begin() + 4,
      first.bytes.begin() + static_cast<std::ptrdiff_t>(first.headerLength));
  expected.insert(expected.end(),
                  first.bytes.begin() +
                      static_cast<std::ptrdiff_t>(first.transactionEnds[1]),
                  first.bytes.end());
  const std::size_t descriptionLength =
      EVENT_HEADER_LENGTH + FormatDescriptionBody(0).size() + CHECKSUM_LENGTH;
  std::vector<std::uint8_t> description(
      expected.begin(),
      expected.begin() + static_cast<std::ptrdiff_t>(descriptionLength));
  for (std::size_t i = 0; i < 4; ++i) description[CREATED_OFFSET + i] = 0;
  WriteChecksum(description);
  std::copy(description.begin(), description.end(), expected.begin());

  CollectingSink sink(expected.size());
  Receive(std::string(SOURCE_UUID) + ":1-2", expected.size(), sink);

  EXPECT_TRUE(sink.stored == expected);
  ASSERT_FALSE(sink.heartbeatPositions.empty());
  EXPECT_EQ(sink.heartbeatPositions.front(), first.transactionEnds[1]);
}

TEST_F(DumpSenderLoopbackTest,
       ASkippedEventNotYetWhollyPublishedIsNotPassedOver) {
  // A transaction the replica has is stored whole but published only up to
  // the middle of its payload: the dump may stand before that event, never
  // past it.
  const std::vector<std::uint8_t> partial =
      TransactionBytes(Transaction{6, 5000}, second.bytes.size());
  std::ofstream(dataDir / "binlog.000002", std::ios::binary | std::ios::app)
      .write(reinterpret_cast<const char *>(partial.data()),
             static_cast<std::streamsize>(partial.size()));
  const std::uint64_t payloadStart = second.bytes.size() + EVENT_HEADER_LENGTH +
                                     GtidBody(6).size() + CHECKSUM_LENGTH;
  published.Advance("binlog.000002", payloadStart + 100);
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);

  CollectingSink sink(expected.size());
  sink.heartbeatsWanted = 2;
  Receive(std::string(SOURCE_UUID) + ":1-100", expected.size(), sink,
          std::chrono::seconds(1));

  ASSERT_GE(sink.heartbeatPositions.size(), 2u) << lastStream.message;
  for (const std::uint32_t position : sink.heartbeatPositions)
    EXPECT_EQ(position, payloadStart);
}

TEST_F(DumpSenderLoopbackTest,
       AReplicaWithAHoleInStoredHistoryGetsTheHoleAndNothingItHas) {
  // Three files begin with nothing, 1-3 and 1-5; the replica has 1-3 and 5.
  // The newest file already assumes 4, so the stream has to start from the
  // middle one: 4 and 6 arrive, 5 and the whole first file do not.
  std::string error;
  ASSERT_TRUE(catalog.Close(second.bytes.size(), error)) << error;
  GtidSet secondFileGtids;
  ASSERT_TRUE(
      secondFileGtids.AddFromText(std::string(SOURCE_UUID) + ":1-5", error))
      << error;
  const BuiltFile third = BuildFile(0, secondFileGtids, {{6, 70}}, true);
  Store("binlog.000003", 3, third, secondFileGtids, true);
  published.Advance("binlog.000003", third.bytes.size());

  auto headerOf = [](const BuiltFile &file) {
    std::vector<std::uint8_t> header(
        file.bytes.begin() + 4,
        file.bytes.begin() + static_cast<std::ptrdiff_t>(file.headerLength));
    header[FLAGS_OFFSET] &=
        static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
    return header;
  };
  std::vector<std::uint8_t> expected = headerOf(second);
  expected.insert(
      expected.end(),
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength),
      second.bytes.begin() +
          static_cast<std::ptrdiff_t>(second.transactionEnds[0]));
  const std::vector<std::uint8_t> thirdHeader = headerOf(third);
  expected.insert(expected.end(), thirdHeader.begin(), thirdHeader.end());
  expected.insert(
      expected.end(),
      third.bytes.begin() + static_cast<std::ptrdiff_t>(third.headerLength),
      third.bytes.end());

  CollectingSink sink(expected.size());
  Receive(std::string(SOURCE_UUID) + ":1-3:5", expected.size(), sink);

  EXPECT_TRUE(sink.stored == expected);
  ASSERT_EQ(sink.rotates.size(), 2U);
  const std::string startName = "binlog.000002";
  EXPECT_NE(std::string(sink.rotates[0].begin(), sink.rotates[0].end())
                .find(startName),
            std::string::npos);
}

TEST_F(DumpSenderLoopbackTest,
       WithASendLingerWhatIsPublishedAfterCatchingUpWaitsForIt) {
  const std::vector<std::uint8_t> next =
      TransactionBytes(Transaction{6, 50}, second.bytes.size());
  std::ofstream(dataDir / "binlog.000002", std::ios::binary | std::ios::app)
      .write(reinterpret_cast<const char *>(next.data()),
             static_cast<std::streamsize>(next.size()));
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  expected.insert(expected.end(), next.begin(), next.end());
  sendLinger = std::chrono::milliseconds(200);

  std::chrono::steady_clock::time_point publishedAt{};
  CollectingSink sink(expected.size());
  Receive(std::string(SOURCE_UUID) + ":1-5", expected.size(), sink,
          std::chrono::seconds(30), [&] {
            // Past the linger of the file's own header: the dump is
            // caught up and waiting.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            publishedAt = std::chrono::steady_clock::now();
            published.Advance("binlog.000002",
                              second.bytes.size() + next.size());
          });

  EXPECT_TRUE(sink.stored == expected) << lastStream.message;
  EXPECT_GE(sink.completedAt - publishedAt, std::chrono::milliseconds(200));
  // The linger already gathers what arrives meanwhile: no polling on top.
  EXPECT_EQ(waitStyles.load(), StyleBit(WaitStyle::Block));
}

TEST_F(DumpSenderLoopbackTest, WithoutASendLingerACaughtUpDumpPollsFirst) {
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);

  CollectingSink sink(expected.size());
  sink.heartbeatsWanted = 1;
  Receive(std::string(SOURCE_UUID) + ":1-100", expected.size(), sink,
          std::chrono::seconds(1));

  ASSERT_GE(sink.heartbeatTimes.size(), 1u) << lastStream.message;
  EXPECT_EQ(waitStyles.load(), StyleBit(WaitStyle::PollFirst));
}

TEST_F(DumpSenderLoopbackTest,
       WithASendLingerAClosedFileIsFollowedWithoutAPause) {
  // The end of the closed first file is no reason to wait: the rest of it
  // goes out as soon as it is read, not a linger later.
  const std::vector<std::uint8_t> expected(first.bytes.begin() + 4,
                                           first.bytes.end());
  sendLinger = std::chrono::seconds(1);

  CollectingSink sink(expected.size());
  Receive("", expected.size(), sink);

  ASSERT_NE(sink.completedAt, std::chrono::steady_clock::time_point{});
  EXPECT_LT(sink.completedAt - sink.startedAt, std::chrono::milliseconds(500));
}

TEST_F(DumpSenderLoopbackTest,
       WithASendLingerAsLongAsTheHeartbeatPeriodHeartbeatsKeepTheirPeriod) {
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  sendLinger = std::chrono::seconds(1);

  CollectingSink sink(expected.size());
  sink.heartbeatsWanted = 4;
  Receive(std::string(SOURCE_UUID) + ":1-100", expected.size(), sink,
          std::chrono::seconds(1));

  ASSERT_GE(sink.heartbeatTimes.size(), 4u) << lastStream.message;
  for (std::size_t i = 2; i < sink.heartbeatTimes.size(); ++i)
    EXPECT_LT(sink.heartbeatTimes[i] - sink.heartbeatTimes[i - 1],
              std::chrono::milliseconds(1600))
        << "between heartbeats " << i - 1 << " and " << i;
}

TEST_F(DumpSenderLoopbackTest,
       StoppingDuringASendLingerEndsTheDumpAndSendsWhatWasQueued) {
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  const std::vector<std::uint8_t> next =
      TransactionBytes(Transaction{6, 50}, second.bytes.size());
  std::ofstream(dataDir / "binlog.000002", std::ios::binary | std::ios::app)
      .write(reinterpret_cast<const char *>(next.data()),
             static_cast<std::streamsize>(next.size()));
  expected.insert(expected.end(), next.begin(), next.end());
  sendLinger = std::chrono::seconds(1);

  std::chrono::steady_clock::duration stopTook{};
  CollectingSink sink(SIZE_MAX);
  Receive(std::string(SOURCE_UUID) + ":1-5", expected.size(), sink,
          std::chrono::seconds(1), [&] {
            // Caught up and lingering over the file's header.
            std::this_thread::sleep_for(std::chrono::milliseconds(1300));
            published.Advance("binlog.000002",
                              second.bytes.size() + next.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto start = std::chrono::steady_clock::now();
            activeListener->Stop();
            stopTook = std::chrono::steady_clock::now() - start;
          });

  EXPECT_TRUE(sink.stored == expected) << lastStream.message;
  EXPECT_LT(stopTook, std::chrono::milliseconds(1500));
  EXPECT_EQ(lastStream.reason, StreamEndReason::ConnectionClosed)
      << lastStream.message;
}

TEST_F(DumpSenderLoopbackTest,
       HeartbeatsGoOnWhileThePublishedPositionMovesButNothingIsReadable) {
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  movedButUnreadable.store(true);

  CollectingSink sink(expected.size());
  sink.heartbeatsWanted =
      3;  // one for what was left out, the others a second apart
  Receive(std::string(SOURCE_UUID) + ":1-100", expected.size(), sink,
          std::chrono::seconds(1));

  EXPECT_EQ(lastStream.reason, StreamEndReason::StoppedBySink)
      << lastStream.message;
  ASSERT_GE(sink.heartbeatPositions.size(), 3u);
  EXPECT_EQ(sink.heartbeatPositions[1], second.bytes.size());
  EXPECT_EQ(sink.heartbeatPositions[2], second.bytes.size());
}

TEST_F(DumpSenderLoopbackTest,
       StoppingTheListenerWithoutAStopSignalEndsADumpThatWaitsForNewEvents) {
  std::vector<std::uint8_t> expected(
      second.bytes.begin() + 4,
      second.bytes.begin() + static_cast<std::ptrdiff_t>(second.headerLength));
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);

  std::chrono::steady_clock::duration stopTook{};
  CollectingSink sink(expected.size());
  // A deadline of the test's own: the sink keeps receiving until the relay
  // ends the dump, and a relay that never does is stopped here after five
  // heartbeats a second apart. Left unbounded, a regression would hang
  // until ctest killed the whole binary, taking every later test with it.
  sink.heartbeatsWanted = 5;
  Receive(std::string(SOURCE_UUID) + ":1-100", expected.size(), sink,
          std::chrono::seconds(1), [&] {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));  // the dump is waiting by now
            const auto start = std::chrono::steady_clock::now();
            activeListener->Stop();
            stopTook = std::chrono::steady_clock::now() - start;
          });

  EXPECT_LT(stopTook, std::chrono::seconds(5));
  // StoppedBySink here would mean the deadline above ran out with the dump
  // still open.
  EXPECT_EQ(lastStream.reason, StreamEndReason::ConnectionClosed)
      << lastStream.message;
}

TEST_F(DumpSenderLoopbackTest,
       AReplicaHoldingTheWholeFirstFileStartsAtTheSecond) {
  std::vector<std::uint8_t> expected(second.bytes.begin() + 4,
                                     second.bytes.end());
  expected[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);

  CollectingSink sink(expected.size());
  Receive(std::string(SOURCE_UUID) + ":1-3", expected.size(), sink);

  EXPECT_TRUE(sink.stored == expected);
  EXPECT_EQ(sink.artificialEvents, 1);
}

TEST_F(DumpSenderLoopbackTest,
       AReplicaHoldingTransactionsUnderTheRelaysOwnUuidGetsError1236) {
  CollectingSink sink(SIZE_MAX);
  Receive(std::string(RELAY_UUID) + ":1-5", 0, sink);

  EXPECT_EQ(lastStream.reason, StreamEndReason::SourceError)
      << lastStream.message;
  EXPECT_EQ(lastStream.errorCode, 1236);
  EXPECT_EQ(lastStream.errorText.rfind(
                "Replica has more GTIDs than the source has", 0),
            0u)
      << lastStream.errorText;
  EXPECT_EQ(sink.artificialEvents, 0);
}

TEST_F(DumpSenderLoopbackTest, AReplicaOlderThanTheStoredHistoryGetsError1236) {
  std::string error;
  ASSERT_TRUE(catalog.Remove(error)) << error;

  CollectingSink sink(SIZE_MAX);
  Receive(std::string(SOURCE_UUID) + ":1-2", 0, sink);

  EXPECT_EQ(lastStream.reason, StreamEndReason::SourceError)
      << lastStream.message;
  EXPECT_EQ(lastStream.errorCode, 1236);
  EXPECT_EQ(
      lastStream.errorText.rfind(
          "Cannot replicate because the source purged required binary logs", 0),
      0u)
      << lastStream.errorText;
  EXPECT_EQ(sink.artificialEvents, 0);
}

TEST_F(DumpSenderLoopbackTest,
       AClientWithServerIdZeroGetsTheStoredHistoryAndThenEof) {
  // What mysqlbinlog is without --stop-never: it does not register, its
  // server_id is 0, and it expects the stream to end where the history does.
  std::vector<std::uint8_t> expected(first.bytes.begin() + 4,
                                     first.bytes.end());
  std::vector<std::uint8_t> secondEvents(second.bytes.begin() + 4,
                                         second.bytes.end());
  secondEvents[FLAGS_OFFSET] &=
      static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  expected.insert(expected.end(), secondEvents.begin(), secondEvents.end());

  Client client;
  client.serverId = 0;
  client.registerAsReplica = false;
  CollectingSink sink(SIZE_MAX);  // never stops the stream itself
  Receive(client, "", expected.size(), sink);

  EXPECT_EQ(lastStream.reason, StreamEndReason::EndOfStream)
      << lastStream.message;
  EXPECT_TRUE(sink.stored == expected);
}

TEST_F(DumpSenderLoopbackTest, ANewDumpFromTheSameReplicaEndsItsEarlierOne) {
  // A replica reconnecting while the relay still serves its previous
  // connection: the earlier dump is closed, as a source kills the zombie
  // dump thread of the same replica.
  const std::size_t headerBytes =
      static_cast<std::size_t>(second.headerLength) - 4;
  const std::string everything = std::string(SOURCE_UUID) + ":1-100";

  StreamResult laterStream;
  Client earlier;
  earlier.heartbeatPeriod = std::chrono::seconds(1);
  earlier.whileReceiving = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Client later;
    later.heartbeatPeriod = std::chrono::seconds(1);
    CollectingSink laterSink(headerBytes);
    laterSink.heartbeatsWanted = 4;  // outlives the earlier dump's end
    laterStream = RunClient(later, everything, laterSink);
  };
  // Left alone the earlier dump would go on forever; this bound is what
  // ends the test if it is not closed.
  CollectingSink sink(headerBytes);
  sink.heartbeatsWanted = 8;
  Receive(earlier, everything, headerBytes, sink);

  EXPECT_EQ(lastStream.reason, StreamEndReason::ConnectionClosed)
      << lastStream.message;
  EXPECT_LT(sink.heartbeatPositions.size(), 8u);
  EXPECT_EQ(laterStream.reason, StreamEndReason::StoppedBySink)
      << laterStream.message;
}

}  // namespace
}  // namespace binlog_streamer
