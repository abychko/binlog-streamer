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

#include "server/cReplicaClientList.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

std::vector<ReplicaClient> Accounts(const std::string &user) {
  ReplicaClient client;
  client.user = user;
  return {client};
}

TEST(ReplicaClientListTest, CurrentIsTheListItWasBuiltWith) {
  const ReplicaClientList list(Accounts("repl"));
  const auto current = list.Current();
  ASSERT_EQ(current->size(), 1u);
  EXPECT_EQ(current->front().user, "repl");
}

TEST(ReplicaClientListTest, ReplaceLeavesATakenSnapshotAsItWas) {
  ReplicaClientList list(Accounts("before"));
  const auto taken = list.Current();
  list.Replace(Accounts("after"));
  EXPECT_EQ(taken->front().user, "before");
  EXPECT_EQ(list.Current()->front().user, "after");
}

TEST(ReplicaClientListTest, ASnapshotOutlivesTheList) {
  ReplicaClientList::Snapshot taken;
  {
    const ReplicaClientList list(Accounts("repl"));
    taken = list.Current();
  }
  EXPECT_EQ(taken->front().user, "repl");
}

}  // namespace
}  // namespace binlog_streamer
