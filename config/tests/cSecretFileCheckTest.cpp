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

#include "cSecretFileCheck.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

constexpr uid_t EXPECTED_UID = 1000;
constexpr gid_t EXPECTED_GID = 2000;

struct stat MakeFileStatus(mode_t mode, uid_t uid = EXPECTED_UID,
                           gid_t gid = EXPECTED_GID) {
  struct stat status{};
  status.st_mode = S_IFREG | mode;
  status.st_uid = uid;
  status.st_gid = gid;
  return status;
}

struct stat MakeDirectoryStatus(mode_t mode, uid_t uid = EXPECTED_UID) {
  struct stat status{};
  status.st_mode = S_IFDIR | mode;
  status.st_uid = uid;
  return status;
}

// Manually-filled struct stat is used throughout: CheckFile and
// CheckDirectory are pure functions over stat, so the "foreign owner/group"
// cases do not need real files and do not need a root skip.

TEST(SecretFileCheckTest, AcceptsMode0640) {
  EXPECT_TRUE(SecretFileCheck::CheckFile(MakeFileStatus(0640), EXPECTED_UID,
                                         EXPECTED_GID)
                  .empty());
}

TEST(SecretFileCheckTest, AcceptsMode0600) {
  EXPECT_TRUE(SecretFileCheck::CheckFile(MakeFileStatus(0600), EXPECTED_UID,
                                         EXPECTED_GID)
                  .empty());
}

// Below, each rejection is checked against the exact violation(s) it must
// produce, not just non-emptiness: a regression that flags 0660 for the
// wrong reason would otherwise go unnoticed.

TEST(SecretFileCheckTest, RejectsOtherReadable0644) {
  EXPECT_EQ(SecretFileCheck::CheckFile(MakeFileStatus(0644), EXPECTED_UID,
                                       EXPECTED_GID),
            (std::vector<std::string>{"others have access"}));
}

TEST(SecretFileCheckTest, RejectsGroupWritable0660) {
  EXPECT_EQ(SecretFileCheck::CheckFile(MakeFileStatus(0660), EXPECTED_UID,
                                       EXPECTED_GID),
            (std::vector<std::string>{"group has write access"}));
}

TEST(SecretFileCheckTest, RejectsGroupAndOtherWritable0666) {
  EXPECT_EQ(SecretFileCheck::CheckFile(MakeFileStatus(0666), EXPECTED_UID,
                                       EXPECTED_GID),
            (std::vector<std::string>{"others have access",
                                      "group has write access"}));
}

TEST(SecretFileCheckTest, RejectsOtherReadable0604) {
  EXPECT_EQ(SecretFileCheck::CheckFile(MakeFileStatus(0604), EXPECTED_UID,
                                       EXPECTED_GID),
            (std::vector<std::string>{"others have access"}));
}

TEST(SecretFileCheckTest, RejectsSetuidBit04640) {
  EXPECT_EQ(SecretFileCheck::CheckFile(MakeFileStatus(04640), EXPECTED_UID,
                                       EXPECTED_GID),
            (std::vector<std::string>{"setuid, setgid or sticky bit is set"}));
}

TEST(SecretFileCheckTest, RejectsWrongOwner) {
  const auto errors = SecretFileCheck::CheckFile(
      MakeFileStatus(0640, EXPECTED_UID + 1, EXPECTED_GID), EXPECTED_UID,
      EXPECTED_GID);
  EXPECT_EQ(errors, (std::vector<std::string>{"wrong owner"}));
}

TEST(SecretFileCheckTest, RejectsWrongGroup) {
  const auto errors = SecretFileCheck::CheckFile(
      MakeFileStatus(0640, EXPECTED_UID, EXPECTED_GID + 1), EXPECTED_UID,
      EXPECTED_GID);
  EXPECT_EQ(errors, (std::vector<std::string>{"wrong group"}));
}

TEST(SecretFileCheckTest, RejectsNonRegularFile) {
  struct stat status{};
  status.st_mode = S_IFDIR | 0640;
  status.st_uid = EXPECTED_UID;
  status.st_gid = EXPECTED_GID;
  EXPECT_EQ(SecretFileCheck::CheckFile(status, EXPECTED_UID, EXPECTED_GID),
            (std::vector<std::string>{"not a regular file"}));
}

TEST(SecretFileCheckTest, AcceptsDirectoryMode0750) {
  EXPECT_TRUE(
      SecretFileCheck::CheckDirectory(MakeDirectoryStatus(0750), EXPECTED_UID)
          .empty());
}

TEST(SecretFileCheckTest, RejectsDirectoryGroupWritable0770) {
  EXPECT_EQ(
      SecretFileCheck::CheckDirectory(MakeDirectoryStatus(0770), EXPECTED_UID),
      (std::vector<std::string>{
          "directory mode allows group or other writes"}));
}

TEST(SecretFileCheckTest, RejectsDirectoryOtherWritable0757) {
  EXPECT_EQ(
      SecretFileCheck::CheckDirectory(MakeDirectoryStatus(0757), EXPECTED_UID),
      (std::vector<std::string>{
          "directory mode allows group or other writes"}));
}

TEST(SecretFileCheckTest, RejectsDirectoryWrongOwner) {
  EXPECT_EQ(SecretFileCheck::CheckDirectory(
                MakeDirectoryStatus(0750, EXPECTED_UID + 1), EXPECTED_UID),
            (std::vector<std::string>{"wrong directory owner"}));
}

}  // namespace
}  // namespace binlog_streamer
