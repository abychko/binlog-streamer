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

#include <string_view>

namespace binlog_streamer {

// The algorithm named on the wire, not the library implementing it:
// Zlib stays "zlib" to a peer even though zlib-ng compresses it.
enum class CompressionAlgorithm {
  None,
  Zstd,
  Zlib,
};

// The names MySQL's own compression options use (include/compression.h,
// COMPRESSION_ALGORITHM_*); also the values of the `compression` key.
constexpr std::string_view CompressionAlgorithmName(
    CompressionAlgorithm algorithm) {
  switch (algorithm) {
    case CompressionAlgorithm::Zstd:
      return "zstd";
    case CompressionAlgorithm::Zlib:
      return "zlib";
    case CompressionAlgorithm::None:
      break;
  }
  return "uncompressed";
}

// What a peer may ask for, and what we ask for when nothing is
// configured (sql-common/compression.cc, is_zstd_compression_level_valid;
// include/compression.h, default_zstd_compression_level).
inline constexpr int MIN_ZSTD_COMPRESSION_LEVEL = 1;
inline constexpr int MAX_ZSTD_COMPRESSION_LEVEL = 22;
inline constexpr int DEFAULT_ZSTD_COMPRESSION_LEVEL = 3;

// CLIENT_COMPRESS carries no level on the wire, so each side picks its
// own and MySQL always picks 6 (mysys/my_compress.cc,
// mysql_default_compression_level; sql/auth/sql_authentication.cc sets
// the same 6 for an inbound connection). The bounds are zlib's
// Z_NO_COMPRESSION..Z_BEST_COMPRESSION.
inline constexpr int MIN_ZLIB_COMPRESSION_LEVEL = 0;
inline constexpr int MAX_ZLIB_COMPRESSION_LEVEL = 9;
inline constexpr int DEFAULT_ZLIB_COMPRESSION_LEVEL = 6;

}  // namespace binlog_streamer
