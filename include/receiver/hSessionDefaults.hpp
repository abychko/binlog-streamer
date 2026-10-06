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

// Limited to what the codec implements; CLIENT_ZSTD_COMPRESSION_ALGORITHM is
// added per connection, from source.yml.
inline constexpr std::uint32_t REPLICA_CLIENT_CAPABILITIES =
    CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
    CLIENT_TRANSACTIONS | CLIENT_RESERVED2 | CLIENT_MULTI_RESULTS |
    CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_ATTRS |
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | CLIENT_SESSION_TRACK |
    CLIENT_DEPRECATE_EOF;

inline constexpr std::uint32_t REPLICA_MAX_PACKET_SIZE =
    1024UL * 1024UL * 1024UL;

inline constexpr std::uint8_t REPLICA_CHARACTER_SET = 8;

inline constexpr char CACHING_SHA2_PASSWORD_PLUGIN_NAME[] =
    "caching_sha2_password";

inline constexpr std::chrono::milliseconds REPLICA_NET_TIMEOUT{60'000};

inline constexpr std::chrono::seconds HEARTBEAT_PERIOD{30};

inline constexpr std::chrono::seconds CONNECT_RETRY_INTERVAL{60};
inline constexpr unsigned CONNECT_ATTEMPTS = 10;

inline constexpr unsigned MINIMUM_SUPPORTED_MAJOR_VERSION = 8;

inline constexpr std::uint16_t ER_UNKNOWN_SYSTEM_VARIABLE = 1193;

// Bounds a pre-dump response; the dump/event stream has its own, larger limit.
inline constexpr std::size_t MAX_COMMAND_RESPONSE_SIZE = 16UL * 1024UL * 1024UL;

inline constexpr PacketChannelOptions SOURCE_CHANNEL_OPTIONS{
    REPLICA_NET_TIMEOUT, REPLICA_NET_TIMEOUT, MAX_COMMAND_RESPONSE_SIZE,
    "source"};

}  // namespace binlog_streamer
