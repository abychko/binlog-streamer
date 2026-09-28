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
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "protocol/cRsaPublicKey.hpp"
#include "protocol/hProtocolLimits.hpp"

namespace binlog_streamer {

// password + trailing NUL, XOR'd cyclically with the nonce, then
// RSA-OAEP-encrypted - reordering any step breaks compatibility with the
// real server.
class CachingSha2FullAuthPassword {
 public:
  // nonce must be the same combined auth-plugin-data
  // CachingSha2Scramble::Compute uses for the fast path. Fails when the
  // password exceeds this key's RSA-OAEP capacity (key size minus 42
  // bytes of fixed OAEP/SHA-1 overhead).
  static bool Encrypt(std::string_view password,
                      std::span<const std::uint8_t, SCRAMBLE_LENGTH> nonce,
                      const RsaPublicKey &publicKey,
                      std::vector<std::uint8_t> &ciphertext,
                      std::string &error);
};

}  // namespace binlog_streamer
