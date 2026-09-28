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

#include "protocol/cCachingSha2FullAuthPassword.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <memory>

namespace binlog_streamer {

namespace {
// RSA-OAEP's fixed overhead with SHA-1/MGF1 (RFC 8017 SS7.1.1: max message
// length is k - 2*hLen - 2). OpenSSL exposes no named constant; the
// reference client hardcodes the same 41/42 split.
constexpr int OAEP_SHA1_OVERHEAD = 42;
}  // namespace

bool CachingSha2FullAuthPassword::Encrypt(
    std::string_view password,
    std::span<const std::uint8_t, SCRAMBLE_LENGTH> nonce,
    const RsaPublicKey &publicKey, std::vector<std::uint8_t> &ciphertext,
    std::string &error) {
  // Password + NUL terminator, XOR'd with the nonce (matches the reference
  // client's xor_string()). Explicit loop, not memcpy(): GCC 14's
  // -Wstringop-overflow (RelWithDebInfo) flags a false overflow otherwise.
  std::vector<std::uint8_t> obfuscated(password.size() + 1);
  for (std::size_t i = 0; i < password.size(); ++i)
    obfuscated[i] =
        static_cast<std::uint8_t>(password[i]) ^ nonce[i % nonce.size()];
  obfuscated[password.size()] =
      nonce[password.size() % nonce.size()];  // XOR of the NUL terminator (0)
                                              // is the nonce byte itself

  const int keySize = static_cast<int>(publicKey.SizeInBytes());
  if (static_cast<int>(obfuscated.size()) > keySize - OAEP_SHA1_OVERHEAD) {
    error =
        "password is too long to be encrypted with the source's RSA public key";
    return false;
  }

  const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
      EVP_PKEY_CTX_new(publicKey.Native(), nullptr), &EVP_PKEY_CTX_free);
  if (!ctx) {
    error = "failed to allocate an RSA encryption context";
    return false;
  }
  if (EVP_PKEY_encrypt_init(ctx.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) <= 0) {
    error = "failed to initialize RSA-OAEP encryption";
    return false;
  }
  // Two-call form (EVP_PKEY_encrypt(3)): the first call only sizes the
  // output and performs no encryption, so ciphertext is allocated exactly
  // instead of over-allocated to some assumed maximum.
  std::size_t outLen = 0;
  if (EVP_PKEY_encrypt(ctx.get(), nullptr, &outLen, obfuscated.data(),
                       obfuscated.size()) <= 0) {
    error = "RSA-OAEP encryption failed";
    return false;
  }
  ciphertext.resize(outLen);
  if (EVP_PKEY_encrypt(ctx.get(), ciphertext.data(), &outLen, obfuscated.data(),
                       obfuscated.size()) <= 0) {
    error = "RSA-OAEP encryption failed";
    return false;
  }
  ciphertext.resize(outLen);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
