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

#include <gtest/gtest.h>
#include <array>
#include <fstream>
#include <string>
#include "cTempDirectoryFixture.hpp"
#include "storage/cReadOnlyBinlogFile.hpp"

namespace binlog_streamer {
namespace {
TEST(ReadOnlyBinlogFileTest, ReadsExactRangesAndRejectsTruncatedRanges) {
  test::TempDirectoryFixture directory;
  const auto path = directory.Path("bytes");
  {
    std::ofstream file(path, std::ios::binary);
    file << "abcdef";
  }
  ReadOnlyBinlogFile reader;
  std::string error;
  ASSERT_TRUE(reader.Open(path.string(), error)) << error;
  std::array<std::uint8_t, 3> bytes{};
  ASSERT_TRUE(reader.ReadAt(2, bytes, error)) << error;
  EXPECT_EQ((std::array<std::uint8_t, 3>{'c', 'd', 'e'}), bytes);
  EXPECT_FALSE(reader.ReadAt(4, bytes, error));
  EXPECT_FALSE(error.empty());
  error.clear();
  EXPECT_TRUE(reader.ReadAt(6, {}, error));
  EXPECT_TRUE(error.empty());
}
TEST(ReadOnlyBinlogFileTest, FailedReopenPreservesThePreviouslyOpenedFile) {
  test::TempDirectoryFixture directory;
  const auto path = directory.Path("bytes");
  {
    std::ofstream file(path, std::ios::binary);
    file << "x";
  }
  ReadOnlyBinlogFile reader;
  std::string error;
  ASSERT_TRUE(reader.Open(path.string(), error));
  EXPECT_FALSE(reader.Open(directory.Path("missing").string(), error));
  error.clear();
  std::array<std::uint8_t, 1> bytes{};
  ASSERT_TRUE(reader.ReadAt(0, bytes, error)) << error;
  EXPECT_EQ(bytes[0], 'x');
}
}  // namespace
}  // namespace binlog_streamer
