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
#include "net/eSslMode.hpp"
#include "net/sTlsMaterial.hpp"

// Keeps <openssl/ssl.h> out of this header.
struct ssl_ctx_st;

namespace binlog_streamer {

// One SSL_CTX, loaded once from PEM text and shared by every TlsTransport
// of the same side: the listener's connections all present the same
// certificate, and every session to the source checks it the same way.
class TlsContext {
 public:
  TlsContext() = default;
  ~TlsContext();
  TlsContext(const TlsContext &) = delete;
  TlsContext &operator=(const TlsContext &) = delete;

  // certPem and keyPem are required; caPem, when set, is what a client
  // certificate has to chain to. A client presenting none is accepted, as
  // a MySQL server with ssl_ca accepts one (vio/viosslfactories.cc).
  bool LoadServer(const TlsMaterial &material, std::string &error);

  // mode decides what is checked of the source's certificate; VerifyCa
  // and VerifyIdentity require caPem. certPem/keyPem, when set, are the
  // relay's own client certificate, presented if the source asks.
  bool LoadClient(SslMode mode, const TlsMaterial &material,
                  std::string &error);

  // Whether the material loads at all - each PEM parses, the key matches
  // the certificate - without deciding a side; what a configuration check
  // asks before any connection is made.
  static bool Check(const TlsMaterial &material, std::string &error);
  bool Loaded() const { return m_context != nullptr; }
  SslMode mode() const { return m_mode; }
  ssl_ctx_st *Raw() const { return m_context; }

 private:
  bool LoadMaterial(const TlsMaterial &material, std::string &error);

  ssl_ctx_st *m_context = nullptr;
  SslMode m_mode = SslMode::Disabled;
};

}  // namespace binlog_streamer
