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

#include "net/cByteBuffer.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace binlog_streamer {
namespace {

constexpr std::size_t BUFFER_SIZE = 64;

void FillWithIndices(ByteBuffer &buffer) {
  for (std::size_t i = 0; i < buffer.size(); ++i)
    buffer[i] = static_cast<std::uint8_t>(i);
}

bool HoldsIndices(const ByteBuffer &buffer, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i)
    if (buffer[i] != static_cast<std::uint8_t>(i)) return false;
  return true;
}

TEST(ByteBufferTest, GrowingWithinTheCapacityLeavesTheBytesUnwritten) {
  ByteBuffer buffer;
  buffer.resize(BUFFER_SIZE);
  std::memset(buffer.data(), 0xAB, buffer.size());
  buffer.clear();
  buffer.resize(BUFFER_SIZE);
  EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(),
                          [](std::uint8_t byte) { return byte == 0xAB; }));
}

TEST(ByteBufferTest, GrowingPastTheCapacityKeepsWhatWasThere) {
  ByteBuffer buffer;
  buffer.resize(BUFFER_SIZE);
  FillWithIndices(buffer);
  buffer.resize(buffer.capacity() + 1);
  EXPECT_EQ(buffer.size(), BUFFER_SIZE + 1);
  EXPECT_TRUE(HoldsIndices(buffer, BUFFER_SIZE));
}

TEST(ByteBufferTest, GrowingInSmallStepsDoublesTheCapacity) {
  ByteBuffer buffer;
  buffer.resize(BUFFER_SIZE);
  buffer.resize(BUFFER_SIZE + 1);
  EXPECT_EQ(buffer.capacity(), 2 * BUFFER_SIZE);
}

TEST(ByteBufferTest, ShrinkToFitKeepsTheContentAndLetsTheRestGo) {
  ByteBuffer buffer;
  buffer.resize(BUFFER_SIZE);
  FillWithIndices(buffer);
  buffer.resize(BUFFER_SIZE / 2);
  buffer.shrink_to_fit();
  EXPECT_EQ(buffer.capacity(), BUFFER_SIZE / 2);
  EXPECT_TRUE(HoldsIndices(buffer, BUFFER_SIZE / 2));

  buffer.clear();
  buffer.shrink_to_fit();
  EXPECT_EQ(buffer.capacity(), 0U);
  EXPECT_EQ(buffer.data(), nullptr);
}

TEST(ByteBufferTest, SwapAndMoveHandOverTheBytes) {
  ByteBuffer first;
  first.resize(BUFFER_SIZE);
  FillWithIndices(first);
  ByteBuffer second;
  second.swap(first);
  EXPECT_TRUE(first.empty());
  EXPECT_EQ(second.size(), BUFFER_SIZE);
  EXPECT_TRUE(HoldsIndices(second, BUFFER_SIZE));

  ByteBuffer third(std::move(second));
  EXPECT_EQ(second.capacity(), 0U);
  EXPECT_TRUE(HoldsIndices(third, BUFFER_SIZE));
  first = std::move(third);
  EXPECT_EQ(third.capacity(), 0U);
  EXPECT_TRUE(HoldsIndices(first, BUFFER_SIZE));
}

TEST(ByteBufferTest, PassesAsASpanOfItsBytes) {
  ByteBuffer buffer;
  buffer.resize(BUFFER_SIZE);
  FillWithIndices(buffer);
  const std::span<const std::uint8_t> bytes = buffer;
  EXPECT_EQ(bytes.data(), buffer.data());
  EXPECT_EQ(bytes.size(), BUFFER_SIZE);
}

}  // namespace
}  // namespace binlog_streamer
