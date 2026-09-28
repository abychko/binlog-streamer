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

#include "binlog/cTransactionBoundaryTracker.hpp"

#include <gtest/gtest.h>
#include <limits>
#include <string>
#include "binlog/eEventType.hpp"

namespace binlog_streamer {
namespace {

// Raw wire type code rather than EventType: some events below (Query,
// Table_map, Write_rows_v2, Xid) have no enumerator of their own.
EventHeader MakeHeader(std::uint8_t type, std::uint32_t eventLength) {
  EventHeader header;
  header.type = type;
  header.eventLength = eventLength;
  return header;
}

EventHeader MakeHeader(EventType type, std::uint32_t eventLength) {
  return MakeHeader(static_cast<std::uint8_t>(type), eventLength);
}

// Wire type codes for events used below that have no EventType enumerator
// of their own (percona-server dfc6d1f,
// libs/mysql/binlog/event/binlog_event.h:300,313,317,348).
constexpr std::uint8_t QUERY_EVENT_TYPE = 2;
constexpr std::uint8_t XID_EVENT_TYPE = 16;
constexpr std::uint8_t TABLE_MAP_EVENT_TYPE = 19;
constexpr std::uint8_t WRITE_ROWS_EVENT_TYPE = 30;

GtidEvent MakeGtidEvent(std::int64_t gno, std::uint64_t transactionLength,
                        bool hasTransactionLength = true) {
  GtidEvent event;
  event.gno = gno;
  event.transactionLength = transactionLength;
  event.hasTransactionLength = hasTransactionLength;
  return event;
}

TEST(TransactionBoundaryTrackerTest,
     TracksARealGroupOfSeveralEventsToItsExactEnd) {
  TransactionBoundaryTracker tracker;
  std::string error;

  const auto gtid = MakeGtidEvent(/*gno=*/22940, /*transactionLength=*/331);
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 103445, &gtid, error),
      BoundaryOutcome::GroupStart)
      << error;
  EXPECT_TRUE(tracker.InGroup());

  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(QUERY_EVENT_TYPE, 91), 103524, nullptr, error),
      BoundaryOutcome::InGroup)
      << error;
  EXPECT_EQ(tracker.OnEvent(MakeHeader(TABLE_MAP_EVENT_TYPE, 84), 103615,
                            nullptr, error),
            BoundaryOutcome::InGroup)
      << error;
  EXPECT_EQ(tracker.OnEvent(MakeHeader(WRITE_ROWS_EVENT_TYPE, 46), 103699,
                            nullptr, error),
            BoundaryOutcome::InGroup)
      << error;
  // This last event lands exactly at the group's end (103445 + 331) - it
  // must trip GroupEnd, not InGroup, catching a `<=` mutant on the InGroup
  // comparison.
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(XID_EVENT_TYPE, 31), 103745, nullptr, error),
      BoundaryOutcome::GroupEnd)
      << error;
  EXPECT_FALSE(tracker.InGroup());
}

// Same real group as above, its last event shifted one byte past the
// boundary its own GTID event promised (eventLength 32 instead of the real
// 31) - the sum overshoots groupEndOffset_ instead of landing on it.
TEST(TransactionBoundaryTrackerTest,
     AnEventCrossingTheGroupEndByOneByteIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto gtid = MakeGtidEvent(22940, 331);
  ASSERT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 103445, &gtid, error),
      BoundaryOutcome::GroupStart);
  ASSERT_EQ(
      tracker.OnEvent(MakeHeader(QUERY_EVENT_TYPE, 91), 103524, nullptr, error),
      BoundaryOutcome::InGroup);
  ASSERT_EQ(tracker.OnEvent(MakeHeader(TABLE_MAP_EVENT_TYPE, 84), 103615,
                            nullptr, error),
            BoundaryOutcome::InGroup);
  ASSERT_EQ(tracker.OnEvent(MakeHeader(WRITE_ROWS_EVENT_TYPE, 46), 103699,
                            nullptr, error),
            BoundaryOutcome::InGroup);

  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(XID_EVENT_TYPE, 32), 103745, nullptr, error),
      BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

TEST(TransactionBoundaryTrackerTest,
     AGtidEventWithoutATransactionLengthIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto gtid =
      MakeGtidEvent(1, /*transactionLength=*/0, /*hasTransactionLength=*/false);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 100, &gtid, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(tracker.InGroup());
}

TEST(TransactionBoundaryTrackerTest,
     AGtidEventWithAZeroTransactionLengthIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto gtid =
      MakeGtidEvent(1, /*transactionLength=*/0, /*hasTransactionLength=*/true);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 100, &gtid, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

TEST(TransactionBoundaryTrackerTest,
     AGtidEventWithoutADecodedGtidEventIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 100, nullptr, error),
      BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

TEST(TransactionBoundaryTrackerTest,
     AServiceEventInsideAnOpenGroupIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto gtid = MakeGtidEvent(1, 200);
  ASSERT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 1000, &gtid, error),
      BoundaryOutcome::GroupStart);

  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Rotate, 50), 1079, nullptr, error),
      BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(tracker.InGroup());  // left open - Reset() is the recovery, not
                                   // another event
}

TEST(TransactionBoundaryTrackerTest, AServiceEventOutsideAnyGroupIsStandalone) {
  TransactionBoundaryTracker tracker;
  std::string error;
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Rotate, 50), 500, nullptr, error),
      BoundaryOutcome::Standalone);
  EXPECT_FALSE(tracker.InGroup());
}

// A GTID event always opens a new group (percona-server dfc6d1f,
// trx_boundary_parser.cpp:305-337) - one arriving mid-group must not be
// read as an ordinary in-group event.
TEST(TransactionBoundaryTrackerTest,
     ANestedGtidEventInsideAnOpenGroupIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto firstGtid = MakeGtidEvent(1, 331);
  ASSERT_EQ(tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 103445, &firstGtid,
                            error),
            BoundaryOutcome::GroupStart);

  const auto secondGtid = MakeGtidEvent(2, 100);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 103524,
                            &secondGtid, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(tracker.InGroup());  // left open, same recovery contract as
                                   // AServiceEventInsideAnOpenGroupIsMalformed

  // AnonymousGtid interrupts an open group just as much as Gtid does.
  TransactionBoundaryTracker anotherTracker;
  ASSERT_EQ(anotherTracker.OnEvent(MakeHeader(EventType::Gtid, 79), 103445,
                                   &firstGtid, error),
            BoundaryOutcome::GroupStart);
  const auto anonymousGtid = MakeGtidEvent(0, 100);
  EXPECT_EQ(anotherTracker.OnEvent(MakeHeader(EventType::AnonymousGtid, 79),
                                   103524, &anonymousGtid, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

// error is cleared on the Standalone path just like every other non-
// Malformed outcome - a caller inspecting error only after a non-Malformed
// return must not see a message left over from an earlier, unrelated event.
TEST(TransactionBoundaryTrackerTest,
     StandaloneClearsAnErrorLeftBehindByAnEarlierMalformedEvent) {
  TransactionBoundaryTracker tracker;
  std::string error;
  ASSERT_EQ(tracker.OnEvent(MakeHeader(EventType::GtidTagged, 90), 500, nullptr,
                            error),
            BoundaryOutcome::Malformed);
  ASSERT_FALSE(error.empty());

  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Rotate, 50), 600, nullptr, error),
      BoundaryOutcome::Standalone);
  EXPECT_TRUE(error.empty());
}

TEST(TransactionBoundaryTrackerTest, ResetDropsAnOpenGroup) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto gtid = MakeGtidEvent(1, 200);
  ASSERT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 1000, &gtid, error),
      BoundaryOutcome::GroupStart);
  ASSERT_TRUE(tracker.InGroup());

  tracker.Reset();
  EXPECT_FALSE(tracker.InGroup());

  // The event that would have completed the dropped group (offset 1079,
  // ending exactly at the old groupEndOffset_ 1200) is now just an
  // ordinary event outside any group, not a leftover GroupEnd.
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(QUERY_EVENT_TYPE, 121), 1079, nullptr, error),
      BoundaryOutcome::Standalone);
}

TEST(TransactionBoundaryTrackerTest,
     ATaggedGtidEventStartsAGroupLikeAnUntaggedOne) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto decoded = MakeGtidEvent(1, 200);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::GtidTagged, 90), 1000,
                            &decoded, error),
            BoundaryOutcome::GroupStart);
  EXPECT_TRUE(tracker.InGroup());
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(QUERY_EVENT_TYPE, 110), 1090, nullptr, error),
      BoundaryOutcome::GroupEnd);
  EXPECT_FALSE(tracker.InGroup());
}

TEST(TransactionBoundaryTrackerTest,
     ATaggedGtidEventWithoutADecodedGtidEventIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::GtidTagged, 90), 1000,
                            nullptr, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

TEST(TransactionBoundaryTrackerTest,
     AnOrdinaryGtidEventWithANonPositiveGnoIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto zeroGno = MakeGtidEvent(/*gno=*/0, 200);
  EXPECT_EQ(
      tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 1000, &zeroGno, error),
      BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());

  const auto negativeGno = MakeGtidEvent(/*gno=*/-1, 200);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 1000, &negativeGno,
                            error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

// GNO_END (percona-server dfc6d1f, control_events.h:1248) is INT64_MAX
// itself, exclusive - catches a mutant computing the upper bound inclusive.
TEST(TransactionBoundaryTrackerTest,
     AnOrdinaryGtidEventWithAGnoAtTheUpperBoundIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto gnoAtUpperBound =
      MakeGtidEvent(/*gno=*/std::numeric_limits<std::int64_t>::max(), 200);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::Gtid, 79), 1000,
                            &gnoAtUpperBound, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

TEST(TransactionBoundaryTrackerTest,
     AnAnonymousGtidEventWithANonZeroGnoIsMalformed) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto nonZeroGno = MakeGtidEvent(/*gno=*/5, 200);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::AnonymousGtid, 79), 1000,
                            &nonZeroGno, error),
            BoundaryOutcome::Malformed);
  EXPECT_FALSE(error.empty());
}

TEST(TransactionBoundaryTrackerTest,
     AnAnonymousGtidEventWithAZeroGnoOpensAGroup) {
  TransactionBoundaryTracker tracker;
  std::string error;
  const auto zeroGno = MakeGtidEvent(/*gno=*/0, 150);
  EXPECT_EQ(tracker.OnEvent(MakeHeader(EventType::AnonymousGtid, 79), 1000,
                            &zeroGno, error),
            BoundaryOutcome::GroupStart)
      << error;
  EXPECT_TRUE(tracker.InGroup());
}

}  // namespace
}  // namespace binlog_streamer
