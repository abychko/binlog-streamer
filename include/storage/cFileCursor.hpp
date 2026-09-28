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

#include <cstdint>
#include <string>
#include "storage/cFilePin.hpp"

namespace binlog_streamer {

class StorageReader;

// A FilePin blocks purge while this cursor is alive; a read-only fd
// lets StorageReader::Read() pread() it independently of the writer's
// own descriptor on the same file.
class FileCursor {
 public:
  FileCursor() = default;
  ~FileCursor();
  FileCursor(FileCursor &&other) noexcept;
  FileCursor &operator=(FileCursor &&other) noexcept;
  FileCursor(const FileCursor &) = delete;
  FileCursor &operator=(const FileCursor &) = delete;

  const std::string &FileName() const { return m_pin.FileName(); }

 private:
  // On-disk records open immediately; memory-only records defer
  // open(2) until Fd() is first needed for a disk read.
  friend class StorageReader;
  FileCursor(FilePin pin, int fd, std::string path, std::uint64_t headerLength);
  // Belongs to one reader thread; lazy opening here is deliberately
  // unsynchronized, including through a const reference.
  int Fd(std::string &error) const;

  FilePin m_pin;
  mutable int m_fd = -1;
  std::string m_path;
  std::uint64_t m_headerLength = 0;
  mutable bool m_previousReadFromDisk = false;
  // Whether this file was the published one when the published file last
  // had number m_checkedMark; still true or false until that number moves.
  mutable std::uint64_t m_checkedMark = 0;
  mutable bool m_published = false;
};

}  // namespace binlog_streamer
