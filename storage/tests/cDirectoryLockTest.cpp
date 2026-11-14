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

#include "storage/cDirectoryLock.hpp"

#include "cTempDirectoryFixture.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace binlog_streamer {
namespace {

using test::TempDirectoryFixture;

TEST(DirectoryLockTest, ASecondHolderIsRefusedUntilTheFirstIsGone) {
  TempDirectoryFixture fixture;
  bool accessProblem = true;
  std::string error;
  std::optional<DirectoryLock> first(std::in_place);
  ASSERT_TRUE(first->Acquire(fixture.Directory(), accessProblem, error))
      << error;
  EXPECT_FALSE(accessProblem);

  DirectoryLock second;
  EXPECT_FALSE(second.Acquire(fixture.Directory(), accessProblem, error));
  EXPECT_FALSE(accessProblem);
  EXPECT_NE(error.find("is already open by another instance"),
            std::string::npos)
      << error;

  first.reset();
  EXPECT_TRUE(second.Acquire(fixture.Directory(), accessProblem, error))
      << error;
}

TEST(DirectoryLockTest, AMissingDirectoryIsAnAccessProblem) {
  TempDirectoryFixture fixture;
  bool accessProblem = false;
  std::string error;
  DirectoryLock lock;
  EXPECT_FALSE(lock.Acquire(fixture.Path("missing"), accessProblem, error));
  EXPECT_TRUE(accessProblem);
}

}  // namespace
}  // namespace binlog_streamer
