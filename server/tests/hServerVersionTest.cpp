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

#include "server/hServerVersion.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(ServerVersionTest, TheSourcesVersionIsWhatTheRelayPresentsItselfBy) {
  EXPECT_EQ(ServerVersionString("8.4.11", "binlog-streamer", "0.27.0"),
            "8.4.11-binlog-streamer-0.27.0");
  EXPECT_EQ(ServerVersionString("8.0.42-33", "binlog-streamer", "1.0.0"),
            "8.0.42-33-binlog-streamer-1.0.0");
}

TEST(ServerVersionTest, WithNoSourceVersionThereIsNoVersionToPresent) {
  EXPECT_EQ(ServerVersionString("", "binlog-streamer", "0.27.0"), "");
}

// A relay whose source is a relay stores the original server's events,
// Format_description_event included, so the version it reads from its
// storage is the one the chain started at - the suffix is added once, by
// whichever relay answers.
TEST(ServerVersionTest, ARelayOfAnyVersionIsToldByTheVersionItPresents) {
  EXPECT_TRUE(IsRelayServerVersion(
      ServerVersionString("8.4.11", "binlog-streamer", "0.23.1"),
      "binlog-streamer"));
  EXPECT_TRUE(IsRelayServerVersion(
      ServerVersionString("5.7.44", "binlog-streamer", "1.0.0"),
      "binlog-streamer"));
}

TEST(ServerVersionTest, AServerOrARelayOfAnotherNameIsNotTakenForOne) {
  EXPECT_FALSE(IsRelayServerVersion("8.4.11", "binlog-streamer"));
  EXPECT_FALSE(IsRelayServerVersion("8.0.42-33", "binlog-streamer"));
  EXPECT_FALSE(IsRelayServerVersion("9.7.0-pbs", "binlog-streamer"));
  EXPECT_FALSE(IsRelayServerVersion(
      ServerVersionString("8.4.11", "other-relay", "0.23.1"),
      "binlog-streamer"));
  // The name is there, but nothing follows it: not a version this relay
  // would ever have presented.
  EXPECT_FALSE(
      IsRelayServerVersion("8.4.11-binlog-streamer-", "binlog-streamer"));
  EXPECT_FALSE(IsRelayServerVersion("", "binlog-streamer"));
}

}  // namespace
}  // namespace binlog_streamer
