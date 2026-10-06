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

#include "status/cStreamProgress.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(StreamProgressTest, StartsEmptyAndFollowsTheStream) {
  StreamProgress progress;
  EXPECT_EQ(progress.Read(), StreamPoint{});

  progress.SetFile("binlog.000001", 4);
  progress.Advance(120, 1'700'000'000);
  StreamPoint point = progress.Read();
  EXPECT_EQ(point.file, "binlog.000001");
  EXPECT_EQ(point.position, 120u);
  EXPECT_EQ(point.timestamp, 1'700'000'000u);
  EXPECT_FALSE(point.idle);

  // A new file starts where its events begin, and is not idle: the rotate
  // itself is progress.
  progress.Idle(200);
  EXPECT_TRUE(progress.Read().idle);
  progress.SetFile("binlog.000002", 4);
  point = progress.Read();
  EXPECT_EQ(point.file, "binlog.000002");
  EXPECT_EQ(point.position, 4u);
  EXPECT_FALSE(point.idle);
  // The timestamp is the last event's, whichever file it was in.
  EXPECT_EQ(point.timestamp, 1'700'000'000u);
}

TEST(StreamProgressTest, IdleNeverMovesThePositionBack) {
  StreamProgress progress;
  progress.SetFile("binlog.000001", 4);
  progress.Advance(500, 1);
  progress.Idle(300);
  EXPECT_EQ(progress.Read().position, 500u);
  EXPECT_TRUE(progress.Read().idle);
  progress.Idle(700);
  EXPECT_EQ(progress.Read().position, 700u);
  progress.Advance(900, 2);
  EXPECT_FALSE(progress.Read().idle);
}

}  // namespace
}  // namespace binlog_streamer
