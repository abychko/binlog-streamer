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

#include "storage/cBinlogFileWriter.hpp"
#include "storage/cBinlogHeaderBuilder.hpp"

#include "binlog/hEventFlags.hpp"
#include "storage/hStorageDefaults.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

namespace binlog_streamer {
namespace {

bool WriteAll(int fd, std::span<const std::uint8_t> bytes, std::string &error,
              std::uint64_t *calls = nullptr) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (calls) ++*calls;
    const ssize_t count =
        write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      return false;
    }
    if (count == 0) {
      error = "write() made no progress";
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

BinlogFileWriter::~BinlogFileWriter() {
  if (fd_ >= 0) close(fd_);
}

bool BinlogFileWriter::Create(const std::string &path,
                              std::span<const std::uint8_t> fdeBytes,
                              std::span<const std::uint8_t> previousGtidsBytes,
                              std::string &error) {
  std::vector<std::uint8_t> header;
  if (!BinlogHeaderBuilder::Build(fdeBytes, previousGtidsBytes, header, error))
    return false;
  const int fd =
      open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  fd_ = fd;
  if (!WriteAll(fd_, header, error)) return false;
  if (fsync(fd_) != 0) {
    error = std::strerror(errno);
    return false;
  }
  length_ = header.size();
  return true;
}

bool BinlogFileWriter::OpenExisting(const std::string &path,
                                    std::string &error) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  struct stat status{};
  if (fstat(fd, &status) != 0) {
    error = std::strerror(errno);
    close(fd);
    return false;
  }
  // Must seek to EOF: open() leaves the cursor at 0, and Append() relies
  // on it already being there.
  if (lseek(fd, 0, SEEK_END) < 0) {
    error = std::strerror(errno);
    close(fd);
    return false;
  }
  fd_ = fd;
  length_ = static_cast<std::uint64_t>(status.st_size);
  return true;
}

bool BinlogFileWriter::Append(std::span<const std::uint8_t> bytes,
                              std::string &error) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (buffer_.empty() && remaining >= WRITE_BUFFER_SIZE) {
      if (!WriteDirect(bytes.subspan(offset), error)) return false;
      return true;
    }
    const std::size_t space = WRITE_BUFFER_SIZE - buffer_.size();
    const std::size_t take = std::min(space, remaining);
    buffer_.insert(buffer_.end(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                   bytes.begin() + static_cast<std::ptrdiff_t>(offset + take));
    offset += take;
    if (buffer_.size() == WRITE_BUFFER_SIZE && !Flush(error)) return false;
  }
  return true;
}

bool BinlogFileWriter::WriteDirect(std::span<const std::uint8_t> bytes,
                                   std::string &error, std::uint64_t *calls) {
  if (!WriteAll(fd_, bytes, error, calls)) return false;
  length_ += bytes.size();
  return true;
}

bool BinlogFileWriter::Flush(std::string &error) {
  if (buffer_.empty()) return true;
  if (!WriteDirect(buffer_, error)) return false;
  buffer_.clear();
  return true;
}

bool BinlogFileWriter::Sync(std::string &error) {
  if (fsync(fd_) != 0) {
    error = std::strerror(errno);
    return false;
  }
  return true;
}

bool BinlogFileWriter::MarkClosed(std::string &error) {
  std::uint8_t flags = 0;
  const ssize_t readCount =
      pread(fd_, &flags, 1, static_cast<off_t>(IN_USE_FLAG_OFFSET));
  if (readCount != 1) {
    error = readCount < 0 ? std::strerror(errno)
                          : "file shorter than IN_USE_FLAG_OFFSET";
    return false;
  }
  flags &= static_cast<std::uint8_t>(~EVENT_FLAG_BINLOG_IN_USE);
  if (pwrite(fd_, &flags, 1, static_cast<off_t>(IN_USE_FLAG_OFFSET)) != 1) {
    error = std::strerror(errno);
    return false;
  }
  return Sync(error);
}

bool BinlogFileWriter::Truncate(std::uint64_t length, std::string &error) {
  // Growing here would let ftruncate(2) zero-pad the gap, producing
  // exactly the sparse file this class forbids.
  if (length > length_ + buffer_.size()) {
    error = "Truncate() length is past the end of what has been written";
    return false;
  }
  buffer_.clear();
  if (ftruncate(fd_, static_cast<off_t>(length)) != 0) {
    error = std::strerror(errno);
    return false;
  }
  if (lseek(fd_, static_cast<off_t>(length), SEEK_SET) < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (fsync(fd_) != 0) {
    error = std::strerror(errno);
    return false;
  }
  length_ = length;
  return true;
}

}  // namespace binlog_streamer
