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

namespace binlog_streamer {

// The source's version followed by the relay's own name and version. Empty
// while the source version is unknown.
std::string ServerVersionString(const std::string &sourceVersion,
                                const std::string &relayName,
                                const std::string &relayVersion);

// A trailing relay name and version tells a relay that its source is a relay
// too.
bool IsRelayServerVersion(const std::string &serverVersion,
                          const std::string &relayName);

}  // namespace binlog_streamer
