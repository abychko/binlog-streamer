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

namespace binlog_streamer {

// A CA and a server certificate it signed, the way auto_generate_certs
// gives a MySQL server a certificate to start with
// (sql/auth/sql_authentication.cc): RSA 2048, SHA-256, valid ten years, the CA
// self-signed. Self-signed means encryption only: a peer verifying the chain
// against its own CA store, or the name, rejects it - as it rejects MySQL's.
struct GeneratedCertificates {
  std::string caCertPem;
  std::string caKeyPem;
  std::string serverCertPem;
  std::string serverKeyPem;
};

class TlsCertificateGenerator {
 public:
  // name goes into the subjects, e.g. "binlog-streamer" gives
  // CN=binlog-streamer_Auto_Generated_CA_Certificate.
  static bool Generate(const std::string &name, GeneratedCertificates &out,
                       std::string &error);
};

}  // namespace binlog_streamer
