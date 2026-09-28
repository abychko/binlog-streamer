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
#include "gtid/cGtidSet.hpp"

namespace binlog_streamer {

// Kept in memory rather than on disk - the same way MySQL's own
// binlog.index holds only bare names, nothing else, per file.
struct StoredFileRecord {
  std::string
      name;  // "<basename>.<digits>", bare - matches binlog.index's own entries
  std::string basename;
  std::uint64_t number = 0;

  // The current published boundary comes from PublishedPositionTracker,
  // not this field.
  std::uint64_t size = 0;

  // Where cBinlogTailScanner starts reading a file's tail; never 0.
  std::uint64_t headerLength = 0;

  // The FDE's Common-Header timestamp, not its body's "created" field -
  // that one is zeroed on every rotated-into file.
  std::uint32_t createdAt = 0;

  // Tells a genuine resumed file apart from one written by a different
  // source reusing the name.
  std::uint32_t serverId = 0;

  // The FDE's server_version: the source that wrote these events, which
  // a relay passes through unchanged, so a chain of them still names the
  // server the stream started at.
  std::string serverVersion;

  // "OFF"/"CRC32"/"UNDEF"/"UNKNOWN(n)" - FormatDescriptionEventCodec's own
  // vocabulary (sFormatDescriptionEvent.hpp).
  std::string checksumAlgorithm;

  // True for at most the last record in a consistent catalog; set on
  // an earlier file means corruption, not reopened for appending.
  bool inUse = false;

  // False until the writer has created the file; readers open it lazily.
  bool onDisk = true;

  GtidSet previousGtids;
};

}  // namespace binlog_streamer
