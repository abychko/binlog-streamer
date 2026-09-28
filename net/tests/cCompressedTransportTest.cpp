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

#include "net/cCompressedTransport.hpp"

#include <zlib.h>
#include <zstd.h>

#include <gtest/gtest.h>
#include <chrono>
#include <numeric>
#include <string>
#include <vector>
#include "cFakeTransport.hpp"
#include "net/cPacketChannel.hpp"
#include "protocol/cPacketFramer.hpp"

namespace binlog_streamer {
namespace {

constexpr std::chrono::milliseconds TEST_TIMEOUT{1000};
constexpr std::chrono::milliseconds TEST_CONTINUATION_TIMEOUT{30'000};
constexpr std::size_t TEST_MAX_PACKET_SIZE = 16UL * 1024UL * 1024UL;
constexpr PacketChannelOptions TEST_OPTIONS{TEST_TIMEOUT, TEST_TIMEOUT,
                                            TEST_MAX_PACKET_SIZE, "peer"};

// Bytes that zstd cannot shrink, to reach the "compressed is not smaller"
// branch without depending on how well a given level does on a given input.
std::vector<std::uint8_t> Incompressible(std::size_t size) {
  std::vector<std::uint8_t> data(size);
  std::uint32_t state = 0x12345678;
  for (std::uint8_t &byte : data) {
    state = state * 1664525U + 1013904223U;
    byte = static_cast<std::uint8_t>(state >> 24);
  }
  return data;
}

std::vector<std::uint8_t> Compressible(std::size_t size) {
  std::vector<std::uint8_t> data(size);
  std::iota(data.begin(), data.end(), static_cast<std::uint8_t>(0));
  for (std::size_t i = 0; i < size; ++i)
    data[i] = static_cast<std::uint8_t>('a' + (i % 4));
  return data;
}

// Wraps an already-encoded body in the 7-byte header, so the tests below
// can build a frame carrying anything, valid or not.
void AppendBody(std::vector<std::uint8_t> &out,
                std::span<const std::uint8_t> body, std::uint8_t sequenceId,
                std::size_t plainLength) {
  out.push_back(static_cast<std::uint8_t>(body.size()));
  out.push_back(static_cast<std::uint8_t>(body.size() >> 8));
  out.push_back(static_cast<std::uint8_t>(body.size() >> 16));
  out.push_back(sequenceId);
  out.push_back(static_cast<std::uint8_t>(plainLength));
  out.push_back(static_cast<std::uint8_t>(plainLength >> 8));
  out.push_back(static_cast<std::uint8_t>(plainLength >> 16));
  out.insert(out.end(), body.begin(), body.end());
}

// Builds a frame the way a peer would, through the reference libraries
// directly rather than through the code under test.
void AppendFrame(std::vector<std::uint8_t> &out,
                 std::span<const std::uint8_t> payload, std::uint8_t sequenceId,
                 bool compress) {
  if (!compress) {
    AppendBody(out, payload, sequenceId, 0);
    return;
  }
  std::vector<std::uint8_t> body(ZSTD_compressBound(payload.size()));
  const std::size_t produced =
      ZSTD_compress(body.data(), body.size(), payload.data(), payload.size(),
                    DEFAULT_ZSTD_COMPRESSION_LEVEL);
  body.resize(produced);
  AppendBody(out, body, sequenceId, payload.size());
}

// The same through zlib's own compress2(), the call MySQL makes
// (mysys/my_compress.cc, zlib_compress_alloc).
void AppendZlibFrame(std::vector<std::uint8_t> &out,
                     std::span<const std::uint8_t> payload,
                     std::uint8_t sequenceId) {
  std::vector<std::uint8_t> body(compressBound(payload.size()));
  uLongf produced = static_cast<uLongf>(body.size());
  EXPECT_EQ(compress2(body.data(), &produced, payload.data(),
                      static_cast<uLong>(payload.size()),
                      DEFAULT_ZLIB_COMPRESSION_LEVEL),
            Z_OK);
  body.resize(produced);
  AppendBody(out, body, sequenceId, payload.size());
}

std::size_t Load3(const std::uint8_t *bytes) {
  return static_cast<std::size_t>(bytes[0]) |
         (static_cast<std::size_t>(bytes[1]) << 8) |
         (static_cast<std::size_t>(bytes[2]) << 16);
}

// Reads everything the transport will deliver, in small pieces, so the
// test also exercises serving one frame across several Read() calls.
std::vector<std::uint8_t> ReadAll(CompressedTransport &transport,
                                  std::size_t expected, std::string &error) {
  std::vector<std::uint8_t> received;
  while (received.size() < expected) {
    std::uint8_t chunk[64] = {};
    std::size_t bytesRead = 0;
    const ReadOutcome outcome =
        transport.Read(chunk, bytesRead, TEST_TIMEOUT, error);
    if (outcome != ReadOutcome::Data) break;
    received.insert(received.end(), chunk, chunk + bytesRead);
  }
  return received;
}

TEST(CompressedTransportTest, PassesBytesThroughUntilEnabled) {
  test::FakeTransport inner;
  inner.incoming = {1, 2, 3, 4};
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  EXPECT_FALSE(transport.Enabled());

  std::uint8_t buffer[4] = {};
  std::size_t bytesRead = 0;
  std::string error;
  ASSERT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Data)
      << error;
  EXPECT_EQ(bytesRead, 4u);
  EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + 4), inner.incoming);

  const std::vector<std::uint8_t> payload{9, 8, 7};
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;
  ASSERT_EQ(inner.writes.size(), 1u);
  EXPECT_EQ(inner.writes[0], payload);  // no frame header added
}

TEST(CompressedTransportTest, PayloadBelowTheCompressionThresholdGoesAsIs) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload =
      Compressible(MIN_COMPRESS_LENGTH - 1);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 1u);
  const std::vector<std::uint8_t> &frame = inner.writes[0];
  ASSERT_EQ(frame.size(), COMPRESSED_HEADER_SIZE + payload.size());
  EXPECT_EQ(Load3(frame.data()), payload.size());
  EXPECT_EQ(frame[3], 0u);
  EXPECT_EQ(Load3(frame.data() + 4), 0u)  // 0: not compressed
      << "a payload shorter than MIN_COMPRESS_LENGTH must not be compressed";
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                         frame.begin() + COMPRESSED_HEADER_SIZE));
}

TEST(CompressedTransportTest, PayloadAtTheThresholdIsCompressed) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload = Compressible(MIN_COMPRESS_LENGTH);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 1u);
  const std::vector<std::uint8_t> &frame = inner.writes[0];
  EXPECT_EQ(Load3(frame.data() + 4), MIN_COMPRESS_LENGTH);
  EXPECT_LT(Load3(frame.data()), MIN_COMPRESS_LENGTH);
}

TEST(CompressedTransportTest, PayloadThatWouldGrowGoesAsIs) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload = Incompressible(64);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 1u);
  const std::vector<std::uint8_t> &frame = inner.writes[0];
  EXPECT_EQ(Load3(frame.data() + 4), 0u);
  EXPECT_EQ(Load3(frame.data()), payload.size());
}

TEST(CompressedTransportTest, PayloadLongerThanOneFrameIsSplit) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload =
      Compressible(MAX_COMPRESSED_FRAME_PAYLOAD + 1);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 2u);
  EXPECT_EQ(Load3(inner.writes[0].data() + 4), MAX_COMPRESSED_FRAME_PAYLOAD);
  EXPECT_EQ(inner.writes[0][3], 0u);
  EXPECT_EQ(inner.writes[1][3], 1u);
  EXPECT_EQ(Load3(inner.writes[1].data()), 1u);  // one byte, stored as is
}

TEST(CompressedTransportTest, ReadsBackWhatAnotherInstanceWrote) {
  test::FakeTransport writerSide;
  CompressedTransport writer(writerSide, TEST_CONTINUATION_TIMEOUT);
  writer.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  const std::vector<std::uint8_t> payload = Compressible(100'000);
  std::string error;
  ASSERT_TRUE(writer.Write(payload, TEST_TIMEOUT, error)) << error;

  test::FakeTransport readerSide;
  for (const std::vector<std::uint8_t> &frame : writerSide.writes)
    readerSide.incoming.insert(readerSide.incoming.end(), frame.begin(),
                               frame.end());
  CompressedTransport reader(readerSide, TEST_CONTINUATION_TIMEOUT);
  reader.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  EXPECT_EQ(ReadAll(reader, payload.size(), error), payload) << error;
}

TEST(CompressedTransportTest, ReassemblesAFrameDeliveredOneByteAtATime) {
  const std::vector<std::uint8_t> payload = Compressible(500);
  test::FakeTransport inner;
  AppendFrame(inner.incoming, payload, 0, true);
  inner.maxBytesPerRead = 1;

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::string error;
  EXPECT_EQ(ReadAll(transport, payload.size(), error), payload) << error;

  // The read that starts a frame waits on the caller's timeout; every
  // read continuing it waits on the continuation timeout.
  ASSERT_FALSE(inner.readTimeouts.empty());
  EXPECT_EQ(inner.readTimeouts.front(), TEST_TIMEOUT);
  for (std::size_t i = 1; i < inner.readTimeouts.size(); ++i)
    EXPECT_EQ(inner.readTimeouts[i], TEST_CONTINUATION_TIMEOUT) << "read " << i;
}

TEST(CompressedTransportTest, RejectsAFrameWithAnUnexpectedSequenceId) {
  test::FakeTransport inner;
  AppendFrame(inner.incoming, Compressible(100), 5, true);

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_EQ(error, "protocol desync: unexpected compressed packet sequence id");
}

TEST(CompressedTransportTest, ResetSequenceStartsTheCounterOver) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  const std::vector<std::uint8_t> payload = Compressible(100);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;
  EXPECT_EQ(inner.writes[1][3], 1u);

  transport.ResetSequence();
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;
  EXPECT_EQ(inner.writes[2][3], 0u);
}

TEST(CompressedTransportTest, RejectsAFrameWhoseBodyIsNotZstd) {
  test::FakeTransport inner;
  // Declares 100 bytes before compression, carries bytes zstd cannot read.
  inner.incoming = {4, 0, 0, 0, 100, 0, 0, 'j', 'u', 'n', 'k'};

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_NE(error.find("cannot decompress a packet"), std::string::npos)
      << error;
}

TEST(CompressedTransportTest, RejectsAFrameWhoseDeclaredLengthIsWrong) {
  const std::vector<std::uint8_t> payload = Compressible(100);
  test::FakeTransport inner;
  AppendFrame(inner.incoming, payload, 0, true);
  inner.incoming[4] = 99;  // one byte short of what the frame decompresses to

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_NE(error.find("cannot decompress a packet"), std::string::npos)
      << error;
}

TEST(CompressedTransportTest, RejectsAFrameThatDecompressesToLessThanDeclared) {
  const std::vector<std::uint8_t> payload = Compressible(100);
  test::FakeTransport inner;
  AppendFrame(inner.incoming, payload, 0, true);
  // One byte more than the frame decompresses to: zstd fits in the buffer
  // and reports no error, so only the length check catches this.
  inner.incoming[4] = 101;

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_NE(error.find("decompressed to 100 bytes, header declares 101"),
            std::string::npos)
      << error;
}

TEST(CompressedTransportTest, ReportsTheInnerOutcomeWhenNoFrameHasStarted) {
  test::FakeTransport inner;
  inner.scriptedOutcomes = {ReadOutcome::TimedOut, ReadOutcome::Interrupted,
                            ReadOutcome::Closed};

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::TimedOut);
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Interrupted);
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Closed);
  EXPECT_EQ(bytesRead, 0u);
}

TEST(CompressedTransportTest, ResumesAFrameInterruptedPartWayThrough) {
  const std::vector<std::uint8_t> payload = Compressible(500);
  test::FakeTransport inner;
  AppendFrame(inner.incoming, payload, 0, true);
  inner.maxBytesPerRead = 4;
  // Interrupted after the first four bytes of the frame have arrived.
  inner.scriptedOutcomes = {ReadOutcome::Data, ReadOutcome::Interrupted};

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  ASSERT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Interrupted);
  EXPECT_EQ(ReadAll(transport, payload.size(), error), payload) << error;
}

// The whole point of the decorator: a channel above it reads ordinary
// packets, whatever the frames below carry.

TEST(CompressedTransportTest, ChannelReadsSeveralPacketsFromOneFrame) {
  std::vector<std::uint8_t> packets;
  std::uint8_t sequenceId = 7;  // the ids inside a frame are not checked
  PacketFramer::Encode(std::vector<std::uint8_t>{1, 2, 3}, sequenceId, packets);
  sequenceId = 0;  // and need not even be monotonic
  PacketFramer::Encode(std::vector<std::uint8_t>{4, 5}, sequenceId, packets);
  sequenceId = 200;
  PacketFramer::Encode(std::vector<std::uint8_t>{6}, sequenceId, packets);

  test::FakeTransport inner;
  AppendFrame(inner.incoming, packets, 0, true);
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  PacketChannel channel(transport, TEST_OPTIONS);

  std::vector<std::uint8_t> payload;
  std::string error;
  ASSERT_TRUE(channel.ReadPacket(payload, error)) << error;
  EXPECT_EQ(payload, (std::vector<std::uint8_t>{1, 2, 3}));
  ASSERT_TRUE(channel.ReadPacket(payload, error)) << error;
  EXPECT_EQ(payload, (std::vector<std::uint8_t>{4, 5}));
  ASSERT_TRUE(channel.ReadPacket(payload, error)) << error;
  EXPECT_EQ(payload, (std::vector<std::uint8_t>{6}));
}

TEST(CompressedTransportTest, ChannelReadsAPacketSplitAcrossFrames) {
  std::vector<std::uint8_t> packets;
  std::uint8_t sequenceId = 0;
  const std::vector<std::uint8_t> payload = Compressible(100);
  PacketFramer::Encode(payload, sequenceId, packets);

  test::FakeTransport inner;
  const std::span<const std::uint8_t> all(packets);
  AppendFrame(inner.incoming, all.subspan(0, 30), 0, false);
  AppendFrame(inner.incoming, all.subspan(30), 1, false);

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  PacketChannel channel(transport, TEST_OPTIONS);

  std::vector<std::uint8_t> received;
  std::string error;
  ASSERT_TRUE(channel.ReadPacket(received, error)) << error;
  EXPECT_EQ(received, payload);
}

TEST(CompressedTransportTest, ChannelStillChecksPacketIdsWithoutCompression) {
  std::uint8_t sequenceId = 3;  // a channel starts at 0, so this mismatches
  test::FakeTransport inner;
  PacketFramer::Encode(std::vector<std::uint8_t>{1, 2, 3}, sequenceId,
                       inner.incoming);

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  PacketChannel channel(transport, TEST_OPTIONS);
  std::vector<std::uint8_t> payload;
  std::string error;
  EXPECT_FALSE(channel.ReadPacket(payload, error));
  EXPECT_EQ(error, "protocol desync: unexpected packet sequence id from peer");
}

TEST(CompressedTransportTest, ChannelResetSequenceAlsoResetsTheFrameCounter) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  PacketChannel channel(transport, TEST_OPTIONS);

  std::string error;
  ASSERT_TRUE(channel.WritePacket(std::vector<std::uint8_t>{1}, error))
      << error;
  ASSERT_TRUE(channel.WritePacket(std::vector<std::uint8_t>{2}, error))
      << error;
  EXPECT_EQ(inner.writes[1][3], 1u);

  channel.ResetSequence();
  ASSERT_TRUE(channel.WritePacket(std::vector<std::uint8_t>{3}, error))
      << error;
  EXPECT_EQ(inner.writes[2][3], 0u);
}

TEST(CompressedTransportTest, ChannelKeepsItsLimitOnTheDecompressedStream) {
  // One frame carrying a packet header that declares more than the
  // channel's limit: the limit applies above the decorator, to the
  // decompressed stream, as it does in mysqld.
  constexpr std::size_t SMALL_LIMIT = 1024;
  const PacketChannelOptions options{TEST_TIMEOUT, TEST_TIMEOUT, SMALL_LIMIT,
                                     "peer"};
  const std::vector<std::uint8_t> header{0x00, 0x10, 0x00, 0x00};  // 4096 bytes
  test::FakeTransport inner;
  AppendFrame(inner.incoming, header, 0, false);

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zstd, DEFAULT_ZSTD_COMPRESSION_LEVEL);
  PacketChannel channel(transport, options);

  std::vector<std::uint8_t> payload;
  std::string error;
  EXPECT_FALSE(channel.ReadPacket(payload, error));
  EXPECT_EQ(error, "packet from peer larger than the 1024-byte limit");
}

// zlib, the algorithm CLIENT_COMPRESS names. Only what differs from zstd
// is repeated here: the framing, the sequence ids, the reassembly and the
// channel above them are one code path, exercised by the tests above.

TEST(CompressedTransportTest, ZlibFrameIsWhatTheReferenceZlibProduces) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload = Compressible(500);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 1u);
  const std::vector<std::uint8_t> &frame = inner.writes[0];
  ASSERT_EQ(Load3(frame.data() + 4), payload.size());
  ASSERT_EQ(Load3(frame.data()), frame.size() - COMPRESSED_HEADER_SIZE);

  // The peer is MySQL, which inflates the body with plain uncompress()
  // (mysys/my_compress.cc, zlib_uncompress): what this transport writes
  // has to be an RFC 1950 stream, not a raw deflate one.
  std::vector<std::uint8_t> inflated(payload.size());
  uLongf inflatedSize = static_cast<uLongf>(inflated.size());
  ASSERT_EQ(
      uncompress(inflated.data(), &inflatedSize,
                 frame.data() + COMPRESSED_HEADER_SIZE,
                 static_cast<uLong>(frame.size() - COMPRESSED_HEADER_SIZE)),
      Z_OK);
  EXPECT_EQ(inflatedSize, payload.size());
  EXPECT_EQ(inflated, payload);
}

TEST(CompressedTransportTest, ReadsAZlibFrameTheReferenceZlibProduced) {
  const std::vector<std::uint8_t> payload = Compressible(5000);
  test::FakeTransport inner;
  AppendZlibFrame(inner.incoming, payload, 0);

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);
  std::string error;
  EXPECT_EQ(ReadAll(transport, payload.size(), error), payload) << error;
}

TEST(CompressedTransportTest, ZlibReadsBackWhatAnotherInstanceWrote) {
  test::FakeTransport writerSide;
  CompressedTransport writer(writerSide, TEST_CONTINUATION_TIMEOUT);
  writer.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);
  const std::vector<std::uint8_t> payload = Compressible(100'000);
  std::string error;
  ASSERT_TRUE(writer.Write(payload, TEST_TIMEOUT, error)) << error;

  test::FakeTransport readerSide;
  for (const std::vector<std::uint8_t> &frame : writerSide.writes)
    readerSide.incoming.insert(readerSide.incoming.end(), frame.begin(),
                               frame.end());
  CompressedTransport reader(readerSide, TEST_CONTINUATION_TIMEOUT);
  reader.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);
  EXPECT_EQ(ReadAll(reader, payload.size(), error), payload) << error;
}

TEST(CompressedTransportTest, ZlibPayloadBelowTheThresholdGoesAsIs) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload =
      Compressible(MIN_COMPRESS_LENGTH - 1);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 1u);
  EXPECT_EQ(Load3(inner.writes[0].data() + 4), 0u);
  EXPECT_EQ(Load3(inner.writes[0].data()), payload.size());
}

TEST(CompressedTransportTest, ZlibPayloadThatWouldNotShrinkGoesAsIs) {
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload = Incompressible(64);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;

  ASSERT_EQ(inner.writes.size(), 1u);
  EXPECT_EQ(Load3(inner.writes[0].data() + 4), 0u);
  EXPECT_EQ(Load3(inner.writes[0].data()), payload.size());
}

TEST(CompressedTransportTest, RejectsAFrameWhoseBodyIsNotZlib) {
  test::FakeTransport inner;
  // Declares 100 bytes before compression, carries bytes zlib cannot read.
  inner.incoming = {4, 0, 0, 0, 100, 0, 0, 'j', 'u', 'n', 'k'};

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_NE(error.find("cannot decompress a packet"), std::string::npos)
      << error;
}

TEST(CompressedTransportTest,
     RejectsAZlibFrameThatInflatesPastItsDeclaredSize) {
  const std::vector<std::uint8_t> payload = Compressible(100);
  test::FakeTransport inner;
  AppendZlibFrame(inner.incoming, payload, 0);
  inner.incoming[4] = 99;  // one byte short of what the frame inflates to

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_NE(error.find("cannot decompress a packet"), std::string::npos)
      << error;
}

TEST(CompressedTransportTest, RejectsAZlibFrameThatInflatesToLessThanDeclared) {
  const std::vector<std::uint8_t> payload = Compressible(100);
  test::FakeTransport inner;
  AppendZlibFrame(inner.incoming, payload, 0);
  // One byte more than the frame inflates to: zlib fits in the buffer and
  // reports no error, so only the length check catches this.
  inner.incoming[4] = 101;

  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, DEFAULT_ZLIB_COMPRESSION_LEVEL);
  std::uint8_t buffer[16] = {};
  std::size_t bytesRead = 0;
  std::string error;
  EXPECT_EQ(transport.Read(buffer, bytesRead, TEST_TIMEOUT, error),
            ReadOutcome::Failed);
  EXPECT_NE(error.find("decompressed to 100 bytes, header declares 101"),
            std::string::npos)
      << error;
}

TEST(CompressedTransportTest, ZlibLevelOutsideItsRangeIsClamped) {
  // The relay never asks for one - CLIENT_COMPRESS carries no level - but
  // a level clamped into zstd's 1..22 would leave zlib's compress2()
  // rejecting it with Z_STREAM_ERROR and the frame going uncompressed.
  test::FakeTransport inner;
  CompressedTransport transport(inner, TEST_CONTINUATION_TIMEOUT);
  transport.Enable(CompressionAlgorithm::Zlib, MAX_ZSTD_COMPRESSION_LEVEL);

  const std::vector<std::uint8_t> payload = Compressible(500);
  std::string error;
  ASSERT_TRUE(transport.Write(payload, TEST_TIMEOUT, error)) << error;
  EXPECT_EQ(Load3(inner.writes[0].data() + 4), payload.size())
      << "the frame was sent uncompressed";
}

}  // namespace
}  // namespace binlog_streamer
