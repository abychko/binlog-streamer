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

#include <cstddef>
#include <cstdint>
#include <memory>

namespace binlog_streamer {

// A growable byte buffer whose resize() leaves added bytes unwritten, for
// buffers that are read or decoded into right after they grow. Names
// follow std::vector so the buffer passes as a std::span.
class ByteBuffer {
 public:
  ByteBuffer() = default;
  ByteBuffer(ByteBuffer &&other) noexcept;
  ByteBuffer &operator=(ByteBuffer &&other) noexcept;
  ByteBuffer(const ByteBuffer &) = delete;
  ByteBuffer &operator=(const ByteBuffer &) = delete;

  std::uint8_t *data() noexcept { return m_data.get(); }
  const std::uint8_t *data() const noexcept { return m_data.get(); }
  std::uint8_t *begin() noexcept { return m_data.get(); }
  const std::uint8_t *begin() const noexcept { return m_data.get(); }
  std::uint8_t *end() noexcept { return m_data.get() + m_size; }
  const std::uint8_t *end() const noexcept { return m_data.get() + m_size; }
  std::uint8_t &operator[](std::size_t index) noexcept { return m_data[index]; }
  const std::uint8_t &operator[](std::size_t index) const noexcept {
    return m_data[index];
  }
  std::size_t size() const noexcept { return m_size; }
  std::size_t capacity() const noexcept { return m_capacity; }
  bool empty() const noexcept { return m_size == 0; }

  // Keeps the first min(size, newSize) bytes; the rest is unwritten.
  void resize(std::size_t newSize);
  void clear() noexcept { m_size = 0; }
  void shrink_to_fit();
  void swap(ByteBuffer &other) noexcept;

 private:
  void Reallocate(std::size_t newCapacity);

  std::unique_ptr<std::uint8_t[]> m_data;
  std::size_t m_size = 0;
  std::size_t m_capacity = 0;
};

}  // namespace binlog_streamer
