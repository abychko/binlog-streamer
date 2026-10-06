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
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include "status/sStreamPoint.hpp"

namespace binlog_streamer {

// Written by one stream thread per event, read by the status thread now
// and then. Only a file change takes the lock: position and timestamp are
// atomics, and a reader holding the lock sees a position that belongs to
// the file it read, since the writer never advances the old file after
// SetFile() returned.
class StreamProgress {
 public:
  void SetFile(const std::string &file, std::uint64_t position);
  void Advance(std::uint64_t position, std::uint32_t timestamp);
  void Idle(std::uint64_t position);
  StreamPoint Read() const;
  void ReadFrom(bool disk);
  std::optional<bool> FromDisk() const;

 private:
  mutable std::mutex m_mutex;
  std::string m_file;
  std::atomic<std::uint64_t> m_position{0};
  std::atomic<std::uint32_t> m_timestamp{0};
  std::atomic<bool> m_idle{false};
  std::atomic<std::uint8_t> m_readFrom{0};  // 0 unknown, 1 cache, 2 disk
};

}  // namespace binlog_streamer
