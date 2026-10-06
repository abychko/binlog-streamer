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

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/mman.h>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

std::vector<std::uint8_t> PublicKeyPem(EVP_PKEY *key) {
  const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()),
                                                      &BIO_free);
  PEM_write_bio_PUBKEY(bio.get(), key);
  char *data = nullptr;
  const long len = BIO_get_mem_data(bio.get(), &data);
  return std::vector<std::uint8_t>(data, data + len);
}

TEST(RsaPublicKeyTest, ParsesValidPem) {
  const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keygenCtx(
      EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), &EVP_PKEY_CTX_free);
  ASSERT_TRUE(keygenCtx);
  ASSERT_GT(EVP_PKEY_keygen_init(keygenCtx.get()), 0);
  ASSERT_GT(EVP_PKEY_CTX_set_rsa_keygen_bits(keygenCtx.get(), 2048), 0);
  EVP_PKEY *raw = nullptr;
  ASSERT_GT(EVP_PKEY_keygen(keygenCtx.get(), &raw), 0);
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> rsaKey(
      raw, &EVP_PKEY_free);

  RsaPublicKey key;
  std::string error;
  ASSERT_TRUE(RsaPublicKey::Parse(PublicKeyPem(rsaKey.get()), key, error))
      << error;
  EXPECT_EQ(key.SizeInBytes(), 256u);
}

TEST(RsaPublicKeyTest, RejectsMalformedPem) {
  const std::vector<std::uint8_t> garbage{'n', 'o', 't', ' ', 'a',
                                          ' ', 'k', 'e', 'y'};
  RsaPublicKey key;
  std::string error;
  EXPECT_FALSE(RsaPublicKey::Parse(garbage, key, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(ERR_peek_error(), 0UL);
}

TEST(RsaPublicKeyTest, RejectsEmptyInput) {
  RsaPublicKey key;
  std::string error;
  EXPECT_FALSE(RsaPublicKey::Parse({}, key, error));
  EXPECT_EQ(error, "malformed RSA public key PEM");
}

TEST(RsaPublicKeyTest, RejectsInputLongerThanIntMax) {
  // Just past INT_MAX the narrowing cast goes negative; a span over a too-small
  // array would be UB, so this maps real address space (PROT_READ|MAP_ANON
  // costs no physical memory until touched).
  const std::size_t size =
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
  void *mem = mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  ASSERT_NE(mem, MAP_FAILED);
  const std::span<const std::uint8_t> oversized(
      static_cast<const std::uint8_t *>(mem), size);

  RsaPublicKey key;
  std::string error;
  EXPECT_FALSE(RsaPublicKey::Parse(oversized, key, error));
  EXPECT_EQ(error, "RSA public key PEM is too large");

  munmap(mem, size);
}

TEST(RsaPublicKeyTest, RejectsNonRsaKey) {
  const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keygenCtx(
      EVP_PKEY_CTX_new_from_name(nullptr, "ED25519", nullptr),
      &EVP_PKEY_CTX_free);
  ASSERT_TRUE(keygenCtx);
  ASSERT_GT(EVP_PKEY_keygen_init(keygenCtx.get()), 0);
  EVP_PKEY *raw = nullptr;
  ASSERT_GT(EVP_PKEY_keygen(keygenCtx.get(), &raw), 0);
  const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> edKey(
      raw, &EVP_PKEY_free);

  RsaPublicKey key;
  std::string error;
  EXPECT_FALSE(RsaPublicKey::Parse(PublicKeyPem(edKey.get()), key, error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace binlog_streamer
