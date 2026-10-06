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
#include "protocol/eCompressionAlgorithm.hpp"

namespace binlog_streamer {

inline constexpr std::uint32_t CLIENT_LONG_PASSWORD = 1;
inline constexpr std::uint32_t CLIENT_LONG_FLAG = 4;
inline constexpr std::uint32_t CLIENT_CONNECT_WITH_DB = 8;
// The only way to ask for zlib; unlike zstd's, this bit carries no level.
inline constexpr std::uint32_t CLIENT_COMPRESS = 32;
inline constexpr std::uint32_t CLIENT_PROTOCOL_41 = 512;
inline constexpr std::uint32_t CLIENT_SSL = 2048;
inline constexpr std::uint32_t CLIENT_TRANSACTIONS = 8192;
inline constexpr std::uint32_t CLIENT_RESERVED2 = 32768;
inline constexpr std::uint32_t CLIENT_MULTI_RESULTS = 1UL << 17;
inline constexpr std::uint32_t CLIENT_PLUGIN_AUTH = 1UL << 19;
inline constexpr std::uint32_t CLIENT_CONNECT_ATTRS = 1UL << 20;
inline constexpr std::uint32_t CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 1UL
                                                                       << 21;
inline constexpr std::uint32_t CLIENT_SESSION_TRACK = 1UL << 23;
inline constexpr std::uint32_t CLIENT_DEPRECATE_EOF = 1UL << 24;
inline constexpr std::uint32_t CLIENT_ZSTD_COMPRESSION_ALGORITHM = 1UL << 26;

// A peer may set both; MySQL then prefers zlib, but this relay offers one at a
// time and masks the answer with what it offered.
constexpr std::uint32_t CompressionCapabilityBit(
    CompressionAlgorithm algorithm) {
  switch (algorithm) {
    case CompressionAlgorithm::Zstd:
      return CLIENT_ZSTD_COMPRESSION_ALGORITHM;
    case CompressionAlgorithm::Zlib:
      return CLIENT_COMPRESS;
    case CompressionAlgorithm::None:
      break;
  }
  return 0;
}

constexpr int CompressionLevelInEffect(CompressionAlgorithm algorithm,
                                       int requestedZstdLevel) {
  return algorithm == CompressionAlgorithm::Zlib
             ? DEFAULT_ZLIB_COMPRESSION_LEVEL
             : requestedZstdLevel;
}

}  // namespace binlog_streamer
