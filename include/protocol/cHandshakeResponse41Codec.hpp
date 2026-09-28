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
#include <vector>
#include "protocol/sHandshakeResponse41.hpp"

namespace binlog_streamer {

class HandshakeResponse41Codec {
 public:
  static void Encode(const HandshakeResponse41 &value,
                     std::vector<std::uint8_t> &out);
  // The SSL request: the same 32-byte fixed header alone, sent in the
  // clear before the TLS handshake; the full response follows over TLS
  // (sql-common/client.cc, CLIENT_SSL).
  static void EncodeSslRequest(const HandshakeResponse41 &value,
                               std::vector<std::uint8_t> &out);
  // True for a packet that asks to switch to TLS before authenticating.
  static bool IsSslRequest(std::span<const std::uint8_t> payload);
  static bool Parse(std::span<const std::uint8_t> payload,
                    HandshakeResponse41 &value, std::string &error);
};

}  // namespace binlog_streamer
