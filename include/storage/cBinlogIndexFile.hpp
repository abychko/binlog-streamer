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

#include <string>
#include <vector>

namespace binlog_streamer {

// Never a path, so entries stay valid if data_dir moves. Per-file
// metadata lives in RAM, not here.
class BinlogIndexFile {
 public:
  // Missing file reads as an empty index, not an error (matches MySQL).
  // A leftover path+".tmp" is left for the startup scan to remove, not
  // Load(). names is empty on any error.
  static bool Load(const std::string &path, std::vector<std::string> &names,
                   std::string &error);

  // A false return does not mean nothing changed: once rename(2)
  // commits, path already holds the new names even if the trailing
  // fsync fails.
  static bool Replace(const std::string &path,
                      const std::vector<std::string> &names,
                      std::string &error);

  // Full copy-then-replace, like MySQL's own add_log_to_index() - not
  // an in-place append despite the name.
  static bool Append(const std::string &path, const std::string &name,
                     std::string &error);
};

}  // namespace binlog_streamer
