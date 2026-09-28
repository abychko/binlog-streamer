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

// Shared by cCachingSha2FullAuthPasswordTest.cpp and cReplicaSessionTest.cpp:
// generates a fresh RSA key pair and reverses Encrypt's own RSA-OAEP +
// nonce XOR, independently of its indexing. Header-only, no matching .cpp.

#include "protocol/hProtocolLimits.hpp"
#include "sRsaTestKeyPair.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace binlog_streamer::test {

class CachingSha2FullAuthPasswordTestSupport {
 public:
  static RsaTestKeyPair GenerateRsaKeyPair() {
    RsaTestKeyPair result;
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keygenCtx(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr),
        &EVP_PKEY_CTX_free);
    EVP_PKEY_keygen_init(keygenCtx.get());
    EVP_PKEY_CTX_set_rsa_keygen_bits(keygenCtx.get(), 2048);
    EVP_PKEY *raw = nullptr;
    EVP_PKEY_keygen(keygenCtx.get(), &raw);
    result.privateKey.reset(raw);

    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()),
                                                        &BIO_free);
    PEM_write_bio_PUBKEY(bio.get(), result.privateKey.get());
    char *data = nullptr;
    const long len = BIO_get_mem_data(bio.get(), &data);
    result.publicKeyPem.assign(data, data + len);
    return result;
  }

  // So a bug in either step shows up as a mismatch, not a cancelling bug in
  // both.
  static std::vector<std::uint8_t> DecryptAndUnXor(
      EVP_PKEY *privateKey, const std::vector<std::uint8_t> &ciphertext,
      std::span<const std::uint8_t, SCRAMBLE_LENGTH> nonce) {
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(
        EVP_PKEY_CTX_new(privateKey, nullptr), &EVP_PKEY_CTX_free);
    EVP_PKEY_decrypt_init(ctx.get());
    EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING);
    std::size_t outLen = 0;
    EVP_PKEY_decrypt(ctx.get(), nullptr, &outLen, ciphertext.data(),
                     ciphertext.size());
    std::vector<std::uint8_t> obfuscated(outLen);
    EVP_PKEY_decrypt(ctx.get(), obfuscated.data(), &outLen, ciphertext.data(),
                     ciphertext.size());
    obfuscated.resize(outLen);

    std::vector<std::uint8_t> plain(obfuscated.size());
    for (std::size_t i = 0; i < obfuscated.size(); ++i)
      plain[i] = obfuscated[i] ^ nonce[i % nonce.size()];
    return plain;
  }
};

}  // namespace binlog_streamer::test
