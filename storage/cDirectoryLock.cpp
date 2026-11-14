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

#include "storage/cDirectoryLock.hpp"

#include "storage/hAccessErrno.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace binlog_streamer {

DirectoryLock::~DirectoryLock() {
  if (m_fd >= 0) close(m_fd);
}

bool DirectoryLock::Acquire(const std::filesystem::path &directory,
                            bool &accessProblem, std::string &error) {
  accessProblem = false;
  const std::string path = directory.string();
  // The directory, not a file in it: index files are replaced by
  // rename(2), and a lock on the old inode would guard nothing.
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    const int savedErrno = errno;
    accessProblem = IsAccessErrno(savedErrno);
    error = "opening " + path + ": " + std::strerror(savedErrno);
    return false;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int savedErrno = errno;
    if (savedErrno == EWOULDBLOCK) {
      error = "data directory " + path + " is already open by another instance";
    } else {
      accessProblem = IsAccessErrno(savedErrno);
      error = "locking " + path + ": " + std::strerror(savedErrno);
    }
    close(fd);
    return false;
  }
  if (m_fd >= 0) close(m_fd);
  m_fd = fd;
  return true;
}

}  // namespace binlog_streamer
