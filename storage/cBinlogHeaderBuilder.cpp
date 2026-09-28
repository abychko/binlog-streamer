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

#include "storage/cBinlogHeaderBuilder.hpp"
#include "binlog/hEventFlags.hpp"
#include "storage/hStorageDefaults.hpp"
namespace binlog_streamer {
bool BinlogHeaderBuilder::Build(std::span<const std::uint8_t> fde,
                                std::span<const std::uint8_t> previousGtids,
                                std::vector<std::uint8_t> &out,
                                std::string &error) {
  if (fde.size() <= IN_USE_FLAG_OFFSET - BINLOG_MAGIC.size()) {
    error = "Format_description_event body shorter than its own flags field";
    return false;
  }
  out.clear();
  out.reserve(BINLOG_MAGIC.size() + fde.size() + previousGtids.size());
  out.insert(out.end(), BINLOG_MAGIC.begin(), BINLOG_MAGIC.end());
  out.insert(out.end(), fde.begin(), fde.end());
  out[IN_USE_FLAG_OFFSET] |=
      static_cast<std::uint8_t>(EVENT_FLAG_BINLOG_IN_USE);
  out.insert(out.end(), previousGtids.begin(), previousGtids.end());
  return true;
}
}  // namespace binlog_streamer
