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

#include "protocol/cCachingSha2Scramble.hpp"

#include <openssl/evp.h>

namespace binlog_streamer {
namespace {

std::array<std::uint8_t, CachingSha2Scramble::LENGTH> Sha256(
    std::span<const std::uint8_t> parts0,
    std::span<const std::uint8_t> parts1 = {}) {
  std::array<std::uint8_t, CachingSha2Scramble::LENGTH> digest{};
  // EVP_Digest, not the deprecated one-shot SHA256(), to build cleanly under
  // OpenSSL 3 with -Werror.
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  if (!parts0.empty()) EVP_DigestUpdate(ctx, parts0.data(), parts0.size());
  if (!parts1.empty()) EVP_DigestUpdate(ctx, parts1.data(), parts1.size());
  unsigned int digestLength = 0;
  EVP_DigestFinal_ex(ctx, digest.data(), &digestLength);
  EVP_MD_CTX_free(ctx);
  return digest;
}

}  // namespace

std::array<std::uint8_t, CachingSha2Scramble::LENGTH>
CachingSha2Scramble::Compute(
    std::string_view password,
    std::span<const std::uint8_t, SCRAMBLE_LENGTH> nonce) {
  const std::span<const std::uint8_t> passwordBytes(
      reinterpret_cast<const std::uint8_t *>(password.data()), password.size());
  const auto digestStage1 = Sha256(passwordBytes);
  const auto digestStage2 = Sha256(digestStage1);
  const auto scrambleStage1 = Sha256(digestStage2, nonce);

  std::array<std::uint8_t, LENGTH> scramble{};
  for (std::size_t i = 0; i < LENGTH; ++i)
    scramble[i] = digestStage1[i] ^ scrambleStage1[i];
  return scramble;
}

}  // namespace binlog_streamer
