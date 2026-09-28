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

#include "receiver/cSourceClock.hpp"
#include "receiver/sSourceIdentity.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace binlog_streamer {
namespace {

using namespace std::chrono_literals;
const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
constexpr std::uint64_t ts = 1'700'000'000;

TEST(SourceClockTest, UnknownWithoutTimestamp) {
  const SourceClock clock;
  EXPECT_FALSE(clock.Known());
  EXPECT_EQ(clock.Now(t0), std::nullopt);
  const SourceClock zero(0, t0);
  EXPECT_FALSE(zero.Known());
  EXPECT_EQ(zero.Now(t0), std::nullopt);
}

TEST(SourceClockTest, AnchorKeepsTimestamp) {
  EXPECT_EQ(SourceClock(ts, t0).Now(t0), ts);
}

TEST(SourceClockTest, FractionalSecondRoundsDown) {
  EXPECT_EQ(SourceClock(ts, t0).Now(t0 + 999ms), ts);
}

TEST(SourceClockTest, WholeSecondAdvances) {
  EXPECT_EQ(SourceClock(ts, t0).Now(t0 + 1000ms), ts + 1);
}

TEST(SourceClockTest, WeekAdvancesWithoutFraction) {
  EXPECT_EQ(SourceClock(ts, t0).Now(t0 + 7 * 24h + 500ms), ts + 604800);
}

TEST(SourceClockTest, BeforeAnchorKeepsTimestamp) {
  EXPECT_EQ(SourceClock(ts, t0).Now(t0 - 5s), ts);
}

TEST(SourceClockTest, OverflowSaturates) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(SourceClock(maximum - 1, t0).Now(t0 + 5s), maximum);
}

TEST(SourceClockTest, IdentitySuppliesTimestampAndAnchor) {
  SourceIdentity identity;
  identity.unixTimestamp = ts;
  identity.unixTimestampReadAt = t0;
  const auto clock = SourceClock::FromIdentity(identity);
  EXPECT_TRUE(clock.Known());
  EXPECT_EQ(clock.Now(t0 + 10s), ts + 10);
  identity.unixTimestamp = 0;
  EXPECT_FALSE(SourceClock::FromIdentity(identity).Known());
}

TEST(SourceClockTest, CurrentTimeUsesMonotonicClock) {
  const SourceClock clock(ts, std::chrono::steady_clock::now());
  const auto now = clock.Now();
  ASSERT_TRUE(now.has_value());
  EXPECT_GE(*now, ts);
  EXPECT_LE(*now, ts + 1);
}

}  // namespace
}  // namespace binlog_streamer
