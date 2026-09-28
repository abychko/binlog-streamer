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

#include "server/cServerUuidFile.hpp"

#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace binlog_streamer {
namespace {

constexpr char FIRST_UUID[] = "8a94f357-aab4-11df-86ab-c80aa9429562";
constexpr char SECOND_UUID[] = "3e11fa47-71ca-11e1-9e33-c80aa9429562";

class ServerUuidFileTest : public ::testing::Test {
 protected:
  std::filesystem::path dataDir;

  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    dataDir =
        std::filesystem::temp_directory_path() /
        ("server-uuid-test-" + std::to_string(getpid()) + "-" + info->name());
    std::filesystem::remove_all(dataDir);
    std::filesystem::create_directories(dataDir);
  }
  void TearDown() override { std::filesystem::remove_all(dataDir); }
};

TEST_F(ServerUuidFileTest,
       CreatesTheFileOnTheFirstStartAndKeepsItsUuidAfterwards) {
  std::string uuid;
  std::string error;
  ASSERT_TRUE(ServerUuidFile::LoadOrCreate(dataDir, FIRST_UUID, uuid, error))
      << error;
  EXPECT_EQ(uuid, FIRST_UUID);

  ASSERT_TRUE(ServerUuidFile::LoadOrCreate(dataDir, SECOND_UUID, uuid, error))
      << error;
  EXPECT_EQ(uuid, FIRST_UUID);
}

TEST_F(ServerUuidFileTest, WritesTheFormatAServerUsesForItsOwn) {
  std::string uuid;
  std::string error;
  ASSERT_TRUE(ServerUuidFile::LoadOrCreate(dataDir, FIRST_UUID, uuid, error))
      << error;
  std::ifstream in(dataDir / "auto.cnf");
  const std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
  EXPECT_EQ(content, std::string("[auto]\nserver-uuid=") + FIRST_UUID + "\n");
}

TEST_F(ServerUuidFileTest, RefusesAFileWithoutAValidUuidInsteadOfReplacingIt) {
  std::ofstream(dataDir / "auto.cnf") << "[auto]\nserver-uuid=not-a-uuid\n";
  std::string uuid;
  std::string error;
  EXPECT_FALSE(ServerUuidFile::LoadOrCreate(dataDir, FIRST_UUID, uuid, error));
  EXPECT_NE(error.find("no valid server-uuid"), std::string::npos) << error;
}

}  // namespace
}  // namespace binlog_streamer
