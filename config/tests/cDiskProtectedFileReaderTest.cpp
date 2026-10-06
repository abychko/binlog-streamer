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
#include "config/cDiskProtectedFileReader.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace binlog_streamer {
namespace {

using test::ProtectedFileFixture;

DiskProtectedFileReader MakeReader() {
  return DiskProtectedFileReader(ProtectedFileFixture::CurrentUserName(),
                                 ProtectedFileFixture::CurrentGroupName());
}

TEST(DiskProtectedFileReaderTest, ReadsWholeFileAcrossMultipleReadCalls) {
  ProtectedFileFixture fixture;
  const std::string content(20000, 'x');
  const auto path = fixture.WriteFile("secret.yml", content);
  auto reader = MakeReader();
  std::string readBack;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read(path.string(), readBack, errors),
            ProtectedFileStatus::Ok);
  EXPECT_TRUE(errors.empty());
  EXPECT_EQ(readBack, content);
}

TEST(DiskProtectedFileReaderTest, AbsentFileHasNoErrors) {
  ProtectedFileFixture fixture;
  auto reader = MakeReader();
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read((fixture.Directory() / "missing.yml").string(), content,
                        errors),
            ProtectedFileStatus::Absent);
  EXPECT_TRUE(errors.empty());
}

TEST(DiskProtectedFileReaderTest, SymbolicLinkFails) {
  ProtectedFileFixture fixture;
  const auto target = fixture.WriteFile("real.yml", "host: db\n");
  const auto link = fixture.Directory() / "link.yml";
  ASSERT_EQ(symlink(target.c_str(), link.c_str()), 0);
  auto reader = MakeReader();
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read(link.string(), content, errors),
            ProtectedFileStatus::Failed);
  EXPECT_FALSE(errors.empty());
}

TEST(DiskProtectedFileReaderTest, DirectoryInsteadOfFileFails) {
  ProtectedFileFixture fixture;
  const auto subdirectory = fixture.Directory() / "sub.yml";
  ASSERT_EQ(mkdir(subdirectory.c_str(), 0700), 0);
  auto reader = MakeReader();
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read(subdirectory.string(), content, errors),
            ProtectedFileStatus::Failed);
  ASSERT_EQ(errors.size(), 1u);
  // Text checked: without the S_ISREG check, read() on a directory fails with
  // EISDIR and the test would still pass.
  EXPECT_NE(errors[0].message.find("not a regular file"), std::string::npos);
}

TEST(DiskProtectedFileReaderTest, FifoFailsWithoutBlocking) {
  ProtectedFileFixture fixture;
  const auto fifo = fixture.Directory() / "pipe.yml";
  ASSERT_EQ(mkfifo(fifo.c_str(), 0640), 0);
  auto reader = MakeReader();
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read(fifo.string(), content, errors),
            ProtectedFileStatus::Failed);
  EXPECT_FALSE(errors.empty());
}

TEST(DiskProtectedFileReaderTest, DirectoryViolationErrorNamesTheDirectory) {
  ProtectedFileFixture fixture;
  fixture.WriteFile("secret.yml", "host: db\n");
  ASSERT_EQ(chmod(fixture.Directory().c_str(), 0777), 0);
  auto reader = MakeReader();
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read((fixture.Directory() / "secret.yml").string(), content,
                        errors),
            ProtectedFileStatus::Failed);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].file, fixture.Directory().string());
}

TEST(DiskProtectedFileReaderTest, ModeViolationMessageNamesTheFixCommand) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("secret.yml", "host: db\n", 0644);
  auto reader = MakeReader();
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read(path.string(), content, errors),
            ProtectedFileStatus::Failed);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(errors[0].message.find("chown"), std::string::npos);
  EXPECT_NE(errors[0].message.find("chmod 0640"), std::string::npos);
}

TEST(DiskProtectedFileReaderTest,
     MissingGroupAndModeViolationCombineIntoOneError) {
  ProtectedFileFixture fixture;
  const auto path = fixture.WriteFile("secret.yml", "host: db\n", 0644);
  DiskProtectedFileReader reader(ProtectedFileFixture::CurrentUserName(),
                                 "binlog-streamer-no-such-group");
  std::string content;
  std::vector<ConfigError> errors;
  EXPECT_EQ(reader.Read(path.string(), content, errors),
            ProtectedFileStatus::Failed);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_NE(
      errors[0].message.find("binlog-streamer-no-such-group does not exist"),
      std::string::npos);
  EXPECT_NE(errors[0].message.find("others have access"), std::string::npos);
  EXPECT_EQ(errors[0].message.find("wrong group"), std::string::npos);
}

}  // namespace
}  // namespace binlog_streamer
