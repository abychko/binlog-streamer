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

#include "config/cDiskProtectedFileReader.hpp"
#include "cFileDescriptor.hpp"
#include "cSecretFileCheck.hpp"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <utility>

namespace binlog_streamer {
namespace {
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
// With a sentinel uid/gid, CheckDirectory/CheckFile also reports a mismatch
// alongside the more specific "... does not exist" - removed here since it's
// a direct consequence of the missing name, not a separate violation.
void RemoveMessage(std::vector<std::string> &messages,
                   const std::string &message) {
  messages.erase(std::remove(messages.begin(), messages.end(), message),
                 messages.end());
}
}  // namespace
DiskProtectedFileReader::DiskProtectedFileReader(std::string expectedOwner,
                                                 std::string expectedGroup)
    : expectedOwner_(std::move(expectedOwner)),
      expectedGroup_(std::move(expectedGroup)) {}
ProtectedFileStatus DiskProtectedFileReader::Read(
    const std::string &path, std::string &content,
    std::vector<ConfigError> &errors) {
  content.clear();
  const auto fail = [&](const std::string &file, const std::string &message) {
    errors.push_back({file, 0, 0, {}, message});
    return ProtectedFileStatus::Failed;
  };
  if (path.find('\0') != std::string::npos)
    return fail(path, "invalid filesystem path");
  const std::filesystem::path filePath(path);
  const std::string directory =
      filePath.parent_path().empty() ? "." : filePath.parent_path().string();
  const FileDescriptor dir(
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.Get() < 0) {
    if (errno == ENOENT) return ProtectedFileStatus::Absent;
    return fail(directory, std::strerror(errno));
  }
  // A missing expected owner/group is folded into the same combined
  // violations list as mode/ownership mismatches, not an early failure,
  // so it never hides an unrelated violation found further down.
  const auto *owner = getpwnam(expectedOwner_.c_str());
  const uid_t uid = owner != nullptr ? owner->pw_uid : static_cast<uid_t>(-1);
  struct stat directoryStatus{};
  if (fstat(dir.Get(), &directoryStatus) != 0)
    return fail(directory, std::strerror(errno));
  auto directoryErrors = SecretFileCheck::CheckDirectory(directoryStatus, uid);
  if (owner == nullptr) {
    RemoveMessage(directoryErrors, "wrong directory owner");
    directoryErrors.insert(directoryErrors.begin(),
                           "owner " + expectedOwner_ + " does not exist");
  }
  if (!directoryErrors.empty())
    return fail(directory, Join(directoryErrors) + "; fix: chown " +
                               ShellQuote(expectedOwner_) + " " +
                               ShellQuote(directory) + " && chmod go-w " +
                               ShellQuote(directory));
  const FileDescriptor file(
      openat(dir.Get(), filePath.filename().c_str(),
             O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (file.Get() < 0) {
    if (errno == ENOENT) return ProtectedFileStatus::Absent;
    if (errno == ELOOP)
      return fail(path, "is a symbolic link; replace it with a regular file");
    return fail(path, std::strerror(errno));
  }
  struct stat status{};
  if (fstat(file.Get(), &status) != 0) return fail(path, std::strerror(errno));
  const auto *group = getgrnam(expectedGroup_.c_str());
  const gid_t gid = group != nullptr ? group->gr_gid : static_cast<gid_t>(-1);
  auto violations = SecretFileCheck::CheckFile(status, uid, gid);
  if (owner == nullptr) RemoveMessage(violations, "wrong owner");
  if (group == nullptr) {
    RemoveMessage(violations, "wrong group");
    violations.insert(violations.begin(),
                      "group " + expectedGroup_ + " does not exist");
  }
  if (!violations.empty())
    return fail(path, Join(violations) + "; fix: chown " +
                          ShellQuote(expectedOwner_ + ":" + expectedGroup_) +
                          " " + ShellQuote(path) + " && chmod 0640 " +
                          ShellQuote(path));
  std::array<char, 8192> buffer{};
  std::string contents;
  for (;;) {
    const ssize_t count = read(file.Get(), buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return fail(path, std::strerror(errno));
    }
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
  content = std::move(contents);
  return ProtectedFileStatus::Ok;
}
}  // namespace binlog_streamer
