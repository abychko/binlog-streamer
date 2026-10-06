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

#pragma once
#include <chrono>
#include <cstddef>
#include <functional>
namespace binlog_streamer {
// The callbacks run outside the index mutex and must not destroy the cache.
struct CacheHooks {
  // Must return an mmap-compatible region, or MAP_FAILED with errno set.
  std::function<void *(std::size_t)> reserve;
  // Runs after admission, before bytes are copied or published; must not
  // throw.
  std::function<void()> beforeAppendCopy;
  std::function<void()> beforeCopy;
  std::function<void()> afterMiss;
  std::function<void()> afterBeginFile;
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<int(void *, std::size_t)> returnPages;
};
}  // namespace binlog_streamer
