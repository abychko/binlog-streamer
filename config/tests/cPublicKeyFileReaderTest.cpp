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

#include "cProtectedFileFixture.hpp"
#include "cPublicKeyFileReader.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace binlog_streamer {
namespace {

using test::ProtectedFileFixture;

PublicKeyFileReader MakeReader() {
  return PublicKeyFileReader(ProtectedFileFixture::CurrentUserName());
}

TEST(PublicKeyFileReaderTest, ReadsWholeFileAcrossMultipleReadCalls) {
  ProtectedFileFixture fixture;
  // Larger than the reader's internal 8192-byte read buffer, so the read loop
  // must iterate more than once.
  const std::string content(20000, 'x');
  const auto path = fixture.WriteFile("key.pem", content, 0644);
  auto reader = MakeReader();
  std::string readBack;
  std::string error;
  EXPECT_EQ(reader.Read(path.string(), readBack, error),
            ProtectedFileStatus::Ok);
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(readBack, content);
}

TEST(PublicKeyFileReaderTest, Accepts0644) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("key.pem", "pem content\n", 0644);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(path.string(), content, error),
            ProtectedFileStatus::Ok);
  EXPECT_TRUE(error.empty());
}

TEST(PublicKeyFileReaderTest, AbsentFileHasNoError) {
  ProtectedFileFixture fixture;
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read((fixture.Directory() / "missing.pem").string(), content,
                        error),
            ProtectedFileStatus::Absent);
  EXPECT_TRUE(error.empty());
}

TEST(PublicKeyFileReaderTest, SymbolicLinkFails) {
  ProtectedFileFixture fixture;
  const auto target = fixture.WriteFile("real.pem", "pem content\n", 0644);
  const auto link = fixture.Directory() / "link.pem";
  ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(link.string(), content, error),
            ProtectedFileStatus::Failed);
  EXPECT_FALSE(error.empty());
}

TEST(PublicKeyFileReaderTest, DirectoryInsteadOfFileFails) {
  ProtectedFileFixture fixture;
  const auto subdirectory = fixture.Directory() / "sub.pem";
  ASSERT_EQ(mkdir(subdirectory.c_str(), 0700), 0);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(subdirectory.string(), content, error),
            ProtectedFileStatus::Failed);
  // Text checked, not just non-emptiness: without the S_ISREG check,
  // read() on a directory fails with EISDIR and this test would still
  // pass for the wrong reason.
  EXPECT_NE(error.find("not a regular file"), std::string::npos);
}

TEST(PublicKeyFileReaderTest, GroupWritableFileFails) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("key.pem", "pem content\n", 0664);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(path.string(), content, error),
            ProtectedFileStatus::Failed);
  EXPECT_NE(error.find("group has write access"), std::string::npos);
}

TEST(PublicKeyFileReaderTest, OtherWritableFileFails) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("key.pem", "pem content\n", 0646);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(path.string(), content, error),
            ProtectedFileStatus::Failed);
  EXPECT_NE(error.find("others have write access"), std::string::npos);
}

TEST(PublicKeyFileReaderTest, SetuidFileFails) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("key.pem", "pem content\n", 04644);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(path.string(), content, error),
            ProtectedFileStatus::Failed);
  EXPECT_NE(error.find("setuid, setgid or sticky bit is set"),
            std::string::npos);
}

TEST(PublicKeyFileReaderTest, GroupWritableDirectoryFails) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("key.pem", "pem content\n", 0644);
  ASSERT_EQ(chmod(fixture.Directory().c_str(), 0770), 0);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(
      reader.Read((fixture.Directory() / "key.pem").string(), content, error),
      ProtectedFileStatus::Failed);
  EXPECT_NE(error.find("directory mode allows group or other writes"),
            std::string::npos);
}

TEST(PublicKeyFileReaderTest, OtherWritableDirectoryFails) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("key.pem", "pem content\n", 0644);
  ASSERT_EQ(chmod(fixture.Directory().c_str(), 0707), 0);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(
      reader.Read((fixture.Directory() / "key.pem").string(), content, error),
      ProtectedFileStatus::Failed);
  EXPECT_NE(error.find("directory mode allows group or other writes"),
            std::string::npos);
}

TEST(PublicKeyFileReaderTest, ModeViolationMessageNamesTheFixCommand) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("key.pem", "pem content\n", 0664);
  auto reader = MakeReader();
  std::string content;
  std::string error;
  EXPECT_EQ(reader.Read(path.string(), content, error),
            ProtectedFileStatus::Failed);
  EXPECT_NE(error.find("chown"), std::string::npos);
  EXPECT_NE(error.find("chmod go-w"), std::string::npos);
}

}  // namespace
}  // namespace binlog_streamer
