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

#include "cPublicKeyFileCheck.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

constexpr uid_t EXPECTED_UID = 1000;

struct stat MakeFileStatus(mode_t mode, uid_t uid = EXPECTED_UID) {
  struct stat status{};
  status.st_mode = S_IFREG | mode;
  status.st_uid = uid;
  status.st_gid =
      999999;  // arbitrary - PublicKeyFileCheck never looks at st_gid
  return status;
}

// Manually-filled struct stat: CheckFile is a pure function over stat, so
// "wrong owner" doesn't need a real file or a root skip to exercise -
// running this process as a different real uid needs root.

TEST(PublicKeyFileCheckTest, Accepts0644) {
  EXPECT_TRUE(PublicKeyFileCheck::CheckFile(MakeFileStatus(0644), EXPECTED_UID)
                  .empty());
}

TEST(PublicKeyFileCheckTest, Accepts0640) {
  EXPECT_TRUE(PublicKeyFileCheck::CheckFile(MakeFileStatus(0640), EXPECTED_UID)
                  .empty());
}

TEST(PublicKeyFileCheckTest, IgnoresGroupOwnership) {
  // st_gid above is an arbitrary value nowhere near a real expected group -
  // if CheckFile looked at it at all, every test in this file would fail.
  EXPECT_TRUE(PublicKeyFileCheck::CheckFile(MakeFileStatus(0644), EXPECTED_UID)
                  .empty());
}

TEST(PublicKeyFileCheckTest, RejectsGroupWritable0664) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(MakeFileStatus(0664), EXPECTED_UID),
            (std::vector<std::string>{"group has write access"}));
}

TEST(PublicKeyFileCheckTest, RejectsOtherWritable0646) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(MakeFileStatus(0646), EXPECTED_UID),
            (std::vector<std::string>{"others have write access"}));
}

TEST(PublicKeyFileCheckTest, RejectsGroupAndOtherWritable0666) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(MakeFileStatus(0666), EXPECTED_UID),
            (std::vector<std::string>{"others have write access",
                                      "group has write access"}));
}

TEST(PublicKeyFileCheckTest, RejectsSetuidBit04644) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(MakeFileStatus(04644), EXPECTED_UID),
            (std::vector<std::string>{"setuid, setgid or sticky bit is set"}));
}

TEST(PublicKeyFileCheckTest, RejectsSetgidBit02644) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(MakeFileStatus(02644), EXPECTED_UID),
            (std::vector<std::string>{"setuid, setgid or sticky bit is set"}));
}

TEST(PublicKeyFileCheckTest, RejectsStickyBit01644) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(MakeFileStatus(01644), EXPECTED_UID),
            (std::vector<std::string>{"setuid, setgid or sticky bit is set"}));
}

TEST(PublicKeyFileCheckTest, RejectsWrongOwner) {
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(
                MakeFileStatus(0644, EXPECTED_UID + 1), EXPECTED_UID),
            (std::vector<std::string>{"wrong owner"}));
}

TEST(PublicKeyFileCheckTest, RejectsNonRegularFile) {
  struct stat status{};
  status.st_mode = S_IFDIR | 0644;
  status.st_uid = EXPECTED_UID;
  EXPECT_EQ(PublicKeyFileCheck::CheckFile(status, EXPECTED_UID),
            (std::vector<std::string>{"not a regular file"}));
}

}  // namespace
}  // namespace binlog_streamer
