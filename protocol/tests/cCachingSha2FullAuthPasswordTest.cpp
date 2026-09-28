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

#include <gtest/gtest.h>
#include <array>
#include <string>
#include <vector>
#include "cCachingSha2FullAuthPasswordTestSupport.hpp"
#include "protocol/cRsaPublicKey.hpp"

namespace binlog_streamer {
namespace {

using test::CachingSha2FullAuthPasswordTestSupport;

std::array<std::uint8_t, SCRAMBLE_LENGTH> MakeNonce() {
  std::array<std::uint8_t, SCRAMBLE_LENGTH> nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i)
    nonce[i] = static_cast<std::uint8_t>(i + 1);
  return nonce;
}

TEST(CachingSha2FullAuthPasswordTest, RoundTripsThroughRsaOaepAndXor) {
  const auto keyPair =
      CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  RsaPublicKey publicKey;
  std::string parseError;
  ASSERT_TRUE(RsaPublicKey::Parse(keyPair.publicKeyPem, publicKey, parseError))
      << parseError;
  const auto nonce = MakeNonce();

  std::vector<std::uint8_t> ciphertext;
  std::string error;
  ASSERT_TRUE(CachingSha2FullAuthPassword::Encrypt("secret", nonce, publicKey,
                                                   ciphertext, error))
      << error;

  const auto plain = CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
      keyPair.privateKey.get(), ciphertext, nonce);
  const std::vector<std::uint8_t> expected{'s', 'e', 'c', 'r', 'e', 't', 0};
  EXPECT_EQ(plain, expected);
}

TEST(CachingSha2FullAuthPasswordTest, EmptyPasswordEncryptsJustTheTerminator) {
  const auto keyPair =
      CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  RsaPublicKey publicKey;
  std::string parseError;
  ASSERT_TRUE(RsaPublicKey::Parse(keyPair.publicKeyPem, publicKey, parseError))
      << parseError;
  const auto nonce = MakeNonce();

  std::vector<std::uint8_t> ciphertext;
  std::string error;
  ASSERT_TRUE(CachingSha2FullAuthPassword::Encrypt("", nonce, publicKey,
                                                   ciphertext, error))
      << error;

  const auto plain = CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
      keyPair.privateKey.get(), ciphertext, nonce);
  EXPECT_EQ(plain, (std::vector<std::uint8_t>{0}));
}

TEST(CachingSha2FullAuthPasswordTest, CyclesTheNonceForPasswordsLongerThanIt) {
  const auto keyPair =
      CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  RsaPublicKey publicKey;
  std::string parseError;
  ASSERT_TRUE(RsaPublicKey::Parse(keyPair.publicKeyPem, publicKey, parseError))
      << parseError;
  const auto nonce = MakeNonce();

  // 25 bytes: longer than the 20-byte nonce, so recovering it correctly
  // requires the XOR index to wrap back to 0 mid-password.
  const std::string password(25, 'a');
  std::vector<std::uint8_t> ciphertext;
  std::string error;
  ASSERT_TRUE(CachingSha2FullAuthPassword::Encrypt(password, nonce, publicKey,
                                                   ciphertext, error))
      << error;

  const auto plain = CachingSha2FullAuthPasswordTestSupport::DecryptAndUnXor(
      keyPair.privateKey.get(), ciphertext, nonce);
  std::vector<std::uint8_t> expected(password.begin(), password.end());
  expected.push_back(0);
  EXPECT_EQ(plain, expected);
}

TEST(CachingSha2FullAuthPasswordTest, RejectsPasswordTooLongForKeyCapacity) {
  // 2048-bit key -> 256-byte cipher, so 214+ bytes of (password + NUL)
  // exceed the OAEP/SHA-1 capacity (256 - 42) well before any RSA_size()
  // truncation could hide the mismatch.
  const auto keyPair =
      CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  RsaPublicKey publicKey;
  std::string parseError;
  ASSERT_TRUE(RsaPublicKey::Parse(keyPair.publicKeyPem, publicKey, parseError))
      << parseError;
  const auto nonce = MakeNonce();

  const std::string password(300, 'x');
  std::vector<std::uint8_t> ciphertext;
  std::string error;
  EXPECT_FALSE(CachingSha2FullAuthPassword::Encrypt(password, nonce, publicKey,
                                                    ciphertext, error));
  EXPECT_FALSE(error.empty());
}

// A password this far past the limit is rejected by OpenSSL's own RSA-OAEP
// even without our capacity check, so the test above alone can't prove our
// check ran. These two pin the exact boundary, one byte on each side.
TEST(CachingSha2FullAuthPasswordTest,
     AcceptsPasswordAtTheOaepCapacityBoundary) {
  // 2048-bit key -> 256-byte cipher, OAEP/SHA-1 capacity 214 bytes of
  // (password+NUL); 213 chars + NUL = 214, the largest size the
  // reference client itself would still encrypt.
  const auto keyPair =
      CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  RsaPublicKey publicKey;
  std::string parseError;
  ASSERT_TRUE(RsaPublicKey::Parse(keyPair.publicKeyPem, publicKey, parseError))
      << parseError;
  const auto nonce = MakeNonce();

  const std::string password(213, 'x');
  std::vector<std::uint8_t> ciphertext;
  std::string error;
  EXPECT_TRUE(CachingSha2FullAuthPassword::Encrypt(password, nonce, publicKey,
                                                   ciphertext, error))
      << error;
}

TEST(CachingSha2FullAuthPasswordTest,
     RejectsPasswordOneByteOverTheOaepCapacityBoundary) {
  const auto keyPair =
      CachingSha2FullAuthPasswordTestSupport::GenerateRsaKeyPair();
  RsaPublicKey publicKey;
  std::string parseError;
  ASSERT_TRUE(RsaPublicKey::Parse(keyPair.publicKeyPem, publicKey, parseError))
      << parseError;
  const auto nonce = MakeNonce();

  const std::string password(214, 'x');
  std::vector<std::uint8_t> ciphertext;
  std::string error;
  EXPECT_FALSE(CachingSha2FullAuthPassword::Encrypt(password, nonce, publicKey,
                                                    ciphertext, error));
  // Pins the source of the rejection to our own capacity check, not to
  // OpenSSL's own OAEP length enforcement inside EVP_PKEY_encrypt (which
  // would instead report "RSA-OAEP encryption failed").
  EXPECT_EQ(
      error,
      "password is too long to be encrypted with the source's RSA public key");
}

}  // namespace
}  // namespace binlog_streamer
