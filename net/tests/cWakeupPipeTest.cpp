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

#include "net/cWakeupPipe.hpp"

#include <gtest/gtest.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>

namespace binlog_streamer {
namespace {

TEST(WakeupPipeTest,
     ReadFdBecomesReadableAfterWakeAndStaysReadableUntilDrained) {
  WakeupPipe pipe;
  std::string error;
  ASSERT_TRUE(pipe.Open(error)) << error;
  EXPECT_TRUE(pipe.IsOpen());

  struct pollfd pfd{pipe.ReadFd(), POLLIN, 0};
  ASSERT_EQ(poll(&pfd, 1, 0), 0);

  pipe.Wake();
  pfd = {pipe.ReadFd(), POLLIN, 0};
  ASSERT_EQ(poll(&pfd, 1, 0), 1);
  EXPECT_NE(pfd.revents & POLLIN, 0);

  // A second Wake() before Drain() must not block (EAGAIN on an
  // already-pending byte is silently ignored) and must not leave more
  // than a normal amount of data queued for Drain() to consume.
  pipe.Wake();
  pipe.Drain();

  pfd = {pipe.ReadFd(), POLLIN, 0};
  EXPECT_EQ(poll(&pfd, 1, 0), 0);
}

TEST(WakeupPipeTest, BothEndsAreNonBlockingAndDistinctFds) {
  WakeupPipe pipe;
  std::string error;
  ASSERT_TRUE(pipe.Open(error)) << error;

  std::uint8_t buffer[1];
  // A non-blocking read on an empty pipe returns -1/EAGAIN immediately
  // rather than blocking this test forever if WakeupPipe::Open() ever
  // regressed on setting O_NONBLOCK.
  EXPECT_EQ(read(pipe.ReadFd(), buffer, sizeof(buffer)), -1);
  EXPECT_EQ(errno, EAGAIN);
}

}  // namespace
}  // namespace binlog_streamer
