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

#include "cSecretFileCheck.hpp"

namespace binlog_streamer {
std::vector<std::string> SecretFileCheck::CheckFile(const struct stat &status,
                                                    uid_t expectedUid,
                                                    gid_t expectedGid) {
  std::vector<std::string> errors;
  if (!S_ISREG(status.st_mode)) errors.emplace_back("not a regular file");
  if (status.st_uid != expectedUid) errors.emplace_back("wrong owner");
  if (status.st_gid != expectedGid) errors.emplace_back("wrong group");
  // Three separate checks, not a single "mode not wider than 0640"
  // comparison: owner and group-execute bits are intentionally left unchecked.
  if ((status.st_mode & 0007) != 0) errors.emplace_back("others have access");
  if ((status.st_mode & 0020) != 0)
    errors.emplace_back("group has write access");
  if ((status.st_mode & 07000) != 0)
    errors.emplace_back("setuid, setgid or sticky bit is set");
  return errors;
}
std::vector<std::string> SecretFileCheck::CheckDirectory(
    const struct stat &status, uid_t expectedUid) {
  std::vector<std::string> errors;
  if (!S_ISDIR(status.st_mode)) errors.emplace_back("not a directory");
  if (status.st_uid != expectedUid)
    errors.emplace_back("wrong directory owner");
  if ((status.st_mode & 0022) != 0)
    errors.emplace_back("directory mode allows group or other writes");
  return errors;
}
}  // namespace binlog_streamer
