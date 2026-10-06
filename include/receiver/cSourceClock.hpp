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
#include <cstdint>
#include <optional>

namespace binlog_streamer {

struct SourceIdentity;

// steady_clock, so wall-clock jumps do not advance source time; the anchor is
// refreshed only on reconnection, as a MySQL replica does.
class SourceClock {
 public:
  SourceClock() = default;
  SourceClock(std::uint64_t sourceUnixTimestamp,
              std::chrono::steady_clock::time_point readAt);
  static SourceClock FromIdentity(const SourceIdentity &identity);
  bool Known() const;
  std::optional<std::uint64_t> Now(
      std::chrono::steady_clock::time_point now) const;
  std::optional<std::uint64_t> Now() const;

 private:
  std::uint64_t m_sourceUnixTimestamp = 0;
  std::chrono::steady_clock::time_point m_readAt{};
};

}  // namespace binlog_streamer
