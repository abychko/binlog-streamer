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
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer {

// Writes files the way MYSQL_BIN_LOG does (sql/binlog.cc). Not
// thread-safe: one writer per data directory.
class BinlogFileWriter {
 public:
  BinlogFileWriter() = default;
  ~BinlogFileWriter();
  BinlogFileWriter(const BinlogFileWriter &) = delete;
  BinlogFileWriter &operator=(const BinlogFileWriter &) = delete;

  // Forces fdeBytes' "file in use" bit to 1: the source always clears
  // it before sending an FDE over the wire, but a stored file must
  // record it as open.
  bool Create(const std::string &path, std::span<const std::uint8_t> fdeBytes,
              std::span<const std::uint8_t> previousGtidsBytes,
              std::string &error);

  bool OpenExisting(const std::string &path, std::string &error);

  // Buffers into WRITE_BUFFER_SIZE, flushing when full; a call at least
  // that long writes directly instead. Buffered bytes flush first, so
  // file order always matches call order.
  bool Append(std::span<const std::uint8_t> bytes, std::string &error);

  // Does not fsync - callers wanting durability call Sync() too.
  bool Flush(std::string &error);

  bool Sync(std::string &error);

  // Clears the bit by masking, not writing literal 0 like MySQL does.
  // Does not flush the write buffer first - call Flush() before
  // MarkClosed(), then Sync().
  bool MarkClosed(std::string &error);

  // Fails untouched if length exceeds Size()+buffered - that would let
  // ftruncate(2) zero-pad a gap instead of truncating.
  bool Truncate(std::uint64_t length, std::string &error);

  // Buffered-but-unflushed bytes are not counted.
  std::uint64_t Size() const { return length_; }

  // Unbuffered append; do not mix with Append without flushing first.
  bool WriteDirect(std::span<const std::uint8_t> bytes, std::string &error,
                   std::uint64_t *calls = nullptr);

 private:
  int fd_ = -1;
  std::vector<std::uint8_t> buffer_;
  std::uint64_t length_ = 0;
};

}  // namespace binlog_streamer
