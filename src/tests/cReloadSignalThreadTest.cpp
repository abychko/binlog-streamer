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

#include "cReloadSignalThread.hpp"

#include <gtest/gtest.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace binlog_streamer {
namespace {

bool WaitFor(const std::atomic<int> &count, int expected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (count.load() < expected) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

class ReloadSignalThreadTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { ReloadSignalThread::BlockReloadSignal(); }
};

TEST_F(ReloadSignalThreadTest, EachSighupToTheProcessRunsTheHandler) {
  std::atomic<int> reloads{0};
  ReloadSignalThread thread([&reloads] { reloads.fetch_add(1); });
  thread.Start();
  ASSERT_EQ(kill(getpid(), SIGHUP), 0);
  ASSERT_TRUE(WaitFor(reloads, 1));
  ASSERT_EQ(kill(getpid(), SIGHUP), 0);
  ASSERT_TRUE(WaitFor(reloads, 2));
  thread.Stop();
  EXPECT_EQ(reloads.load(), 2);
}

TEST_F(ReloadSignalThreadTest, SighupBeforeStartIsHandledOnceStarted) {
  std::atomic<int> reloads{0};
  ReloadSignalThread thread([&reloads] { reloads.fetch_add(1); });
  ASSERT_EQ(kill(getpid(), SIGHUP), 0);
  thread.Start();
  EXPECT_TRUE(WaitFor(reloads, 1));
  thread.Stop();
}

TEST_F(ReloadSignalThreadTest, StopEndsTheThreadWithoutRunningTheHandler) {
  std::atomic<int> reloads{0};
  ReloadSignalThread thread([&reloads] { reloads.fetch_add(1); });
  thread.Start();
  thread.Stop();
  EXPECT_EQ(reloads.load(), 0);
}

// A parent that ignores SIGHUP (nohup, some launchers) passes SIG_IGN on;
// macOS then discards the SIGHUP Stop() sends, and join() never returns.
// Run in a child with an alarm, so a hang fails the test instead of it.
TEST(ReloadSignalThreadDeathTest, StopReturnsWhenSighupWasIgnoredByTheParent) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        alarm(5);
        signal(SIGHUP, SIG_IGN);
        ReloadSignalThread::BlockReloadSignal();
        ReloadSignalThread thread([] {});
        thread.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        thread.Stop();
        _exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

// A stop request is SIGTERM or SIGINT; one blocked by the parent would
// never reach the process.
TEST(ReloadSignalThreadDeathTest, StopSignalsBlockedByTheParentAreUnblocked) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        sigset_t inherited;
        sigemptyset(&inherited);
        sigaddset(&inherited, SIGTERM);
        sigaddset(&inherited, SIGINT);
        pthread_sigmask(SIG_BLOCK, &inherited, nullptr);
        ReloadSignalThread::BlockReloadSignal();
        sigset_t current;
        pthread_sigmask(SIG_BLOCK, nullptr, &current);
        _exit(sigismember(&current, SIGTERM) == 0 &&
                      sigismember(&current, SIGINT) == 0 &&
                      sigismember(&current, SIGHUP) == 1
                  ? 0
                  : 1);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_F(ReloadSignalThreadTest, StopWithoutStartDoesNothing) {
  ReloadSignalThread thread([] {});
  thread.Stop();
}

}  // namespace
}  // namespace binlog_streamer
