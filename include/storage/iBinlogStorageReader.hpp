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

#include "gtid/cGtidSet.hpp"
#include "storage/cFileCursor.hpp"
#include "storage/eNextFileOutcome.hpp"
#include "storage/eWaitOutcome.hpp"
#include "storage/eWaitStyle.hpp"
#include "storage/sPublishedPosition.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace binlog_streamer {

// Never reads past what PublishedPositionTracker has published.
class BinlogStorageReader {
 public:
  virtual ~BinlogStorageReader() = default;

  // Mirrors MYSQL_BIN_LOG::find_first_log_not_in_gtid_set.
  virtual std::optional<std::string> FindStartFile(
      const GtidSet &replicaSet) const = 0;

  virtual std::unique_ptr<FileCursor> Open(const std::string &fileName,
                                           std::string &error) = 0;

  // 0 exactly at the boundary is not an error, just nothing new yet;
  // offset past the boundary is refused (0, error set) as a caller error.
  virtual std::size_t Read(const FileCursor &cursor, std::uint64_t offset,
                           std::span<std::uint8_t> out, std::string &error) = 0;

  // Found is only returned once current is actually done: closed, and
  // offset at its final size.
  virtual NextFileOutcome Next(const FileCursor &current, std::uint64_t offset,
                               std::unique_ptr<FileCursor> &next,
                               std::string &error) = 0;

  virtual WaitOutcome WaitForNewEvents(const PublishedPosition &target,
                                       std::chrono::milliseconds timeout,
                                       WaitStyle style) = 0;

  virtual PublishedPosition Published() const = 0;
};

}  // namespace binlog_streamer
