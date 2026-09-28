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

#include "cPublicKeyFileReader.hpp"
#include "cFileDescriptor.hpp"
#include "cPublicKeyFileCheck.hpp"
#include "cSecretFileCheck.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace binlog_streamer {
namespace {
// Duplicated from cDiskProtectedFileReader.cpp: the two checks differ (this
// one skips group ownership), so a shared header would only serve these two
// call sites.
std::string ShellQuote(const std::string &text) {
  std::string quoted = "'";
  for (char character : text)
    quoted += character == '\'' ? "'\\''" : std::string(1, character);
  return quoted + "'";
}
std::string Join(const std::vector<std::string> &messages) {
  std::string joined;
  for (const auto &message : messages) {
    if (!joined.empty()) joined += ", ";
    joined += message;
  }
  return joined;
}
}  // namespace
PublicKeyFileReader::PublicKeyFileReader(std::string expectedOwner)
    : expectedOwner_(std::move(expectedOwner)) {}
ProtectedFileStatus PublicKeyFileReader::Read(const std::string &path,
                                              std::string &content,
                                              std::string &errorMessage) {
  content.clear();
  errorMessage.clear();
  const auto fail = [&](const std::string &message) {
    errorMessage = message;
    return ProtectedFileStatus::Failed;
  };
  if (path.find('\0') != std::string::npos)
    return fail("invalid filesystem path");
  const std::filesystem::path filePath(path);
  const std::string directory =
      filePath.parent_path().empty() ? "." : filePath.parent_path().string();
  const FileDescriptor dir(
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.Get() < 0) {
    if (errno == ENOENT) return ProtectedFileStatus::Absent;
    return fail(std::strerror(errno));
  }
  // expectedOwner_ was already validated while reading source.yml, so a
  // missing account here means it was deleted mid-run; reported plainly
  // rather than folded into the combined mode-violation message below.
  const auto *owner = getpwnam(expectedOwner_.c_str());
  if (owner == nullptr)
    return fail("owner " + expectedOwner_ + " does not exist");
  const uid_t uid = owner->pw_uid;
  struct stat directoryStatus{};
  if (fstat(dir.Get(), &directoryStatus) != 0)
    return fail(std::strerror(errno));
  auto directoryErrors = SecretFileCheck::CheckDirectory(directoryStatus, uid);
  if (!directoryErrors.empty())
    return fail(Join(directoryErrors) + "; fix: chown " +
                ShellQuote(expectedOwner_) + " " + ShellQuote(directory) +
                " && chmod go-w " + ShellQuote(directory));
  // O_NOFOLLOW: a symlink here could point anywhere the process can read,
  // defeating the checks below. O_NONBLOCK: keeps a FIFO at this path from
  // hanging the read below.
  const FileDescriptor file(
      openat(dir.Get(), filePath.filename().c_str(),
             O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (file.Get() < 0) {
    if (errno == ENOENT) return ProtectedFileStatus::Absent;
    if (errno == ELOOP)
      return fail("is a symbolic link; replace it with a regular file");
    return fail(std::strerror(errno));
  }
  // fstat on the open descriptor, not stat() on the path: the permission
  // check must agree with what is actually read, not with the path (TOCTOU).
  struct stat status{};
  if (fstat(file.Get(), &status) != 0) return fail(std::strerror(errno));
  auto violations = PublicKeyFileCheck::CheckFile(status, uid);
  if (!violations.empty())
    return fail(Join(violations) + "; fix: chown " +
                ShellQuote(expectedOwner_) + " " + ShellQuote(path) +
                " && chmod go-w " + ShellQuote(path));
  std::array<char, 8192> buffer{};
  std::string contents;
  for (;;) {
    const ssize_t count = read(file.Get(), buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return fail(std::strerror(errno));
    }
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
  content = std::move(contents);
  return ProtectedFileStatus::Ok;
}
}  // namespace binlog_streamer
