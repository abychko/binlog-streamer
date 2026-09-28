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

#include <limits>
#include "receiver/sSourceIdentity.hpp"

namespace binlog_streamer {

SourceClock::SourceClock(std::uint64_t sourceUnixTimestamp,
                         std::chrono::steady_clock::time_point readAt)
    : m_sourceUnixTimestamp(sourceUnixTimestamp), m_readAt(readAt) {}

SourceClock SourceClock::FromIdentity(const SourceIdentity &identity) {
  return SourceClock(identity.unixTimestamp, identity.unixTimestampReadAt);
}

bool SourceClock::Known() const { return m_sourceUnixTimestamp != 0; }

std::optional<std::uint64_t> SourceClock::Now(
    std::chrono::steady_clock::time_point now) const {
  if (!Known()) return std::nullopt;
  if (now <= m_readAt) return m_sourceUnixTimestamp;
  const auto elapsed = static_cast<std::uint64_t>(
      std::chrono::floor<std::chrono::seconds>(now - m_readAt).count());
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (elapsed > maximum - m_sourceUnixTimestamp) return maximum;
  return m_sourceUnixTimestamp + elapsed;
}

std::optional<std::uint64_t> SourceClock::Now() const {
  return Now(std::chrono::steady_clock::now());
}

}  // namespace binlog_streamer
