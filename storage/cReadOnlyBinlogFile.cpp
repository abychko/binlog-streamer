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

#include "storage/cReadOnlyBinlogFile.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
namespace binlog_streamer {
ReadOnlyBinlogFile::~ReadOnlyBinlogFile() {
  if (m_fd >= 0) close(m_fd);
}
bool ReadOnlyBinlogFile::Open(const std::string &path, std::string &error) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (m_fd >= 0) close(m_fd);
  m_fd = fd;
  return true;
}
bool ReadOnlyBinlogFile::ReadAt(std::uint64_t offset,
                                std::span<std::uint8_t> out,
                                std::string &error) const {
  return ReadAt(m_fd, offset, out, error);
}
bool ReadOnlyBinlogFile::ReadAt(int fd, std::uint64_t offset,
                                std::span<std::uint8_t> out,
                                std::string &error) {
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t count = pread(fd, out.data() + done, out.size() - done,
                                static_cast<off_t>(offset + done));
    if (count < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      return false;
    }
    if (count == 0) {
      error =
          "file is shorter than its own published or catalog boundary claims";
      return false;
    }
    done += static_cast<std::size_t>(count);
  }
  return true;
}
}  // namespace binlog_streamer
