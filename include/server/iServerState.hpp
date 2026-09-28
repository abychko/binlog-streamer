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
#include <optional>
#include <string>

namespace binlog_streamer {

struct PreviousGtidsEvent {
  std::uint64_t position = 0;
  std::uint64_t endPosition = 0;
  std::uint32_t serverId = 0;
  std::string gtids;
};

// Called from every connection's thread, so an implementation must be
// safe for concurrent use.
class ServerState {
 public:
  virtual ~ServerState() = default;

  // Counterpart of a server's gtid_purged.
  virtual std::string GtidPurged() const = 0;

  // Counterpart of a server's gtid_executed; may trail what was
  // received in the last moments.
  virtual std::string GtidExecuted() const = 0;

  // The server_version of the Format_description_event of the newest
  // stored file: the source this relay serves the events of. Empty while
  // nothing is stored - the relay then has no version to name and no
  // events to hand out.
  virtual std::string SourceVersion() const = 0;

  // "CRC32" or "NONE", counterpart of a server's binlog_checksum.
  virtual std::string BinlogChecksum() const = 0;
  // nullopt for a file the relay does not hold. A relay whose source is
  // this relay asks for it to learn where its start file begins.
  virtual std::optional<PreviousGtidsEvent> PreviousGtids(
      const std::string &fileName) const = 0;
};

}  // namespace binlog_streamer
