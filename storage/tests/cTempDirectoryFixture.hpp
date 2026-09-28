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

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace binlog_streamer::test {

class TempDirectoryFixture {
 public:
  TempDirectoryFixture() {
    auto templatePath = (std::filesystem::temp_directory_path() /
                         "binlog-streamer-storage-testXXXXXX")
                            .string();
    std::vector<char> buffer(templatePath.begin(), templatePath.end());
    buffer.push_back('\0');
    // mkdtemp() returns nullptr on failure; constructing a path from that
    // would be undefined behavior.
    const char *created = mkdtemp(buffer.data());
    if (created == nullptr)
      throw std::runtime_error(std::string("mkdtemp failed: ") +
                               std::strerror(errno));
    directory_ = created;
  }
  ~TempDirectoryFixture() { std::filesystem::remove_all(directory_); }
  TempDirectoryFixture(const TempDirectoryFixture &) = delete;
  TempDirectoryFixture &operator=(const TempDirectoryFixture &) = delete;

  const std::filesystem::path &Directory() const { return directory_; }
  std::filesystem::path Path(const std::string &name) const {
    return directory_ / name;
  }

 private:
  std::filesystem::path directory_;
};

}  // namespace binlog_streamer::test
