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

#include "storage/cFilePin.hpp"

#include "storage/cStorageCatalog.hpp"
#include "storage/sStoredFileRecord.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <utility>

namespace binlog_streamer {
namespace {

// Two reference parameters rather than `*self = std::move(*self)`: -Wself-move
// flags that expression under -Werror even though this exercises
// operator=()'s own self-assignment guard.
void MoveAssign(FilePin &to, FilePin &from) { to = std::move(from); }

TEST(FilePinTest,
     MoveAssignmentReleasesTheOverwrittenPinAndSurvivesSelfAssignment) {
  StorageCatalog catalog;
  StoredFileRecord first;
  first.name = "binlog.000001";
  catalog.Add(first);
  StoredFileRecord second;
  second.name = "binlog.000002";
  catalog.Add(second);

  std::string error;
  auto firstPin = catalog.Pin("binlog.000001", error);
  ASSERT_TRUE(firstPin.has_value()) << error;
  auto secondPin = catalog.Pin("binlog.000002", error);
  ASSERT_TRUE(secondPin.has_value()) << error;

  *firstPin = std::move(*secondPin);
  EXPECT_EQ(firstPin->FileName(), "binlog.000002");

  // binlog.000001 is unpinned now; Remove() is oldest-first.
  EXPECT_TRUE(catalog.Remove(error)) << error;
  EXPECT_FALSE(catalog.Remove(error));
  EXPECT_EQ(error, "binlog.000002 is pinned by 1 reader(s)");

  // secondPin is moved-from now; resetting it must not touch firstPin's pin.
  secondPin.reset();
  EXPECT_FALSE(catalog.Remove(error));
  EXPECT_EQ(error, "binlog.000002 is pinned by 1 reader(s)");

  // Self-assignment must not release the pin it overwrites with itself.
  MoveAssign(*firstPin, *firstPin);
  EXPECT_FALSE(catalog.Remove(error));
  EXPECT_EQ(error, "binlog.000002 is pinned by 1 reader(s)");

  firstPin.reset();
  EXPECT_TRUE(catalog.Remove(error)) << error;
  EXPECT_EQ(catalog.Size(), 0u);
}

}  // namespace
}  // namespace binlog_streamer
