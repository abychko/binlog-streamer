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
#include <cstddef>
#include <cstdint>
#include "net/sPacketChannelOptions.hpp"
#include "protocol/hCapabilityFlags.hpp"

namespace binlog_streamer {

// Every flag hCapabilityFlags.hpp defines, limited to what this relay's
// codec actually implements (e.g. no CLIENT_QUERY_ATTRIBUTES).
// CLIENT_ZSTD_COMPRESSION_ALGORITHM is absent on purpose: it is added
// per connection, from source.yml, not implied by the codec.
inline constexpr std::uint32_t REPLICA_CLIENT_CAPABILITIES =
    CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
    CLIENT_TRANSACTIONS | CLIENT_RESERVED2 | CLIENT_MULTI_RESULTS |
    CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_ATTRS |
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | CLIENT_SESSION_TRACK |
    CLIENT_DEPRECATE_EOF;

// Matches the reference client's default max_allowed_packet
// (sql-common/client.cc).
inline constexpr std::uint32_t REPLICA_MAX_PACKET_SIZE =
    1024UL * 1024UL * 1024UL;

// latin1_swedish_ci, the reference client's default (mysys/charset.cc).
// Only affects how the source reads the login packet's strings.
inline constexpr std::uint8_t REPLICA_CHARACTER_SET = 8;

inline constexpr char CACHING_SHA2_PASSWORD_PLUGIN_NAME[] =
    "caching_sha2_password";

// Matches replica_net_timeout (sys_vars.cc); used for every pre-dump
// network operation since this relay has no separate timeouts yet.
inline constexpr std::chrono::milliseconds REPLICA_NET_TIMEOUT{60'000};

// A replica's own default is min(configured maximum,
// replica_net_timeout/2) (rpl_replica.cc); with no configured maximum
// yet, this is half of REPLICA_NET_TIMEOUT.
inline constexpr std::chrono::seconds HEARTBEAT_PERIOD{30};

// Matches SOURCE_CONNECT_RETRY/SOURCE_RETRY_COUNT defaults; counts
// total connection attempts, not retries beyond the first.
inline constexpr std::chrono::seconds CONNECT_RETRY_INTERVAL{60};
inline constexpr unsigned CONNECT_ATTEMPTS = 10;

inline constexpr unsigned MINIMUM_SUPPORTED_MAJOR_VERSION = 8;

// Verified against a live 8.4.11 source (SELECT @@GLOBAL.<nonexistent>);
// several pre-dump steps treat it specially.
inline constexpr std::uint16_t ER_UNKNOWN_SYSTEM_VARIABLE = 1193;

// Turns a source stuck sending an unbounded pre-dump response into a
// clear error instead of unbounded memory growth. The dump/event stream
// has its own, larger limit.
inline constexpr std::size_t MAX_COMMAND_RESPONSE_SIZE = 16UL * 1024UL * 1024UL;

inline constexpr PacketChannelOptions SOURCE_CHANNEL_OPTIONS{
    REPLICA_NET_TIMEOUT, REPLICA_NET_TIMEOUT, MAX_COMMAND_RESPONSE_SIZE,
    "source"};

}  // namespace binlog_streamer
