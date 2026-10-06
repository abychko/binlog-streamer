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

#include "net/cPacketChannel.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include "cFakeTransport.hpp"
#include "protocol/cPacketFramer.hpp"

namespace binlog_streamer {
namespace {

constexpr std::size_t TEST_MAX_PACKET_SIZE = 16UL * 1024UL * 1024UL;
constexpr PacketChannelOptions TEST_OPTIONS{std::chrono::milliseconds{1000},
                                            std::chrono::milliseconds{1000},
                                            TEST_MAX_PACKET_SIZE, "peer"};

TEST(PacketChannelTest, ReassemblesPacketByteExactWhenDeliveredOneByteAtATime) {
  const std::vector<std::uint8_t> payload{0x03, 'a', 'b', 'c'};
  std::uint8_t writeSequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(payload, writeSequenceId, transport.incoming);
  transport.maxBytesPerRead = 1;

  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> received;
  std::string error;
  ASSERT_TRUE(channel.ReadPacket(received, error)) << error;
  EXPECT_EQ(received, payload);
  EXPECT_EQ(transport.readCallCount, transport.incoming.size());
}

TEST(PacketChannelTest, RejectsAPacketLargerThanItsLimitWithoutRepeatedGrowth) {
  test::FakeTransport transport;
  transport.incoming = {0xFF, 0xFF, 0xFF, 0x00};

  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> received;
  std::string error;
  EXPECT_FALSE(channel.ReadPacket(received, error));
  EXPECT_EQ(error, "packet from peer larger than the 16777216-byte limit");
  EXPECT_EQ(transport.readCallCount, 1u);
}

TEST(PacketChannelTest, InterruptedReadIsReportedThroughWasInterrupted) {
  test::FakeTransport transport;
  transport.scriptedOutcomes = {ReadOutcome::Interrupted};

  PacketChannel channel(transport, TEST_OPTIONS);
  EXPECT_FALSE(channel.WasInterrupted());
  std::vector<std::uint8_t> received;
  std::string error;
  EXPECT_FALSE(channel.ReadPacket(received, error));
  EXPECT_TRUE(channel.WasInterrupted());
}

TEST(PacketChannelTest, IdleTimeoutAppliesOnlyToTheFirstReadOfAFreshPacket) {
  const std::vector<std::uint8_t> payload{0x03, 'a', 'b', 'c'};
  std::uint8_t writeSequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(payload, writeSequenceId, transport.incoming);
  transport.maxBytesPerRead = 1;

  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> received;
  std::string error;
  constexpr std::chrono::milliseconds IDLE_TIMEOUT{28'800'000};
  ASSERT_TRUE(channel.ReadPacket(received, error, IDLE_TIMEOUT)) << error;
  EXPECT_EQ(received, payload);

  ASSERT_EQ(transport.readTimeouts.size(), transport.incoming.size());
  EXPECT_EQ(transport.readTimeouts.front(), IDLE_TIMEOUT);
  for (std::size_t i = 1; i < transport.readTimeouts.size(); ++i)
    EXPECT_EQ(transport.readTimeouts[i], TEST_OPTIONS.readTimeout)
        << "at index " << i;
}

TEST(PacketChannelTest,
     IdleTimeoutDoesNotApplyWhenThisPacketIsAlreadyPartlyBuffered) {
  const std::vector<std::uint8_t> firstPayload{0x01, 0xAA};
  const std::vector<std::uint8_t> secondPayload{0x02, 0xBB, 0xCC};
  std::uint8_t sequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(firstPayload, sequenceId, transport.incoming);
  std::vector<std::uint8_t> secondEncoded;
  PacketFramer::Encode(secondPayload, sequenceId, secondEncoded);
  transport.incoming.insert(transport.incoming.end(), secondEncoded.begin(),
                            secondEncoded.end() - 1);

  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> received;
  std::string error;
  ASSERT_TRUE(channel.ReadPacket(received, error)) << error;
  EXPECT_EQ(received, firstPayload);
  const unsigned readsBeforeSecondCall = transport.readTimeouts.size();

  transport.incoming.push_back(secondEncoded.back());
  constexpr std::chrono::milliseconds IDLE_TIMEOUT{28'800'000};
  ASSERT_TRUE(channel.ReadPacket(received, error, IDLE_TIMEOUT)) << error;
  EXPECT_EQ(received, secondPayload);

  ASSERT_GT(transport.readTimeouts.size(), readsBeforeSecondCall);
  EXPECT_EQ(transport.readTimeouts[readsBeforeSecondCall],
            TEST_OPTIONS.readTimeout);
}

TEST(PacketChannelTest, LeavesOverreadBytesBufferedForTheNextReadPacketCall) {
  const std::vector<std::uint8_t> firstPayload{0x01, 0xAA};
  const std::vector<std::uint8_t> secondPayload{0x02, 0xBB, 0xCC};
  std::uint8_t sequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(firstPayload, sequenceId, transport.incoming);
  PacketFramer::Encode(secondPayload, sequenceId, transport.incoming);

  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> received;
  std::string error;
  ASSERT_TRUE(channel.ReadPacket(received, error)) << error;
  EXPECT_EQ(received, firstPayload);
  const unsigned readsAfterFirstPacket = transport.readCallCount;

  ASSERT_TRUE(channel.ReadPacket(received, error)) << error;
  EXPECT_EQ(received, secondPayload);
  EXPECT_EQ(transport.readCallCount, readsAfterFirstPacket);
}

TEST(PacketChannelTest, QueuedPacketsGoOutTogetherAndAheadOfTheNextWrittenOne) {
  test::FakeTransport transport;
  PacketChannel channel(transport, TEST_OPTIONS);
  std::string error;
  const std::vector<std::uint8_t> first{1, 2};
  const std::vector<std::uint8_t> second{3};
  const std::vector<std::uint8_t> third{4, 5, 6};
  ASSERT_TRUE(channel.QueuePacket(first, error)) << error;
  ASSERT_TRUE(channel.QueuePacket(second, error)) << error;
  EXPECT_TRUE(transport.writes.empty());
  ASSERT_TRUE(channel.WritePacket(third, error)) << error;

  std::vector<std::uint8_t> expected;
  std::uint8_t sequenceId = 0;
  PacketFramer::Encode(first, sequenceId, expected);
  PacketFramer::Encode(second, sequenceId, expected);
  PacketFramer::Encode(third, sequenceId, expected);
  ASSERT_EQ(transport.writes.size(), 1U);
  EXPECT_EQ(transport.writes[0], expected);
  EXPECT_TRUE(channel.Flush(error));
  EXPECT_EQ(transport.writes.size(), 1U);
}

TEST(PacketChannelTest, QueuedPacketsGoOutOnTheirOwnOnceEnoughHasGathered) {
  test::FakeTransport transport;
  PacketChannel channel(transport, TEST_OPTIONS);
  std::string error;
  const std::vector<std::uint8_t> payload(1000, 0xAB);
  std::size_t queued = 0;
  while (transport.writes.empty() && queued < 100) {
    ASSERT_TRUE(channel.QueuePacket(payload, error)) << error;
    ++queued;
  }
  ASSERT_EQ(transport.writes.size(), 1U);
  EXPECT_EQ(queued, 66U);
  EXPECT_EQ(transport.writes[0].size(), queued * 1004);
}

TEST(PacketChannelTest, TakeUnreadHandsOverWhatWasReadPastThePacket) {
  const std::vector<std::uint8_t> first{0x01, 0x02};
  const std::vector<std::uint8_t> second{0x03, 0x04, 0x05};
  std::uint8_t writeSequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(first, writeSequenceId, transport.incoming);
  const std::size_t firstEnd = transport.incoming.size();
  PacketFramer::Encode(second, writeSequenceId, transport.incoming);
  const std::vector<std::uint8_t> secondFramed(
      transport.incoming.begin() + static_cast<std::ptrdiff_t>(firstEnd),
      transport.incoming.end());

  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> received;
  std::string error;
  ASSERT_TRUE(channel.ReadPacket(received, error)) << error;
  EXPECT_EQ(received, first);
  EXPECT_EQ(channel.TakeUnread(), secondFramed);
  EXPECT_TRUE(channel.TakeUnread().empty());
  EXPECT_FALSE(channel.ReadPacket(received, error));
}

}  // namespace
}  // namespace binlog_streamer
