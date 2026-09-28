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
  bool getSourcePublicKey = false;  // mirrors GET_SOURCE_PUBLIC_KEY; fallback
                                    // when no local key file is set
  std::string sourcePublicKeyPath;  // mirrors SOURCE_PUBLIC_KEY_PATH; tried
                                    // before asking the source
  // Raw PEM, not a parsed RsaPublicKey: keeps SourceSettings copyable and
  // avoids reopening the file, closing a TOCTOU window between validation and
  // use.
  std::string sourcePublicKeyPem;

  // What the relay asks the source for. None asks for nothing; anything
  // else is a strict request - a source that does not offer the matching
  // capability ends the session rather than falling back silently.
  // Uncompressed by default, as a replication channel's own
  // compression_algorithm is (sql/rpl_mi.cc): asking costs the source's
  // CPU, so it is opted into. The other direction defaults the other way
  // for the same reason - offering costs nothing (sReplicaSettings.hpp).
  CompressionAlgorithm compression = CompressionAlgorithm::None;
  // Ignored unless compression is Zstd. The source spends the CPU, the
  // relay picks the level, exactly as a replica does to this relay.
  int zstdCompressionLevel = DEFAULT_ZSTD_COMPRESSION_LEVEL;

  // In the clear unless set, as a replication channel is until SOURCE_SSL
  // is given; the values and their meaning are the client
  // library's --ssl-mode. Anything but PREFERRED is strict: a source that
  // does not offer TLS ends the session.
  SslMode sslMode = SslMode::Disabled;
  std::string sslCa;    // mirrors SOURCE_SSL_CA; what VERIFY_* checks against
  std::string sslCert;  // mirrors SOURCE_SSL_CERT, with sslKey: the relay's
  std::string sslKey;   // own certificate, when the source asks for one
  // The three files' contents, read while the configuration is loaded,
  // for the same reason sourcePublicKeyPem is.
  TlsMaterial tls;
  bool operator==(const SourceSettings &) const = default;
};

}  // namespace binlog_streamer
