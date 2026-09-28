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

#include "storage/cBinlogFileHeaderReader.hpp"

#include "cTempDirectoryFixture.hpp"
#include "gtid/cGtidSet.hpp"

#include <gtest/gtest.h>
#include <fstream>
#include <vector>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

void WriteFile(const std::filesystem::path &path,
               std::span<const std::uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// Real CRC32 header captured via mysqlbinlog --hexdump: magic + FDE
// (offsets 4->127) + Previous_gtids_event (offsets 127->198).
const std::vector<std::uint8_t> REAL_HEADER{
    0xfe, 0x62, 0x69, 0x6e, 0x1d, 0x64, 0xaa, 0x6a, 0x0f, 0x01, 0x00, 0x00,
    0x00, 0x7b, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x38, 0x2e, 0x34, 0x2e, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x0d, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x63, 0x00,
    0x04, 0x1a, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x0a, 0x0a, 0x0a, 0x2a, 0x2a, 0x00, 0x12, 0x34, 0x00, 0x0a, 0x28,
    0x00, 0x00, 0x01, 0xde, 0x90, 0x02, 0x3f, 0x1d, 0x64, 0xaa, 0x6a, 0x23,
    0x01, 0x00, 0x00, 0x00, 0x47, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdb, 0x06,
    0xab, 0x0a, 0x39, 0xaa, 0x11, 0xf1, 0x90, 0xc3, 0x19, 0x59, 0x73, 0x35,
    0xb2, 0xdb, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x5d, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x04, 0xd7, 0x54, 0x59,
};

TEST(BinlogFileHeaderReaderTest, ReadsARealCrc32Header) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000222"), REAL_HEADER);

  StoredFileRecord record;
  std::string error;
  ASSERT_TRUE(BinlogFileHeaderReader::Read(fixture.Path("binlog.000222"),
                                           "binlog.000222", record, error))
      << error;
  EXPECT_EQ(record.name, "binlog.000222");
  EXPECT_EQ(record.basename, "binlog");
  EXPECT_EQ(record.number, 222u);
  EXPECT_EQ(record.size, REAL_HEADER.size());
  EXPECT_EQ(record.headerLength, 198u);
  EXPECT_EQ(record.createdAt,
            1789551645u);  // decoded from the Common-Header timestamp
  EXPECT_EQ(record.serverId, 1u);
  // The version the greeting presents to replicas after a restart the
  // source is not up for.
  EXPECT_EQ(record.serverVersion, "8.4.11");
  EXPECT_EQ(record.checksumAlgorithm, "CRC32");
  EXPECT_FALSE(
      record.inUse);  // flags byte 0 => closed (rotated-away-from) file
  EXPECT_EQ(record.previousGtids.ToText(),
            "db06ab0a-39aa-11f1-90c3-19597335b2db:1-23883");
}

// Version < 5.6.1 has no checksum trailer, so the Previous_gtids_event body
// carries no trailing CRC to cut off.
std::vector<std::uint8_t> BuildEventHeader(std::uint8_t type,
                                           std::uint32_t serverId,
                                           std::uint32_t eventLength,
                                           std::uint32_t nextPosition,
                                           std::uint16_t flags) {
  std::vector<std::uint8_t> out(19, 0x00);
  out[4] = type;
  out[5] = static_cast<std::uint8_t>(serverId);
  // Full 4-byte little-endian fields (sEventHeader.hpp); writing only the
  // low two bytes would silently cap values at 0xFFFF.
  out[9] = static_cast<std::uint8_t>(eventLength);
  out[10] = static_cast<std::uint8_t>(eventLength >> 8);
  out[11] = static_cast<std::uint8_t>(eventLength >> 16);
  out[12] = static_cast<std::uint8_t>(eventLength >> 24);
  out[13] = static_cast<std::uint8_t>(nextPosition);
  out[14] = static_cast<std::uint8_t>(nextPosition >> 8);
  out[15] = static_cast<std::uint8_t>(nextPosition >> 16);
  out[16] = static_cast<std::uint8_t>(nextPosition >> 24);
  out[17] = static_cast<std::uint8_t>(flags);
  out[18] = static_cast<std::uint8_t>(flags >> 8);
  return out;
}

TEST(BinlogFileHeaderReaderTest,
     ReadsAPreChecksumHeaderWithNoCrcTrailerToStrip) {
  std::vector<std::uint8_t> fdeBody(57, 0x00);
  fdeBody[0] = 3;
  const std::string version = "5.5.62";
  for (std::size_t i = 0; i < version.size(); ++i)
    fdeBody[2 + i] = static_cast<std::uint8_t>(version[i]);
  fdeBody[56] = 19;

  const std::uint32_t fdeEventLength =
      static_cast<std::uint32_t>(19 + fdeBody.size());
  auto fdeHeader = BuildEventHeader(
      /*type=FormatDescription*/ 15, /*serverId=*/42, fdeEventLength,
      /*nextPosition=*/4 + fdeEventLength, /*flags=*/1 /*IN_USE*/);

  const auto pgeBody = GtidSet().Encode(
      /*skipTaggedGtids=*/false);  // 8 zero bytes, no checksum trailer
  const std::uint32_t pgeEventLength =
      static_cast<std::uint32_t>(19 + pgeBody.size());
  auto pgeHeader = BuildEventHeader(/*type=PreviousGtids*/ 35, /*serverId=*/42,
                                    pgeEventLength, /*nextPosition=*/0,
                                    /*flags=*/0);

  std::vector<std::uint8_t> file{0xfe, 0x62, 0x69, 0x6e};  // BINLOG_MAGIC
  file.insert(file.end(), fdeHeader.begin(), fdeHeader.end());
  file.insert(file.end(), fdeBody.begin(), fdeBody.end());
  file.insert(file.end(), pgeHeader.begin(), pgeHeader.end());
  file.insert(file.end(), pgeBody.begin(), pgeBody.end());

  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), file);

  StoredFileRecord record;
  std::string error;
  ASSERT_TRUE(BinlogFileHeaderReader::Read(fixture.Path("binlog.000001"),
                                           "binlog.000001", record, error))
      << error;
  EXPECT_EQ(record.checksumAlgorithm, "UNDEF");
  EXPECT_EQ(record.serverId, 42u);
  EXPECT_TRUE(record.inUse);
  EXPECT_TRUE(record.previousGtids.IsEmpty());
  EXPECT_EQ(record.headerLength,
            file.size());  // right after the Previous_gtids_event - nothing
                           // follows it here
}

TEST(BinlogFileHeaderReaderTest, RejectsAFileNotStartingWithTheBinlogMagic) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), std::vector<std::uint8_t>(30, 0x00));

  StoredFileRecord record;
  std::string error;
  EXPECT_FALSE(BinlogFileHeaderReader::Read(fixture.Path("binlog.000001"),
                                            "binlog.000001", record, error));
  EXPECT_FALSE(error.empty());
}

// eventLength near UINT32_MAX must be widened before "4 + eventLength" or
// it wraps, bypassing the bounds check - hence checking for this exact
// message, not just any failure.
TEST(BinlogFileHeaderReaderTest,
     RejectsAFormatDescriptionEventLengthThatWouldOverflowThirtyTwoBits) {
  auto fdeHeader = BuildEventHeader(/*type=FormatDescription*/ 15,
                                    /*serverId=*/1, /*eventLength=*/0xFFFFFFFF,
                                    /*nextPosition=*/0, /*flags=*/0);
  std::vector<std::uint8_t> file{0xfe, 0x62, 0x69, 0x6e};  // BINLOG_MAGIC
  file.insert(file.end(), fdeHeader.begin(), fdeHeader.end());
  file.insert(file.end(), 10,
              0x00);  // a short, unrelated tail - nowhere near 0xFFFFFFFF bytes

  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), file);

  StoredFileRecord record;
  std::string error;
  EXPECT_FALSE(BinlogFileHeaderReader::Read(fixture.Path("binlog.000001"),
                                            "binlog.000001", record, error));
  EXPECT_NE(error.find("runs past the end of the file"), std::string::npos)
      << error;
}

}  // namespace
}  // namespace binlog_streamer
