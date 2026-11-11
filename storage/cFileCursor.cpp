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

#include "storage/cFileCursor.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>

namespace binlog_streamer {

FileCursor::FileCursor(FilePin pin, int fd, std::string path,
                       std::uint64_t headerLength)
    : m_pin(std::move(pin)),
      m_fd(fd),
      m_path(std::move(path)),
      m_headerLength(headerLength) {}

FileCursor::~FileCursor() {
  if (m_fd >= 0) close(m_fd);
}

FileCursor::FileCursor(FileCursor &&other) noexcept
    : m_pin(std::move(other.m_pin)),
      m_fd(std::exchange(other.m_fd, -1)),
      m_path(std::move(other.m_path)),
      m_headerLength(other.m_headerLength),
      m_previousReadFromDisk(other.m_previousReadFromDisk),
      m_dropStarted(other.m_dropStarted),
      m_droppedTo(other.m_droppedTo),
      m_checkedMark(other.m_checkedMark),
      m_published(other.m_published) {}

FileCursor &FileCursor::operator=(FileCursor &&other) noexcept {
  if (this == &other) return *this;
  if (m_fd >= 0) close(m_fd);
  m_pin = std::move(other.m_pin);
  m_fd = std::exchange(other.m_fd, -1);
  m_path = std::move(other.m_path);
  m_headerLength = other.m_headerLength;
  m_previousReadFromDisk = other.m_previousReadFromDisk;
  m_dropStarted = other.m_dropStarted;
  m_droppedTo = other.m_droppedTo;
  m_checkedMark = other.m_checkedMark;
  m_published = other.m_published;
  return *this;
}

int FileCursor::Fd(std::string &error) const {
  if (m_fd >= 0) return m_fd;
  m_fd = open(m_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (m_fd < 0) {
    error = "opening " + m_path + ": " + std::strerror(errno);
    return m_fd;
  }
#if defined(__linux__)
  posix_fadvise(m_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  return m_fd;
}

void FileCursor::DropConsumedPages([[maybe_unused]] std::uint64_t begin,
                                   [[maybe_unused]] std::uint64_t end) const {
#if defined(__linux__)
  static constexpr std::uint64_t DROP_BATCH_BYTES = 8ull << 20;
  if (m_fd < 0) return;
  if (!m_dropStarted) {
    // Pages before the first read were not brought in by this cursor.
    m_dropStarted = true;
    m_droppedTo = begin;
  }
  if (end <= m_droppedTo || end - m_droppedTo < DROP_BATCH_BYTES) return;
  posix_fadvise(m_fd, static_cast<off_t>(m_droppedTo),
                static_cast<off_t>(end - m_droppedTo), POSIX_FADV_DONTNEED);
  m_droppedTo = end;
#endif
}

}  // namespace binlog_streamer
