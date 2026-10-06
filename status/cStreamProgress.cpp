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

#include "status/cStreamProgress.hpp"

namespace binlog_streamer {

void StreamProgress::SetFile(const std::string &file, std::uint64_t position) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_file = file;
  m_position.store(position, std::memory_order_relaxed);
  m_idle.store(false, std::memory_order_relaxed);
}

void StreamProgress::Advance(std::uint64_t position, std::uint32_t timestamp) {
  m_position.store(position, std::memory_order_relaxed);
  m_timestamp.store(timestamp, std::memory_order_relaxed);
  m_idle.store(false, std::memory_order_relaxed);
}

void StreamProgress::Idle(std::uint64_t position) {
  std::uint64_t current = m_position.load(std::memory_order_relaxed);
  while (current < position &&
         !m_position.compare_exchange_weak(current, position,
                                           std::memory_order_relaxed)) {
  }
  m_idle.store(true, std::memory_order_relaxed);
}

StreamPoint StreamProgress::Read() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  StreamPoint point;
  point.file = m_file;
  point.position = m_position.load(std::memory_order_relaxed);
  point.timestamp = m_timestamp.load(std::memory_order_relaxed);
  point.idle = m_idle.load(std::memory_order_relaxed);
  return point;
}

void StreamProgress::ReadFrom(bool disk) {
  m_readFrom.store(disk ? 2 : 1, std::memory_order_relaxed);
}

std::optional<bool> StreamProgress::FromDisk() const {
  const std::uint8_t from = m_readFrom.load(std::memory_order_relaxed);
  if (from == 0) return std::nullopt;
  return from == 2;
}

}  // namespace binlog_streamer
