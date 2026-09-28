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

#include <chrono>
#include <cstdint>
#include <string>

namespace binlog_streamer {

// Populated incrementally; fields for steps not reached when a session
// ends early are left at their defaults.
struct SourceIdentity {
  std::string versionString;
  unsigned versionMajor = 0;
  // major*10000 + minor*100 + patch, matching mysql_get_server_version()
  // - lets version-gated logic use one numeric comparison instead of
  // re-parsing versionString.
  std::uint32_t versionNumber = 0;
  std::uint32_t serverId = 0;
  std::string checksumAlgorithm;  // @source_binlog_checksum, or "OFF" if
                                  // unsupported
  std::string gtidMode;
  std::string serverUuid;
  // The source's own clock (ReportClockSkew()), since events carry the
  // source's clock, not this relay's.
  std::uint64_t unixTimestamp = 0;
  std::chrono::steady_clock::time_point unixTimestampReadAt{};  // default
                                                                // means no
                                                                // timestamp
                                                                // was
                                                                // received yet
  bool registered = false;
};

}  // namespace binlog_streamer
