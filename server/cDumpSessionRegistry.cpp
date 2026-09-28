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

#include "cDumpSessionRegistry.hpp"

namespace binlog_streamer {

DumpSessionRegistry::SupersededFlag DumpSessionRegistry::Claim(
    const std::string &key) {
  auto flag = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard<std::mutex> lock(m_mutex);
  auto &slot = m_sessions[key];
  if (slot) slot->store(true);
  slot = flag;
  return flag;
}

void DumpSessionRegistry::Release(const std::string &key,
                                  const SupersededFlag &flag) {
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_sessions.find(key);
  if (found != m_sessions.end() && found->second == flag)
    m_sessions.erase(found);
}

}  // namespace binlog_streamer
