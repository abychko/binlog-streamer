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

#include "storage/cPublishedPositionTracker.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace binlog_streamer {
namespace {

TEST(PublishedPositionTrackerTest, StartsAtTheDefaultPosition) {
  PublishedPositionTracker tracker;
  const PublishedPosition current = tracker.Current();
  EXPECT_EQ(current.fileName, "");
  EXPECT_EQ(current.position, 0u);
}

TEST(PublishedPositionTrackerTest, CurrentReflectsTheMostRecentAdvance) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 116);
  tracker.Advance("binlog.000001", 241);
  const PublishedPosition current = tracker.Current();
  EXPECT_EQ(current.fileName, "binlog.000001");
  EXPECT_EQ(current.position, 241u);
}

TEST(PublishedPositionTrackerTest, TheMarksFileChangesOnlyWithTheFile) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 116);
  const PublishedMark first = tracker.Mark();
  tracker.Advance("binlog.000001", 241);
  const PublishedMark later = tracker.Mark();
  EXPECT_EQ(later.file, first.file);
  EXPECT_EQ(later.position, 241u);
  tracker.Advance("binlog.000002", 4);
  const PublishedMark next = tracker.Mark();
  EXPECT_NE(next.file, first.file);
  EXPECT_EQ(next.position, 4u);
}

TEST(PublishedPositionTrackerTest, LocateTellsWhetherAFileIsThePublishedOne) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000002", 300);
  bool published = false;
  const PublishedMark own = tracker.Locate("binlog.000002", published);
  EXPECT_TRUE(published);
  EXPECT_EQ(own.position, 300u);
  EXPECT_EQ(own.file, tracker.Mark().file);
  const PublishedMark other = tracker.Locate("binlog.000001", published);
  EXPECT_FALSE(published);
  EXPECT_EQ(other.file, own.file);
}

TEST(PublishedPositionTrackerTest, MarkNeverPairsOneFilesNumberWithAnother) {
  PublishedPositionTracker tracker;
  constexpr std::uint64_t STEP = 1000;
  constexpr std::uint64_t FILES = 200;
  tracker.Advance("binlog.1", STEP);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (std::uint64_t file = 1; file <= FILES; ++file)
      for (std::uint64_t n = 0; n < 50; ++n)
        tracker.Advance("binlog." + std::to_string(file), file * STEP + n);
    done.store(true, std::memory_order_release);
  });
  int torn = 0;
  while (!done.load(std::memory_order_acquire)) {
    const PublishedMark mark = tracker.Mark();
    if (mark.position / STEP != mark.file) ++torn;
  }
  writer.join();
  EXPECT_EQ(torn, 0);
}

TEST(PublishedPositionTrackerTest,
     WaitReturnsImmediatelyWhenAlreadyPastTheTarget) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 300);
  const auto outcome = tracker.Wait(PublishedPosition{"binlog.000001", 241},
                                    std::chrono::milliseconds(0));
  EXPECT_EQ(outcome, WaitOutcome::Advanced);
}

TEST(PublishedPositionTrackerTest,
     WaitWakesUpWhenAnotherThreadAdvancesPastTheTarget) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 241);

  std::atomic<bool> started{false};
  WaitOutcome outcome = WaitOutcome::TimedOut;
  std::thread waiter([&] {
    started.store(true, std::memory_order_release);
    outcome = tracker.Wait(PublishedPosition{"binlog.000001", 241},
                           std::chrono::seconds(5));
  });
  while (!started.load(std::memory_order_acquire)) {
  }
  tracker.Advance("binlog.000001", 366);
  waiter.join();

  EXPECT_EQ(outcome, WaitOutcome::Advanced);
}

// A timed-out-then-polled wait would also find the new position, so the
// elapsed time, not just the outcome, is what proves the wake happened.
TEST(PublishedPositionTrackerTest,
     AdvanceWakesAReaderThatHasStoppedPollingAndBlocked) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 241);

  WaitOutcome outcome = WaitOutcome::TimedOut;
  std::chrono::steady_clock::duration waited{};
  std::thread waiter([&] {
    const auto startedAt = std::chrono::steady_clock::now();
    outcome = tracker.Wait(PublishedPosition{"binlog.000001", 241},
                           std::chrono::seconds(10));
    waited = std::chrono::steady_clock::now() - startedAt;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  tracker.Advance("binlog.000001", 366);
  waiter.join();

  EXPECT_EQ(outcome, WaitOutcome::Advanced);
  EXPECT_LT(waited, std::chrono::seconds(5));
}

TEST(PublishedPositionTrackerTest, APollingReaderNoticesAnAdvanceByItself) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 241);

  std::atomic<bool> stop{false};
  std::thread publisher([&] {
    std::uint64_t position = 241;
    while (!stop.load()) tracker.Advance("binlog.000001", ++position);
  });
  for (int i = 0; i < 200; ++i) {
    const PublishedPosition seen = tracker.Current();
    const auto startedAt = std::chrono::steady_clock::now();
    const WaitOutcome outcome = tracker.Wait(seen, std::chrono::seconds(10));
    ASSERT_EQ(outcome, WaitOutcome::Advanced);
    ASSERT_LT(std::chrono::steady_clock::now() - startedAt,
              std::chrono::seconds(5));
  }
  stop.store(true);
  publisher.join();
}

class PublishedPositionTrackerStyleTest
    : public ::testing::TestWithParam<WaitStyle> {};

// An iteration whose Wait() took the full timeout is a lost wakeup even if
// it returns Advanced: the predicate re-checks at the deadline too.
TEST_P(PublishedPositionTrackerStyleTest, NoLostWakeupsAcrossManyIterations) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 0);

  constexpr int ITERATIONS = 1000;
  constexpr auto TIMEOUT = std::chrono::milliseconds(200);
  int losses = 0;
  for (int i = 0; i < ITERATIONS; ++i) {
    const PublishedPosition target = tracker.Current();
    std::atomic<bool> started{false};
    std::chrono::steady_clock::duration waited{};
    std::thread waiter([&] {
      started.store(true, std::memory_order_release);
      const auto begin = std::chrono::steady_clock::now();
      tracker.Wait(target, TIMEOUT, GetParam());
      waited = std::chrono::steady_clock::now() - begin;
    });
    while (!started.load(std::memory_order_acquire)) {
    }
    tracker.Advance(target.fileName, target.position + 1);
    waiter.join();
    if (waited >= TIMEOUT) ++losses;
  }

  EXPECT_EQ(losses, 0) << "a correct Wait() is notified well before its own "
                          "timeout, regardless of scheduling";
}

TEST_P(PublishedPositionTrackerStyleTest, WaitTimesOutWithoutAnyAdvance) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 241);
  const auto startedAt = std::chrono::steady_clock::now();
  const auto outcome = tracker.Wait(PublishedPosition{"binlog.000001", 241},
                                    std::chrono::milliseconds(50), GetParam());
  EXPECT_EQ(outcome, WaitOutcome::TimedOut);
  EXPECT_GE(std::chrono::steady_clock::now() - startedAt,
            std::chrono::milliseconds(50));
  const PublishedPosition current = tracker.Current();
  EXPECT_EQ(current.fileName, "binlog.000001");
  EXPECT_EQ(current.position, 241u);
}

TEST_P(PublishedPositionTrackerStyleTest,
       WaitWakesUpWhenPublishedMovesToADifferentFile) {
  PublishedPositionTracker tracker;
  tracker.Advance("binlog.000001", 241);
  std::atomic<bool> started{false};
  WaitOutcome outcome = WaitOutcome::TimedOut;
  std::thread waiter([&] {
    started.store(true, std::memory_order_release);
    outcome = tracker.Wait(PublishedPosition{"binlog.000001", 241},
                           std::chrono::seconds(5), GetParam());
  });
  while (!started.load(std::memory_order_acquire)) {
  }
  tracker.Advance("binlog.000002", 116);
  waiter.join();
  EXPECT_EQ(outcome, WaitOutcome::Advanced);
}

INSTANTIATE_TEST_SUITE_P(BothStyles, PublishedPositionTrackerStyleTest,
                         ::testing::Values(WaitStyle::PollFirst,
                                           WaitStyle::Block),
                         [](const ::testing::TestParamInfo<WaitStyle> &info) {
                           return info.param == WaitStyle::Block ? "Block"
                                                                 : "PollFirst";
                         });

}  // namespace
}  // namespace binlog_streamer
