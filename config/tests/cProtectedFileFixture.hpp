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

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace binlog_streamer::test {

class ProtectedFileFixture {
 public:
  ProtectedFileFixture() {
    auto templatePath =
        (std::filesystem::temp_directory_path() / "binlog-streamer-testXXXXXX")
            .string();
    std::vector<char> buffer(templatePath.begin(), templatePath.end());
    buffer.push_back('\0');
    // mkdtemp() returns nullptr on failure; building a path from it would be
    // undefined behavior.
    const char *created = mkdtemp(buffer.data());
    if (created == nullptr)
      throw std::runtime_error(std::string("mkdtemp failed: ") +
                               std::strerror(errno));
    directory_ = created;
  }
  ~ProtectedFileFixture() { std::filesystem::remove_all(directory_); }
  ProtectedFileFixture(const ProtectedFileFixture &) = delete;
  ProtectedFileFixture &operator=(const ProtectedFileFixture &) = delete;

  const std::filesystem::path &Directory() const { return directory_; }

  std::filesystem::path WriteFile(const std::string &name,
                                  const std::string &content,
                                  int mode = 0640) const {
    const auto path = directory_ / name;
    std::ofstream output(path, std::ios::binary);
    output << content;
    output.close();
    if (output.fail())
      throw std::runtime_error("failed to write " + path.string());
    if (chmod(path.c_str(), static_cast<mode_t>(mode)) != 0)
      throw std::runtime_error(std::string("chmod failed on ") + path.string() +
                               ": " + std::strerror(errno));
    return path;
  }

  static std::string CurrentUserName() {
    const auto *entry = getpwuid(getuid());
    return entry != nullptr ? entry->pw_name : std::string();
  }
  static std::string CurrentGroupName() {
    const auto *entry = getgrgid(getgid());
    return entry != nullptr ? entry->gr_name : std::string();
  }

 private:
  std::filesystem::path directory_;
};

}  // namespace binlog_streamer::test
