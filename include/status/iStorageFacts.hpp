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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include "status/sMemoryStatus.hpp"

namespace binlog_streamer {

// What the status needs from storage, asked on the status thread: every
// method has to be safe against the writer and the purger.
class StorageFacts {
 public:
  virtual ~StorageFacts() = default;
  virtual std::size_t Files() const = 0;
  virtual std::uint64_t Bytes() const = 0;
  virtual std::uint64_t MaxBytes() const = 0;
  virtual MemoryStatus Memory() const = 0;
  virtual void Published(std::string &file, std::uint64_t &position) const = 0;
  // Bytes stored before this point, across files; nullopt for a file
  // storage does not hold (purged, or not created yet).
  virtual std::optional<std::uint64_t> Offset(const std::string &file,
                                              std::uint64_t position) const = 0;
  // Bytes from one point to a later one, in one look at the catalog;
  // nullopt when either file is gone or `to` comes before `from`.
  virtual std::optional<std::uint64_t> Distance(
      const std::string &fromFile, std::uint64_t fromPosition,
      const std::string &toFile, std::uint64_t toPosition) const = 0;
};

}  // namespace binlog_streamer
