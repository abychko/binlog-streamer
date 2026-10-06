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
#include <string>
#include "config/hConfigDefaults.hpp"
#include "net/eSslMode.hpp"
#include "net/sTlsMaterial.hpp"
#include "protocol/eCompressionAlgorithm.hpp"

namespace binlog_streamer {

struct SourceSettings {
  std::string host;
  std::uint16_t port = DEFAULT_SOURCE_PORT;
  std::string user;
  std::string password;
  bool getSourcePublicKey = false;
  std::string sourcePublicKeyPath;
  // Raw PEM, not parsed: avoids reopening the file between validation and use
  // (TOCTOU).
  std::string sourcePublicKeyPem;

  // None asks for nothing; anything else is strict: a source without the
  // capability ends the session.
  CompressionAlgorithm compression = CompressionAlgorithm::None;
  int zstdCompressionLevel = DEFAULT_ZSTD_COMPRESSION_LEVEL;

  // Anything but PREFERRED is strict: a source that does not offer TLS ends the
  // session.
  SslMode sslMode = SslMode::Disabled;
  std::string sslCa;
  std::string sslCert;
  std::string sslKey;
  TlsMaterial tls;
  bool operator==(const SourceSettings &) const = default;
};

}  // namespace binlog_streamer
