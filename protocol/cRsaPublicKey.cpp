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

#include "protocol/cRsaPublicKey.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <limits>

namespace binlog_streamer {

void RsaPublicKey::Deleter::operator()(EVP_PKEY *key) const noexcept {
  EVP_PKEY_free(key);
}

bool RsaPublicKey::Parse(std::span<const std::uint8_t> pem, RsaPublicKey &key,
                         std::string &error) {
  // An empty span's data() may be null, and BIO_new_mem_buf then fails with a
  // misleading 'failed to allocate'.
  if (pem.empty()) {
    error = "malformed RSA public key PEM";
    return false;
  }
  // BIO_new_mem_buf takes an int length; past INT_MAX the cast would go
  // negative and it would read via strlen().
  if (pem.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    error = "RSA public key PEM is too large";
    return false;
  }
  const std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), &BIO_free);
  if (!bio) {
    error = "failed to allocate a memory BIO for the RSA public key PEM";
    return false;
  }
  std::unique_ptr<EVP_PKEY, Deleter> parsed(
      PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
  if (!parsed) {
    // A failed parse leaves an entry on OpenSSL's error queue; clear it so it
    // does not surface in a later, unrelated call.
    ERR_clear_error();
    error = "malformed RSA public key PEM";
    return false;
  }
  if (EVP_PKEY_get_base_id(parsed.get()) != EVP_PKEY_RSA) {
    error = "public key is not RSA";
    return false;
  }
  key.m_key = std::move(parsed);
  error.clear();
  return true;
}

std::size_t RsaPublicKey::SizeInBytes() const {
  return static_cast<std::size_t>(EVP_PKEY_get_size(m_key.get()));
}

}  // namespace binlog_streamer
