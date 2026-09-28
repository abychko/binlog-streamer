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

// The five values of MySQL's --ssl-mode, in its order of strictness
// (include/mysql.h, enum mysql_ssl_mode): a relay connecting to its
// source is a client, and takes the client's setting.
enum class SslMode {
  Disabled,        // never asks for TLS
  Preferred,       // TLS if the source offers it, plain otherwise
  Required,        // TLS or no session; the certificate is not checked
  VerifyCa,        // plus the chain has to end in ssl_ca
  VerifyIdentity,  // plus the certificate has to name the host
};

constexpr std::string_view SslModeName(SslMode mode) {
  switch (mode) {
    case SslMode::Disabled:
      return "DISABLED";
    case SslMode::Preferred:
      return "PREFERRED";
    case SslMode::Required:
      return "REQUIRED";
    case SslMode::VerifyCa:
      return "VERIFY_CA";
    case SslMode::VerifyIdentity:
      return "VERIFY_IDENTITY";
  }
  return "";
}

}  // namespace binlog_streamer
