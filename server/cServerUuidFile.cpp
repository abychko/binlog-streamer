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

#include "server/cServerUuidFile.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace binlog_streamer {

namespace {

constexpr std::size_t UUID_TEXT_LENGTH = 36;
constexpr char KEY[] = "server-uuid=";

bool IsUuidText(const std::string &text) {
  if (text.size() != UUID_TEXT_LENGTH) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool dashExpected = i == 8 || i == 13 || i == 18 || i == 23;
    if (dashExpected ? text[i] != '-'
                     : std::isxdigit(static_cast<unsigned char>(text[i])) == 0)
      return false;
  }
  return true;
}

}  // namespace

bool ServerUuidFile::LoadOrCreate(const std::filesystem::path &dataDir,
                                  const std::string &freshUuid,
                                  std::string &uuid, std::string &error) {
  const std::filesystem::path path = dataDir / FILE_NAME;
  std::error_code existsError;
  if (std::filesystem::exists(path, existsError)) {
    std::ifstream in(path);
    if (!in) {
      error = "cannot read " + path.string();
      return false;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind(KEY, 0) != 0) continue;
      const std::string value = line.substr(sizeof(KEY) - 1);
      if (!IsUuidText(value)) break;
      uuid = value;
      error.clear();
      return true;
    }
    error = path.string() + " holds no valid server-uuid";
    return false;
  }
  if (!IsUuidText(freshUuid)) {
    error = "generated server UUID is malformed";
    return false;
  }
  std::ofstream out(path, std::ios::trunc);
  out << "[auto]\n" << KEY << freshUuid << '\n';
  out.flush();
  if (!out) {
    error = "cannot write " + path.string();
    return false;
  }
  uuid = freshUuid;
  error.clear();
  return true;
}

}  // namespace binlog_streamer
