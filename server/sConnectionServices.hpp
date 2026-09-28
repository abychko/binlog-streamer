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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include "protocol/eCompressionAlgorithm.hpp"

namespace binlog_streamer {

class BinlogStorageReader;
class DumpSessionRegistry;
class QueryResponder;
class RelayStatusTracker;
class TlsContext;

// What a connection needs beyond its own socket, shared between
// connections and owned by the listener or the application. Leaving a
// field null switches the matching command off (error 1047), for testing one
// part alone.
struct ConnectionServices {
  const QueryResponder *queryResponder = nullptr;
  BinlogStorageReader *storageReader = nullptr;
  DumpSessionRegistry *dumpSessions = nullptr;
  std::string serverUuid;
  std::uint32_t serverId = 0;
  const std::atomic<bool> *stopRequested = nullptr;
  // What the greeting offers a replica. None advertises no compression
  // bit, so every replica that can fall back to uncompressed does.
  CompressionAlgorithm compression = CompressionAlgorithm::None;
  // The relay's certificate; null advertises no CLIENT_SSL, and a
  // replica insisting on TLS is refused as by a server without one.
  const TlsContext *tls = nullptr;
  // Refuses a replica that stays in the clear (ERR 3159), as
  // require_secure_transport does.
  bool requireSecureTransport = false;
  // Where a logged-in replica is registered for the status page; null
  // registers none.
  RelayStatusTracker *status = nullptr;
  // server.send_linger: zero sends each dump's events at once.
  std::chrono::microseconds sendLinger{0};
};

}  // namespace binlog_streamer
