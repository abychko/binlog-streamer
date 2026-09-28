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

// What is known about a replica once it logged in and does not change
// while it stays connected.
struct ReplicaFacts {
  std::string address;  // "ip:port" it connected from
  std::string user;
  bool tls = false;
  std::string compression;  // "none", "zlib" or "zstd"
  // Connection attributes the replica sent: program_name ("mysqld" for a
  // server) and _client_version (a server's own version, e.g. "8.4.6-6").
  // Empty when not sent.
  std::string program;
  std::string version;
};

}  // namespace binlog_streamer
