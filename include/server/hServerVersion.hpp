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

// What a replica reads as the version of the server it is attached to:
// the version of the source whose events this relay stores, with the
// relay's own name and version after it - 8.4.11-binlog-streamer-0.27.0.
// A replica's IO thread refuses a source whose greeting does not start
// with a numeric major >= 5, and the source's own version supplies that
// much without this relay inventing a number of its own.
//
// Empty when the source version is not known: the relay has stored
// nothing yet, so there is nothing to serve and no version to name.
std::string ServerVersionString(const std::string &sourceVersion,
                                const std::string &relayName,
                                const std::string &relayVersion);

// How a relay tells that its source is a relay too, regardless of the
// version the chain started from: the name and version a relay appends
// end the string.
bool IsRelayServerVersion(const std::string &serverVersion,
                          const std::string &relayName);

}  // namespace binlog_streamer
