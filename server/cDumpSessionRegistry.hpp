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

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace binlog_streamer {

// One dump per replica, so a replica reconnecting while the relay still
// holds a half-open previous connection is not served twice. Keyed by the
// replica's UUID, or its server_id when it sent none; shared by all
// connections.
class DumpSessionRegistry {
 public:
  using SupersededFlag = std::shared_ptr<std::atomic<bool>>;

  // Registers a new dump under key and tells the previous holder of the
  // same key, if any, to stop. The returned flag turns true when a later
  // dump takes this one's place.
  SupersededFlag Claim(const std::string &key);

  // Forgets the dump, unless a later one already took its place.
  void Release(const std::string &key, const SupersededFlag &flag);

 private:
  std::mutex m_mutex;
  std::map<std::string, SupersededFlag> m_sessions;
};

}  // namespace binlog_streamer
