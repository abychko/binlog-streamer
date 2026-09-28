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

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(CachingSha2ScrambleTest, MatchesIndependentlyComputedReferenceValue) {
  // Cross-checked against an independent Python implementation
  // (hashlib.sha256, not OpenSSL): d1=sha256(pw); d2=sha256(d1);
  // s1=sha256(d2+nonce); scramble=xor(d1,s1).
  std::array<std::uint8_t, 20> nonce{};
  for (std::size_t i = 0; i < nonce.size(); ++i)
    nonce[i] = static_cast<std::uint8_t>(i + 1);

  const auto scramble = CachingSha2Scramble::Compute("secret", nonce);

  const std::array<std::uint8_t, 32> expected{
      116, 110, 190, 32, 93,  86,  160, 112, 122, 203, 62,
      121, 110, 131, 78, 13,  215, 177, 214, 23,  67,  178,
      107, 213, 32,  44, 122, 98,  50,  48,  199, 201};
  EXPECT_EQ(scramble, expected);
}

TEST(CachingSha2ScrambleTest, DifferentNoncesProduceDifferentScrambles) {
  // Guards against an implementation that ignores the nonce entirely
  // (e.g. only hashing the password).
  std::array<std::uint8_t, 20> nonceA{};
  std::array<std::uint8_t, 20> nonceB{};
  for (std::size_t i = 0; i < nonceA.size(); ++i) {
    nonceA[i] = static_cast<std::uint8_t>(i);
    nonceB[i] = static_cast<std::uint8_t>(i + 1);
  }
  EXPECT_NE(CachingSha2Scramble::Compute("secret", nonceA),
            CachingSha2Scramble::Compute("secret", nonceB));
}

}  // namespace
}  // namespace binlog_streamer
