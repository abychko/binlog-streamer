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

#include "storage/cBinlogIndexFile.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <unordered_set>

namespace binlog_streamer {
namespace {

std::string TmpPath(const std::string &path) { return path + ".tmp"; }

// Rejects a trailing '\r': Load() does not trim it, unlike MySQL's own
// reader.
bool ValidateIndexName(const std::string &name, std::string &error) {
  if (name.empty()) {
    error = "empty line in index";
    return false;
  }
  for (unsigned char c : name) {
    if (c == '/') {
      error = "index holds a bare file name, not a path: " + name;
      return false;
    }
    if (c < 0x20) {
      error = "index name contains a control character: " + name;
      return false;
    }
  }
  return true;
}

bool SyncFd(int fd, std::string &error) {
  if (fsync(fd) != 0) {
    error = std::strerror(errno);
    return false;
  }
  return true;
}

bool WriteAll(int fd, const std::string &content, std::string &error) {
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t count =
        write(fd, content.data() + offset, content.size() - offset);
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

bool BinlogIndexFile::Load(const std::string &path,
                           std::vector<std::string> &names,
                           std::string &error) {
  names.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return true;
    error = std::strerror(errno);
    return false;
  }
  struct stat status{};
  if (fstat(fd, &status) != 0) {
    error = std::strerror(errno);
    close(fd);
    return false;
  }
  std::string content(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t count =
        read(fd, content.data() + offset, content.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      close(fd);
      return false;
    }
    if (count == 0) break;  // file shrank under us: keep what was read so far
    offset += static_cast<std::size_t>(count);
  }
  content.resize(offset);
  close(fd);

  // A file not ending in '\n' is corruption, not a partial name to accept.
  if (!content.empty() && content.back() != '\n') {
    error = "index does not end with a newline";
    return false;
  }

  std::unordered_set<std::string> seen;
  std::size_t start = 0;
  while (start < content.size()) {
    const std::size_t newline = content.find('\n', start);
    const std::string line = content.substr(start, newline - start);
    start = newline + 1;
    if (!ValidateIndexName(line, error)) {
      names.clear();
      return false;
    }
    if (!seen.insert(line).second) {
      error = "duplicate name in index: " + line;
      names.clear();
      return false;
    }
    names.push_back(line);
  }
  return true;
}

bool BinlogIndexFile::Replace(const std::string &path,
                              const std::vector<std::string> &names,
                              std::string &error) {
  std::string content;
  std::unordered_set<std::string> seen;
  seen.reserve(names.size());
  for (const auto &name : names) {
    if (!ValidateIndexName(name, error)) return false;
    if (!seen.insert(name).second) {
      error = "duplicate name in index: " + name;
      return false;
    }
    content += name;
    content += '\n';
  }

  const std::string tmpPath = TmpPath(path);
  // O_TRUNC, not O_EXCL: a leftover ".tmp" from an interrupted Replace()
  // is expected.
  const int fd =
      open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (!WriteAll(fd, content, error)) {
    close(fd);
    return false;
  }
  if (!SyncFd(fd, error)) {
    close(fd);
    return false;
  }
  close(fd);

  if (rename(tmpPath.c_str(), path.c_str()) != 0) {
    error = std::strerror(errno);
    return false;
  }

  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  const int dirFd =
      open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_CLOEXEC);
  if (dirFd < 0) {
    error = std::strerror(errno);
    return false;
  }
  const bool dirSynced = SyncFd(dirFd, error);
  close(dirFd);
  return dirSynced;
}

bool BinlogIndexFile::Append(const std::string &path, const std::string &name,
                             std::string &error) {
  std::vector<std::string> names;
  if (!Load(path, names, error)) return false;
  names.push_back(name);
  return Replace(path, names, error);
}

}  // namespace binlog_streamer
