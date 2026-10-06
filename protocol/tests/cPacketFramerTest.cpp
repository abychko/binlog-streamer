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

#include "protocol/cPacketFramer.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
#include "protocol/hProtocolLimits.hpp"

namespace binlog_streamer {
namespace {

TEST(PacketFramerTest, EncodesSmallPayloadAsOnePacket) {
  const std::vector<std::uint8_t> payload{0x03, 'a', 'b', 'c'};
  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> out;
  PacketFramer::Encode(payload, sequenceId, out);

  ASSERT_EQ(out.size(), 4 + payload.size());
  EXPECT_EQ(out[0], 0x04);
  EXPECT_EQ(out[1], 0x00);
  EXPECT_EQ(out[2], 0x00);
  EXPECT_EQ(out[3], 0x00);
  EXPECT_EQ(sequenceId, 1);
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), out.begin() + 4));
}

TEST(PacketFramerTest, EncodeStartsFromGivenSequenceIdAndAdvancesIt) {
  const std::vector<std::uint8_t> payload{0x01};
  std::uint8_t sequenceId = 5;
  std::vector<std::uint8_t> out;
  PacketFramer::Encode(payload, sequenceId, out);
  EXPECT_EQ(out[3], 5);
  EXPECT_EQ(sequenceId, 6);
}

TEST(PacketFramerTest, EncodeSplitsExactBoundaryWithEmptyTerminator) {
  const std::vector<std::uint8_t> payload(MAX_PAYLOAD_PER_PACKET, 0x7A);
  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> out;
  PacketFramer::Encode(payload, sequenceId, out);

  ASSERT_EQ(out.size(),
            PACKET_HEADER_SIZE + MAX_PAYLOAD_PER_PACKET + PACKET_HEADER_SIZE);
  EXPECT_EQ(out[0], 0xFF);
  EXPECT_EQ(out[1], 0xFF);
  EXPECT_EQ(out[2], 0xFF);
  EXPECT_EQ(out[3], 0);
  const std::size_t secondHeader = PACKET_HEADER_SIZE + MAX_PAYLOAD_PER_PACKET;
  EXPECT_EQ(out[secondHeader], 0);
  EXPECT_EQ(out[secondHeader + 1], 0);
  EXPECT_EQ(out[secondHeader + 2], 0);
  EXPECT_EQ(out[secondHeader + 3], 1);
  EXPECT_EQ(sequenceId, 2);
}

TEST(PacketFramerTest, EncodeSplitsPayloadOverMaxIntoTwoSubPackets) {
  const std::vector<std::uint8_t> payload(MAX_PAYLOAD_PER_PACKET + 10, 0x11);
  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> out;
  PacketFramer::Encode(payload, sequenceId, out);

  ASSERT_EQ(out.size(), PACKET_HEADER_SIZE + MAX_PAYLOAD_PER_PACKET +
                            PACKET_HEADER_SIZE + 10);
  EXPECT_EQ(out[3], 0);
  const std::size_t secondHeader = PACKET_HEADER_SIZE + MAX_PAYLOAD_PER_PACKET;
  EXPECT_EQ(out[secondHeader], 10);
  EXPECT_EQ(out[secondHeader + 1], 0);
  EXPECT_EQ(out[secondHeader + 2], 0);
  EXPECT_EQ(out[secondHeader + 3], 1);
  EXPECT_EQ(sequenceId, 2);
}

TEST(PacketFramerTest, EncodeOfAHeadAndABodyMatchesEncodeOfThemJoined) {
  std::vector<std::uint8_t> pattern(MAX_PAYLOAD_PER_PACKET);
  for (std::size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = static_cast<std::uint8_t>(i * 7);
  const std::vector<std::uint8_t> heads[] = {{}, {0xA0}, {0xA0, 0xA1, 0xA2}};
  for (const std::vector<std::uint8_t> &head : heads) {
    for (const std::size_t bodySize :
         {std::size_t{0}, std::size_t{1}, MAX_PAYLOAD_PER_PACKET - 3,
          MAX_PAYLOAD_PER_PACKET - 1, MAX_PAYLOAD_PER_PACKET}) {
      const std::span<const std::uint8_t> body(pattern.data(), bodySize);
      std::vector<std::uint8_t> joined(head);
      joined.insert(joined.end(), body.begin(), body.end());

      std::uint8_t joinedSequence = 5, splitSequence = 5;
      std::vector<std::uint8_t> expected, out;
      PacketFramer::Encode(joined, joinedSequence, expected);
      PacketFramer::Encode(head, body, splitSequence, out);
      EXPECT_TRUE(out == expected) << head.size() << " + " << bodySize;
      EXPECT_EQ(splitSequence, joinedSequence);
    }
  }
}

TEST(PacketFramerTest, DecodeReassemblesSinglePacket) {
  const std::vector<std::uint8_t> data{0x04, 0x00, 0x00, 0x00,
                                       0x03, 'a',  'b',  'c'};
  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> payload;
  const auto result = PacketFramer::Decode(data, sequenceId, payload);
  EXPECT_EQ(result.status, PacketDecodeStatus::Complete);
  EXPECT_EQ(result.bytesConsumed, data.size());
  EXPECT_EQ(payload, (std::vector<std::uint8_t>{0x03, 'a', 'b', 'c'}));
  EXPECT_EQ(sequenceId, 1);
}

TEST(PacketFramerTest, DecodeReportsNeedMoreBytesForIncompleteHeader) {
  const std::vector<std::uint8_t> data{0x04, 0x00};
  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> payload;
  const auto result = PacketFramer::Decode(data, sequenceId, payload);
  EXPECT_EQ(result.status, PacketDecodeStatus::NeedMoreBytes);
  EXPECT_GT(result.bytesNeeded, 0u);
  EXPECT_EQ(sequenceId, 0);
}

TEST(PacketFramerTest, DecodeReportsNeedMoreBytesForIncompletePayload) {
  const std::vector<std::uint8_t> data{0x04, 0x00, 0x00, 0x00, 0x03, 'a'};
  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> payload;
  const auto result = PacketFramer::Decode(data, sequenceId, payload);
  EXPECT_EQ(result.status, PacketDecodeStatus::NeedMoreBytes);
  EXPECT_EQ(result.bytesNeeded, 2u);
}

TEST(PacketFramerTest, DecodeReassemblesSubPacketsAtExactBoundary) {
  std::vector<std::uint8_t> data;
  data.insert(data.end(), {0xFF, 0xFF, 0xFF, 0});
  data.insert(data.end(), MAX_PAYLOAD_PER_PACKET, 0x7A);
  data.insert(data.end(), {0, 0, 0, 1});

  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> payload;
  const auto result = PacketFramer::Decode(data, sequenceId, payload);
  EXPECT_EQ(result.status, PacketDecodeStatus::Complete);
  EXPECT_EQ(result.bytesConsumed, data.size());
  EXPECT_EQ(payload.size(), MAX_PAYLOAD_PER_PACKET);
  EXPECT_EQ(sequenceId, 2);
}

TEST(PacketFramerTest, DecodeDetectsSequenceMismatch) {
  const std::vector<std::uint8_t> data{0x01, 0x00, 0x00, 0x07, 0x00};
  std::uint8_t sequenceId = 3;
  std::vector<std::uint8_t> payload;
  const auto result = PacketFramer::Decode(data, sequenceId, payload);
  EXPECT_EQ(result.status, PacketDecodeStatus::SequenceMismatch);
  EXPECT_EQ(sequenceId, 3);
}

TEST(PacketFramerTest,
     MeasureReportsCompleteForASinglePacketWithoutMutatingSequenceId) {
  const std::vector<std::uint8_t> data{0x04, 0x00, 0x00, 0x00,
                                       0x03, 'a',  'b',  'c'};
  const std::uint8_t sequenceId = 0;
  const auto result = PacketFramer::Measure(data, sequenceId);
  EXPECT_EQ(result.status, PacketDecodeStatus::Complete);
  EXPECT_EQ(result.bytesConsumed, data.size());
}

TEST(PacketFramerTest, MeasureReportsNeedMoreBytesForIncompleteHeader) {
  const std::vector<std::uint8_t> data{0x04, 0x00};
  const auto result = PacketFramer::Measure(data, 0);
  EXPECT_EQ(result.status, PacketDecodeStatus::NeedMoreBytes);
  EXPECT_EQ(result.bytesNeeded, 2u);
}

TEST(PacketFramerTest, MeasureReportsNeedMoreBytesForIncompletePayload) {
  const std::vector<std::uint8_t> data{0x04, 0x00, 0x00, 0x00, 0x03, 'a'};
  const auto result = PacketFramer::Measure(data, 0);
  EXPECT_EQ(result.status, PacketDecodeStatus::NeedMoreBytes);
  EXPECT_EQ(result.bytesNeeded, 2u);
}

TEST(PacketFramerTest, MeasureReassemblesSubPacketsAtExactBoundary) {
  std::vector<std::uint8_t> data;
  data.insert(data.end(), {0xFF, 0xFF, 0xFF, 0});
  data.insert(data.end(), MAX_PAYLOAD_PER_PACKET, 0x7A);
  data.insert(data.end(), {0, 0, 0, 1});

  const auto result = PacketFramer::Measure(data, 0);
  EXPECT_EQ(result.status, PacketDecodeStatus::Complete);
  EXPECT_EQ(result.bytesConsumed, data.size());
}

TEST(PacketFramerTest, MeasureDetectsSequenceMismatch) {
  const std::vector<std::uint8_t> data{0x01, 0x00, 0x00, 0x07, 0x00};
  const auto result = PacketFramer::Measure(data, 3);
  EXPECT_EQ(result.status, PacketDecodeStatus::SequenceMismatch);
}

TEST(PacketFramerTest,
     MeasureAndDecodeAgreeOnACompletePacketFollowedByLeftoverBytes) {
  std::vector<std::uint8_t> data{0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c'};
  const std::vector<std::uint8_t> nextPacketHeader{0x01, 0x00, 0x00, 0x01};
  data.insert(data.end(), nextPacketHeader.begin(), nextPacketHeader.end());

  const auto measured = PacketFramer::Measure(data, 0);
  ASSERT_EQ(measured.status, PacketDecodeStatus::Complete);
  EXPECT_EQ(measured.bytesConsumed, 7u);

  std::uint8_t sequenceId = 0;
  std::vector<std::uint8_t> payload;
  const auto decoded = PacketFramer::Decode(data, sequenceId, payload);
  EXPECT_EQ(decoded.bytesConsumed, measured.bytesConsumed);
  EXPECT_EQ(payload, (std::vector<std::uint8_t>{'a', 'b', 'c'}));
}

TEST(PacketFramerTest, EncodeThenDecodeRoundTripsMultiSubPacketPayload) {
  std::vector<std::uint8_t> payload(MAX_PAYLOAD_PER_PACKET * 2 + 123);
  for (std::size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<std::uint8_t>(i);

  std::uint8_t writeSequenceId = 0;
  std::vector<std::uint8_t> wire;
  PacketFramer::Encode(payload, writeSequenceId, wire);

  std::uint8_t readSequenceId = 0;
  std::vector<std::uint8_t> decoded;
  const auto result = PacketFramer::Decode(wire, readSequenceId, decoded);
  EXPECT_EQ(result.status, PacketDecodeStatus::Complete);
  EXPECT_EQ(result.bytesConsumed, wire.size());
  EXPECT_EQ(decoded, payload);
  EXPECT_EQ(readSequenceId, writeSequenceId);
}

}  // namespace
}  // namespace binlog_streamer
