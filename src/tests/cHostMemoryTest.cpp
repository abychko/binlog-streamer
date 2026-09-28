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

#include "cHostMemory.hpp"

#include <gtest/gtest.h>
#include <unistd.h>
#include <limits>

namespace binlog_streamer {
namespace {

TEST(HostMemoryTest, PhysicalBytesMatchesHostPages) {
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(pages, 0);
  ASSERT_GT(pageSize, 0);
  const auto count = static_cast<std::uint64_t>(pages);
  const auto size = static_cast<std::uint64_t>(pageSize);
  ASSERT_LE(count, std::numeric_limits<std::uint64_t>::max() / size);
  const auto bytes = HostMemory::PhysicalBytes();
  EXPECT_GT(bytes, 0);
  EXPECT_EQ(bytes, count * size);
}

}  // namespace
}  // namespace binlog_streamer
