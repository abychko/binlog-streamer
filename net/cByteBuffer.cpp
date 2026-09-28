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

#include "net/cByteBuffer.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace binlog_streamer {

ByteBuffer::ByteBuffer(ByteBuffer &&other) noexcept { swap(other); }

ByteBuffer &ByteBuffer::operator=(ByteBuffer &&other) noexcept {
  ByteBuffer taken(std::move(other));
  swap(taken);
  return *this;
}

void ByteBuffer::resize(std::size_t newSize) {
  // Doubling keeps a buffer that grows in small steps from copying itself
  // on every step.
  if (newSize > m_capacity) Reallocate(std::max(newSize, 2 * m_capacity));
  m_size = newSize;
}

void ByteBuffer::shrink_to_fit() {
  if (m_capacity != m_size) Reallocate(m_size);
}

void ByteBuffer::swap(ByteBuffer &other) noexcept {
  std::swap(m_data, other.m_data);
  std::swap(m_size, other.m_size);
  std::swap(m_capacity, other.m_capacity);
}

void ByteBuffer::Reallocate(std::size_t newCapacity) {
  std::unique_ptr<std::uint8_t[]> moved;
  if (newCapacity > 0) {
    moved = std::make_unique_for_overwrite<std::uint8_t[]>(newCapacity);
    if (m_size > 0) std::memcpy(moved.get(), m_data.get(), m_size);
  }
  m_data = std::move(moved);
  m_capacity = newCapacity;
}

}  // namespace binlog_streamer
