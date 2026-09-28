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

#include "storage/cFileCursor.hpp"

#include "cTempDirectoryFixture.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageReader.hpp"
#include "storage/sStoredFileRecord.hpp"

#include <dirent.h>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

// FileCursor's constructor is private; every test obtains one the only way
// a real caller can, through StorageReader::Open().
void WriteFile(const std::filesystem::path &path, std::string_view content) {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

// Only ever used for the difference between two calls: opendir()/readdir()
// themselves add transient entries.
std::size_t CountOpenFileDescriptors() {
  DIR *dir = opendir("/dev/fd");
  if (dir == nullptr) throw std::runtime_error("opendir(/dev/fd) failed");
  std::size_t count = 0;
  while (readdir(dir) != nullptr) ++count;
  closedir(dir);
  return count;
}

// Two separate parameters instead of `*self = std::move(*self)`: -Wself-move
// flags the literal expression under -Werror, while this indirection still
// exercises operator=()'s self-assignment guard at runtime.
void MoveAssign(FileCursor &to, FileCursor &from) { to = std::move(from); }

TEST(FileCursorTest, DefaultConstructedCursorHasNoFileName) {
  FileCursor cursor;
  EXPECT_EQ(cursor.FileName(), "");
}

TEST(FileCursorTest,
     MoveAssignmentReleasesTheOverwrittenPinAndSurvivesSelfAssignment) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "first");
  WriteFile(fixture.Path("binlog.000002"), "second");
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name = "binlog.000001";
  catalog.Add(first);
  StoredFileRecord second;
  second.name = "binlog.000002";
  catalog.Add(second);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  auto firstCursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(firstCursor) << error;
  auto secondCursor = reader.Open("binlog.000002", error);
  ASSERT_TRUE(secondCursor) << error;

  *firstCursor = std::move(*secondCursor);
  EXPECT_EQ(firstCursor->FileName(), "binlog.000002");

  // binlog.000001 is unpinned now; Remove() is oldest-first.
  EXPECT_TRUE(catalog.Remove(error)) << error;
  EXPECT_FALSE(catalog.Remove(error));
  EXPECT_EQ(error, "binlog.000002 is pinned by 1 reader(s)");

  // Self-assignment must not close the fd or release the pin.
  const std::size_t fdsBeforeSelfMove = CountOpenFileDescriptors();
  MoveAssign(*firstCursor, *firstCursor);
  EXPECT_EQ(CountOpenFileDescriptors(), fdsBeforeSelfMove);
  EXPECT_FALSE(catalog.Remove(error));
  EXPECT_EQ(error, "binlog.000002 is pinned by 1 reader(s)");

  firstCursor.reset();
  EXPECT_TRUE(catalog.Remove(error)) << error;
  EXPECT_EQ(catalog.Size(), 0u);
}

TEST(FileCursorTest, DestroyingACursorReleasesItsPin) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "x");
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  std::string error;
  {
    auto cursor = reader.Open("binlog.000001", error);
    ASSERT_TRUE(cursor) << error;
    EXPECT_FALSE(catalog.Remove(error));
  }
  EXPECT_TRUE(catalog.Remove(error)) << error;
}

// Checked via the process's open-descriptor count since Fd() is private.
TEST(FileCursorTest, DestroyingACursorClosesItsFileDescriptor) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "x");
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  catalog.Add(record);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  const std::size_t before = CountOpenFileDescriptors();
  std::string error;
  {
    auto cursor = reader.Open("binlog.000001", error);
    ASSERT_TRUE(cursor) << error;
    EXPECT_EQ(CountOpenFileDescriptors(), before + 1);
  }
  EXPECT_EQ(CountOpenFileDescriptors(), before);
}

TEST(FileCursorTest, MoveAssignmentClosesTheDescriptorItOverwrites) {
  TempDirectoryFixture fixture;
  WriteFile(fixture.Path("binlog.000001"), "first");
  WriteFile(fixture.Path("binlog.000002"), "second");
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name = "binlog.000001";
  catalog.Add(first);
  StoredFileRecord second;
  second.name = "binlog.000002";
  catalog.Add(second);
  PublishedPositionTracker published;
  StorageReader reader(fixture.Directory(), catalog, published);

  const std::size_t before = CountOpenFileDescriptors();
  std::string error;
  auto firstCursor = reader.Open("binlog.000001", error);
  ASSERT_TRUE(firstCursor) << error;
  auto secondCursor = reader.Open("binlog.000002", error);
  ASSERT_TRUE(secondCursor) << error;
  ASSERT_EQ(CountOpenFileDescriptors(), before + 2);

  *firstCursor = std::move(*secondCursor);
  EXPECT_EQ(CountOpenFileDescriptors(), before + 1);

  firstCursor.reset();
  EXPECT_EQ(CountOpenFileDescriptors(), before);
}

}  // namespace
}  // namespace binlog_streamer
