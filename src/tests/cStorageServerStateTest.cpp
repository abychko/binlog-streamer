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

#include "cStorageServerState.hpp"

#include "binlog/eEventType.hpp"
#include "binlog/hEventLimits.hpp"
#include "storage/cStorageCatalog.hpp"

#include <gtest/gtest.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

// The UUID the groups below are written under, as text and as the 16 bytes
// a GTID event carries.
constexpr const char *SOURCE_UUID = "3e11fa47-71ca-11e1-9e33-c80aa9429562";
constexpr std::array<std::uint8_t, 16> SOURCE_UUID_BYTES = {
    0x3e, 0x11, 0xfa, 0x47, 0x71, 0xca, 0x11, 0xe1,
    0x9e, 0x33, 0xc8, 0x0a, 0xa9, 0x42, 0x95, 0x62};

// Everything before the first group: this test never reads it back, only
// the offset it ends at, which is what a record's headerLength means.
constexpr std::uint64_t HEADER_LENGTH = 120;

class TempDirectory {
 public:
  TempDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "binlog-streamer-server-state-testXXXXXX")
                       .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char *created = mkdtemp(buffer.data());
    if (created == nullptr)
      throw std::runtime_error(std::string("mkdtemp failed: ") +
                               std::strerror(errno));
    m_directory = created;
  }
  ~TempDirectory() { std::filesystem::remove_all(m_directory); }
  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  const std::filesystem::path &Directory() const { return m_directory; }
  std::filesystem::path Path(const std::string &name) const {
    return m_directory / name;
  }

 private:
  std::filesystem::path m_directory;
};

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

void AppendEvent(std::vector<std::uint8_t> &file, std::uint8_t type,
                 std::span<const std::uint8_t> body,
                 std::uint64_t nextPosition) {
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
  body.insert(body.end(), SOURCE_UUID_BYTES.begin(), SOURCE_UUID_BYTES.end());
  AppendLittleEndian(body, static_cast<std::uint64_t>(gno), 8);
  body.push_back(0x02);
  AppendLittleEndian(body, 1, 8);
  AppendLittleEndian(body, 2, 8);
  AppendLittleEndian(body, 0x0001'2345'6789ULL, 7);
  const auto encodedLength = Lenenc(transactionLength);
  body.insert(body.end(), encodedLength.begin(), encodedLength.end());
  return body;
}

// GTID(69) + Query(29) + Xid(27) = 125, no checksum trailer: the records
// below are written without one.
constexpr std::uint64_t GROUP_LENGTH = 125;

void AppendFullGroup(std::vector<std::uint8_t> &file, std::uint64_t start,
                     std::int64_t gno) {
  AppendEvent(file, static_cast<std::uint8_t>(EventType::Gtid),
              GtidBody(gno, GROUP_LENGTH), start + 69);
  AppendEvent(file, 2 /* Query */, std::vector<std::uint8_t>(10, 0xAB),
              start + 69 + 29);
  AppendEvent(file, 16 /* Xid */, std::vector<std::uint8_t>(8, 0xCD),
              start + GROUP_LENGTH);
}

// Writes `groups` complete transactions after a header-sized filler,
// numbered from firstGno. Returns the file's size.
std::uint64_t WriteFileWithGroups(const std::filesystem::path &path,
                                  std::int64_t firstGno, int groups) {
  std::vector<std::uint8_t> bytes(HEADER_LENGTH, 0x00);
  for (int i = 0; i < groups; ++i)
    AppendFullGroup(bytes, bytes.size(), firstGno + i);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return bytes.size();
}

StoredFileRecord MakeRecord(const std::string &name, std::uint64_t number,
                            const std::string &previousGtids,
                            const std::string &checksumAlgorithm = "NONE") {
  StoredFileRecord record;
  record.name = name;
  record.basename = "binlog";
  record.number = number;
  record.headerLength = HEADER_LENGTH;
  record.serverId = 1;
  record.checksumAlgorithm = checksumAlgorithm;
  record.serverVersion = "8.4.11";
  std::string error;
  EXPECT_TRUE(record.previousGtids.AddFromText(previousGtids, error)) << error;
  return record;
}

TEST(StorageServerStateTest, AnswersAnEmptyStorageWithoutTouchingTheDisk) {
  TempDirectory directory;
  StorageCatalog catalog;
  const StorageServerState state(catalog, directory.Directory());

  EXPECT_EQ(state.GtidPurged(), "");
  EXPECT_EQ(state.GtidExecuted(), "");
  // A server's own default until a stored file says otherwise.
  EXPECT_EQ(state.BinlogChecksum(), "CRC32");
  // No file, no header to read the source's version from - and the
  // listener refuses connections while that is so.
  EXPECT_EQ(state.SourceVersion(), "");
  EXPECT_FALSE(state.PreviousGtids("binlog.000001").has_value());
}

TEST(StorageServerStateTest, GtidPurgedIsThePreviousGtidsOfTheOldestFile) {
  TempDirectory directory;
  StorageCatalog catalog;
  catalog.Add(
      MakeRecord("binlog.000001", 1, std::string(SOURCE_UUID) + ":1-2"));
  catalog.Add(
      MakeRecord("binlog.000002", 2, std::string(SOURCE_UUID) + ":1-9"));
  const StorageServerState state(catalog, directory.Directory());

  EXPECT_EQ(state.GtidPurged(), std::string(SOURCE_UUID) + ":1-2");

  std::string error;
  ASSERT_TRUE(catalog.Remove(error)) << error;
  EXPECT_EQ(state.GtidPurged(), std::string(SOURCE_UUID) + ":1-9");
}

TEST(StorageServerStateTest, BinlogChecksumComesFromTheOldestFile) {
  TempDirectory directory;
  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, "", "CRC32"));
  const StorageServerState crc(catalog, directory.Directory());
  EXPECT_EQ(crc.BinlogChecksum(), "CRC32");

  StorageCatalog off;
  off.Add(MakeRecord("binlog.000001", 1, "", "OFF"));
  const StorageServerState none(off, directory.Directory());
  // What a replica expects to read back: the source's OFF is NONE here.
  EXPECT_EQ(none.BinlogChecksum(), "NONE");

  StorageCatalog unknown;
  unknown.Add(MakeRecord("binlog.000001", 1, "", ""));
  const StorageServerState fallback(unknown, directory.Directory());
  EXPECT_EQ(fallback.BinlogChecksum(), "CRC32");
}

// The newest file, not the oldest: an upgraded source writes its new
// version into the file it starts after the upgrade, and that is the
// version the relay presents from then on.
TEST(StorageServerStateTest, SourceVersionComesFromTheNewestFile) {
  TempDirectory directory;
  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, ""));
  StoredFileRecord newer = MakeRecord("binlog.000002", 2, "");
  newer.serverVersion = "8.4.12";
  catalog.Add(newer);
  const StorageServerState state(catalog, directory.Directory());

  EXPECT_EQ(state.SourceVersion(), "8.4.12");
}

TEST(StorageServerStateTest, GtidExecutedAddsTheGroupsTheLastFileHolds) {
  TempDirectory directory;
  const std::uint64_t size =
      WriteFileWithGroups(directory.Path("binlog.000002"), 3, 2);

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, std::string(SOURCE_UUID) + ":1"));
  StoredFileRecord last =
      MakeRecord("binlog.000002", 2, std::string(SOURCE_UUID) + ":1-2");
  last.size = size;
  catalog.Add(last);
  const StorageServerState state(catalog, directory.Directory());

  EXPECT_EQ(state.GtidExecuted(), std::string(SOURCE_UUID) + ":1-4");
}

TEST(StorageServerStateTest, GtidExecutedPicksUpGroupsAppendedSinceItLastRan) {
  TempDirectory directory;
  const auto path = directory.Path("binlog.000001");
  WriteFileWithGroups(path, 1, 1);

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, ""));
  const StorageServerState state(catalog, directory.Directory());
  EXPECT_EQ(state.GtidExecuted(), std::string(SOURCE_UUID) + ":1");

  std::vector<std::uint8_t> appended;
  AppendFullGroup(appended, HEADER_LENGTH + GROUP_LENGTH, 2);
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out.write(reinterpret_cast<const char *>(appended.data()),
            static_cast<std::streamsize>(appended.size()));
  out.close();

  EXPECT_EQ(state.GtidExecuted(), std::string(SOURCE_UUID) + ":1-2");
}

TEST(StorageServerStateTest, GtidExecutedStartsOverWhenTheLastFileChanges) {
  TempDirectory directory;
  WriteFileWithGroups(directory.Path("binlog.000001"), 1, 1);
  WriteFileWithGroups(directory.Path("binlog.000002"), 2, 1);

  StorageCatalog catalog;
  catalog.Add(MakeRecord("binlog.000001", 1, ""));
  const StorageServerState state(catalog, directory.Directory());
  ASSERT_EQ(state.GtidExecuted(), std::string(SOURCE_UUID) + ":1");

  // The rotation the relay just made: the scan restarts at the new file's
  // header, its Previous_gtids covering everything before it.
  catalog.Add(MakeRecord("binlog.000002", 2, std::string(SOURCE_UUID) + ":1"));
  EXPECT_EQ(state.GtidExecuted(), std::string(SOURCE_UUID) + ":1-2");
}

TEST(StorageServerStateTest, GtidExecutedIsThePreviousGtidsOfAnUnreadableFile) {
  TempDirectory directory;  // nothing written into it
  StorageCatalog catalog;
  catalog.Add(
      MakeRecord("binlog.000001", 1, std::string(SOURCE_UUID) + ":1-5"));
  const StorageServerState state(catalog, directory.Directory());

  EXPECT_EQ(state.GtidExecuted(), std::string(SOURCE_UUID) + ":1-5");
}

TEST(StorageServerStateTest, PreviousGtidsDescribesTheEventInAStoredHeader) {
  TempDirectory directory;
  StorageCatalog catalog;
  StoredFileRecord record =
      MakeRecord("binlog.000001", 1, std::string(SOURCE_UUID) + ":1-2");
  record.serverId = 77;
  catalog.Add(record);
  const StorageServerState state(catalog, directory.Directory());

  const auto event = state.PreviousGtids("binlog.000001");
  ASSERT_TRUE(event.has_value());
  EXPECT_EQ(event->endPosition, HEADER_LENGTH);
  EXPECT_EQ(event->position, HEADER_LENGTH - EVENT_HEADER_LENGTH -
                                 record.previousGtids.GetEncodedLength(false));
  EXPECT_EQ(event->serverId, 77u);
  EXPECT_EQ(event->gtids, std::string(SOURCE_UUID) + ":1-2");
  EXPECT_FALSE(state.PreviousGtids("binlog.000009").has_value());
}

// A header too short to hold the event it would describe is storage this
// class cannot speak for, rather than an event with a negative position.
TEST(StorageServerStateTest, PreviousGtidsIsNulloptWhenTheHeaderIsTooShort) {
  TempDirectory directory;
  StorageCatalog catalog;
  StoredFileRecord record =
      MakeRecord("binlog.000001", 1, std::string(SOURCE_UUID) + ":1-2");
  record.headerLength = EVENT_HEADER_LENGTH;
  catalog.Add(record);
  const StorageServerState state(catalog, directory.Directory());

  EXPECT_FALSE(state.PreviousGtids("binlog.000001").has_value());
}

}  // namespace
}  // namespace binlog_streamer
