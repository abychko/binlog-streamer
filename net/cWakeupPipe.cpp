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

#include "net/cWakeupPipe.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace binlog_streamer {
namespace {

bool SetNonBlockingAndCloseOnExec(int fd, std::string &error) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    error = std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

bool WakeupPipe::Open(std::string &error) {
  Close();
  int fds[2] = {-1, -1};
  if (pipe(fds) < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (!SetNonBlockingAndCloseOnExec(fds[0], error) ||
      !SetNonBlockingAndCloseOnExec(fds[1], error)) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  m_readFd = fds[0];
  m_writeFd = fds[1];
  return true;
}

void WakeupPipe::Wake() const {
  if (m_writeFd < 0) return;
  const std::uint8_t byte = 0;
  // Return value ignored: there is no signal-safe way to report failure. A
  // named variable, not a (void) cast, because _FORTIFY_SOURCE's
  // warn_unused_result rejects the cast.
  const ssize_t ignoredResult = write(m_writeFd, &byte, sizeof(byte));
  (void)ignoredResult;
}

void WakeupPipe::Drain() const {
  if (m_readFd < 0) return;
  std::uint8_t buffer[64];
  while (read(m_readFd, buffer, sizeof(buffer)) > 0) {
  }
}

void WakeupPipe::Close() {
  if (m_readFd >= 0) {
    close(m_readFd);
    m_readFd = -1;
  }
  if (m_writeFd >= 0) {
    close(m_writeFd);
    m_writeFd = -1;
  }
}

}  // namespace binlog_streamer
