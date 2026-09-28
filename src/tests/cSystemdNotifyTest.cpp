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

#include "cSystemdNotify.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace binlog_streamer {
namespace {

class SystemdNotifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/bs-notify-XXXXXX";
    ASSERT_NE(mkdtemp(pattern), nullptr);
    m_dir = pattern;
    m_path = m_dir + "/notify";
    m_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(m_fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, m_path.c_str());
    ASSERT_EQ(bind(m_fd, reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address)),
              0);
  }

  void TearDown() override {
    unsetenv("NOTIFY_SOCKET");
    if (m_fd >= 0) close(m_fd);
    unlink(m_path.c_str());
    rmdir(m_dir.c_str());
  }

  std::string Received() {
    char buffer[64] = {};
    const ssize_t size = recv(m_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    return size < 0 ? std::string() : std::string(buffer, size);
  }

  std::string m_dir;
  std::string m_path;
  int m_fd = -1;
};

TEST_F(SystemdNotifyTest, SendsTheMessageToNotifySocket) {
  setenv("NOTIFY_SOCKET", m_path.c_str(), 1);
  EXPECT_EQ(SystemdNotify::Send("READY=1"), std::nullopt);
  EXPECT_EQ(Received(), "READY=1");
}

TEST_F(SystemdNotifyTest, DoesNothingWithoutNotifySocket) {
  unsetenv("NOTIFY_SOCKET");
  EXPECT_EQ(SystemdNotify::Send("READY=1"), std::nullopt);
  EXPECT_EQ(Received(), "");
}

TEST_F(SystemdNotifyTest, ReportsASocketNobodyListensOn) {
  const std::string missing = m_dir + "/missing";
  setenv("NOTIFY_SOCKET", missing.c_str(), 1);
  EXPECT_NE(SystemdNotify::Send("READY=1"), std::nullopt);
}

TEST_F(SystemdNotifyTest, RefusesANameThatIsNotAPath) {
  setenv("NOTIFY_SOCKET", "notify", 1);
  EXPECT_NE(SystemdNotify::Send("READY=1"), std::nullopt);
}

}  // namespace
}  // namespace binlog_streamer
