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

// Test-only key material for caching_sha2_password full authentication: a
// private/public RSA key pair generated fresh by
// CachingSha2FullAuthPasswordTestSupport, so tests avoid a fixture key file.

#include <openssl/evp.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace binlog_streamer::test {

struct RsaTestKeyPair {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> privateKey{
      nullptr, &EVP_PKEY_free};
  std::vector<std::uint8_t> publicKeyPem;
};

}  // namespace binlog_streamer::test
