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

#include "server/cServerCertificateFiles.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include "net/cTlsContext.hpp"

namespace binlog_streamer {
namespace {

class ServerCertificateFilesTest : public ::testing::Test {
 protected:
  std::filesystem::path dataDir;

  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    dataDir = std::filesystem::temp_directory_path() /
              ("server-certificate-files-test-" + std::to_string(getpid()) +
               "-" + info->name());
    std::filesystem::remove_all(dataDir);
    std::filesystem::create_directories(dataDir);
  }
  void TearDown() override { std::filesystem::remove_all(dataDir); }

  mode_t ModeOf(std::string_view name) const {
    struct stat status{};
    stat((dataDir / name).c_str(), &status);
    return status.st_mode & 0777;
  }
  std::string ContentOf(std::string_view name) const {
    std::ifstream in(dataDir / name);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }
};

TEST_F(ServerCertificateFilesTest,
       GeneratesFourFilesOnTheFirstStartWithPrivateKeys) {
  TlsMaterial material;
  std::string error;
  ASSERT_TRUE(
      ServerCertificateFiles::LoadOrCreate(dataDir, "relay", material, error))
      << error;
  EXPECT_EQ(ModeOf(ServerCertificateFiles::CA_FILE_NAME), 0644u);
  EXPECT_EQ(ModeOf(ServerCertificateFiles::CA_KEY_FILE_NAME), 0600u);
  EXPECT_EQ(ModeOf(ServerCertificateFiles::CERT_FILE_NAME), 0644u);
  EXPECT_EQ(ModeOf(ServerCertificateFiles::KEY_FILE_NAME), 0600u);
  EXPECT_EQ(material.caPem, ContentOf(ServerCertificateFiles::CA_FILE_NAME));
  EXPECT_EQ(material.certPem,
            ContentOf(ServerCertificateFiles::CERT_FILE_NAME));
  EXPECT_EQ(material.keyPem, ContentOf(ServerCertificateFiles::KEY_FILE_NAME));
  EXPECT_TRUE(TlsContext::Check(material, error)) << error;
  TlsContext context;
  EXPECT_TRUE(context.LoadServer(material, error)) << error;
}

TEST_F(ServerCertificateFilesTest, ReadsTheSameFilesOnTheNextStart) {
  TlsMaterial first;
  std::string error;
  ASSERT_TRUE(
      ServerCertificateFiles::LoadOrCreate(dataDir, "relay", first, error))
      << error;
  TlsMaterial second;
  ASSERT_TRUE(
      ServerCertificateFiles::LoadOrCreate(dataDir, "relay", second, error))
      << error;
  EXPECT_EQ(second.caPem, first.caPem);
  EXPECT_EQ(second.certPem, first.certPem);
  EXPECT_EQ(second.keyPem, first.keyPem);
}

TEST_F(ServerCertificateFilesTest, KeepsACaTheConfigurationAlreadyNamed) {
  TlsMaterial material;
  material.caPem = "configured ca";
  std::string error;
  ASSERT_TRUE(
      ServerCertificateFiles::LoadOrCreate(dataDir, "relay", material, error))
      << error;
  EXPECT_EQ(material.caPem, "configured ca");
  EXPECT_TRUE(
      std::filesystem::exists(dataDir / ServerCertificateFiles::CA_FILE_NAME));
}

TEST_F(ServerCertificateFilesTest, RefusesACertificateWithoutItsKey) {
  TlsMaterial material;
  std::string error;
  ASSERT_TRUE(
      ServerCertificateFiles::LoadOrCreate(dataDir, "relay", material, error))
      << error;
  std::filesystem::remove(dataDir / ServerCertificateFiles::KEY_FILE_NAME);
  TlsMaterial again;
  EXPECT_FALSE(
      ServerCertificateFiles::LoadOrCreate(dataDir, "relay", again, error));
  EXPECT_NE(error.find("server-key.pem is missing"), std::string::npos)
      << error;
  EXPECT_NE(error.find("remove both"), std::string::npos) << error;
}

}  // namespace
}  // namespace binlog_streamer
