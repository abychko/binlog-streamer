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

#include "receiver/cEventStreamReader.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "binlog/hEventLimits.hpp"
#include "cFakeTransport.hpp"
#include "cRecordingEventSink.hpp"
#include "cScriptedStreamBuilder.hpp"
#include "protocol/cPacketFramer.hpp"
#include "protocol/hProtocolLimits.hpp"

namespace binlog_streamer {
namespace {

StreamPosition StartPosition() { return StreamPosition{"binlog.000001", 100}; }

TEST(EventStreamReaderTest, DeliversByteExactEventWithATinyBufferAndSlowReads) {
  test::ScriptedStreamBuilder builder;
  const std::string body(200, 'x');  // bigger than the 64-byte buffer below
  // nextPosition must agree with the running position, or the sync check
  // rejects the event before this test's actual subject ever runs.
  const std::uint32_t eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + body.size() + 4);
  builder.PushEvent(
      33 /* GTID_EVENT */, std::vector<std::uint8_t>(body.begin(), body.end()),
      4, static_cast<std::uint32_t>(StartPosition().position + eventLength));
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  // Small and non-power-of-two, to stress reads that never align with a
  // header/event boundary.
  transport.maxBytesPerRead = 5;

  test::RecordingEventSink sink;
  StreamReaderOptions options;
  options.checksumLength = 4;
  options.bufferSize = 64;
  EventStreamReader reader(transport, sink, StartPosition(), options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_TRUE(sink.events[0].ended);
  EXPECT_EQ(sink.events[0].header.type, 33);
  EXPECT_EQ(sink.events[0].header.eventLength,
            EVENT_HEADER_LENGTH + body.size() + 4);
  ASSERT_EQ(sink.events[0].bytes.size(), sink.events[0].header.eventLength);
  // Byte-exact against what the builder put on the wire (marker dropped,
  // header+body+checksum kept).
  const auto &wire = transport.incoming;
  EXPECT_TRUE(std::equal(sink.events[0].bytes.begin(),
                         sink.events[0].bytes.end(),
                         wire.begin() + PACKET_HEADER_SIZE + 1));
}

TEST(EventStreamReaderTest, ReassemblesAnEventSpanningTwoSubPackets) {
  test::ScriptedStreamBuilder builder;
  // MAX_PAYLOAD_PER_PACKET + 1000 forces a split into two sub-packets.
  const std::size_t bodySize =
      MAX_PAYLOAD_PER_PACKET + 1000 - 1 - EVENT_HEADER_LENGTH;
  std::vector<std::uint8_t> body(bodySize);
  for (std::size_t i = 0; i < body.size(); ++i)
    body[i] = static_cast<std::uint8_t>(i);
  const std::uint32_t eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + bodySize);
  builder.PushEvent(
      33, body, 0,
      static_cast<std::uint32_t>(StartPosition().position + eventLength));
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(result.largestEventSubPackets, 2u);
  ASSERT_EQ(sink.events[0].bytes.size(), EVENT_HEADER_LENGTH + body.size());
  EXPECT_TRUE(std::equal(body.begin(), body.end(),
                         sink.events[0].bytes.begin() + EVENT_HEADER_LENGTH));
}

TEST(
    EventStreamReaderTest,
    ConsumesTheMandatoryEmptyTerminatorAtExactlyMaxPayloadAndReadsTheNextEvent) {
  test::ScriptedStreamBuilder builder;
  // Packet payload exactly MAX_PAYLOAD_PER_PACKET: still needs an empty
  // terminator sub-packet after it.
  const std::size_t bodySize = MAX_PAYLOAD_PER_PACKET - 1 - EVENT_HEADER_LENGTH;
  const std::vector<std::uint8_t> firstBody(bodySize, 0x11);
  const std::uint32_t firstEventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + bodySize);
  const std::uint32_t firstNextPosition =
      static_cast<std::uint32_t>(StartPosition().position + firstEventLength);
  builder.PushEvent(33, firstBody, 0, firstNextPosition);
  const std::vector<std::uint8_t> secondBody{0xAA, 0xBB, 0xCC};
  const std::uint32_t secondEventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + secondBody.size());
  builder.PushEvent(33, secondBody, 0, firstNextPosition + secondEventLength);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(sink.events[0].header.eventLength,
            EVENT_HEADER_LENGTH + firstBody.size());
  ASSERT_EQ(sink.events[1].bytes.size(),
            EVENT_HEADER_LENGTH + secondBody.size());
  EXPECT_TRUE(std::equal(secondBody.begin(), secondBody.end(),
                         sink.events[1].bytes.begin() + EVENT_HEADER_LENGTH));
}

TEST(EventStreamReaderTest, RotateUpdatesFileNameAndPosition) {
  test::ScriptedStreamBuilder builder;
  builder.PushRotate(4, "binlog.000002", /*artificial=*/true, 4);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  StreamReaderOptions options;
  options.checksumLength = 4;
  EventStreamReader reader(transport, sink, StartPosition(), options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  EXPECT_EQ(result.lastPosition.fileName, "binlog.000002");
  EXPECT_EQ(result.lastPosition.position, 4u);
}

TEST(EventStreamReaderTest, FirstPositionIsWhereTheFirstEventBegan) {
  const std::vector<std::uint8_t> body(10, 0x11);
  const std::uint32_t length = EVENT_HEADER_LENGTH + 10 + 4;
  test::ScriptedStreamBuilder builder;
  builder.PushEvent(2, body, 4, 100 + length);
  builder.PushEvent(2, body, 4, 100 + 2 * length);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  StreamReaderOptions options;
  options.checksumLength = 4;
  EventStreamReader reader(transport, sink, StartPosition(), options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream) << result.message;
  EXPECT_EQ(result.firstPosition.fileName, "binlog.000001");
  EXPECT_EQ(result.firstPosition.position, 100u);
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(sink.events[0].positionBeforeEvent.position, 100u);
  EXPECT_EQ(sink.events[1].positionBeforeEvent.position, 100u + length);
  EXPECT_EQ(result.lastPosition.position, 100u + 2 * length);
}

TEST(EventStreamReaderTest, AFirstRotateLeavesTheFirstPositionBeforeIt) {
  test::ScriptedStreamBuilder builder;
  builder.PushRotate(4, "binlog.000002", /*artificial=*/true, 4);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  StreamReaderOptions options;
  options.checksumLength = 4;
  EventStreamReader reader(transport, sink, StartPosition(), options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream) << result.message;
  EXPECT_EQ(result.firstPosition.fileName, "binlog.000001");
  EXPECT_EQ(result.firstPosition.position, 100u);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(sink.events[0].positionBeforeEvent.fileName, "binlog.000001");
  EXPECT_EQ(result.lastPosition.fileName, "binlog.000002");
}

TEST(EventStreamReaderTest,
     AFirstEventCutShortLeavesTheStartAsTheFirstPosition) {
  test::ScriptedStreamBuilder builder;
  builder.PushRotate(4, "binlog.000002", /*artificial=*/true, 4);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  sink.stopAtEventBytesCall = 1;
  StreamReaderOptions options;
  options.checksumLength = 4;
  EventStreamReader reader(transport, sink, StartPosition(), options);
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::StoppedBySink) << result.message;
  EXPECT_EQ(result.firstPosition.fileName, "binlog.000001");
  EXPECT_EQ(result.firstPosition.position, 100u);
}

TEST(EventStreamReaderTest, HeartbeatV1AdvancesPositionWithinTheSameFile) {
  test::ScriptedStreamBuilder builder;
  builder.PushHeartbeatV1("binlog.000001", 0, /*nextPosition=*/500);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  EXPECT_EQ(result.lastPosition.position, 500u);
  EXPECT_EQ(result.heartbeats, 1u);
}

TEST(EventStreamReaderTest, HeartbeatExactlyAtTheCurrentPositionSucceeds) {
  // The boundary itself: heartbeat_queue_event's check is "position <
  // current" fails, so equal must succeed.
  test::ScriptedStreamBuilder builder;
  builder.PushHeartbeatV1(
      "binlog.000001", 0,
      /*nextPosition=*/100);  // StartPosition() is already at 100
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  EXPECT_EQ(result.lastPosition.position, 100u);
}

TEST(EventStreamReaderTest, HeartbeatV2UsesItsOwnPositionFieldOverTheHeader) {
  test::ScriptedStreamBuilder builder;
  builder.PushHeartbeatV2("binlog.000001", 700, 0, /*nextPosition=*/1);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.lastPosition.position, 700u);
}

TEST(EventStreamReaderTest, HeartbeatNamingADifferentFileIsAFailure) {
  test::ScriptedStreamBuilder builder;
  builder.PushHeartbeatV1("binlog.999999", 0, 500);

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::HeartbeatFailure);
}

TEST(EventStreamReaderTest, HeartbeatBehindTheCurrentPositionIsAFailure) {
  test::ScriptedStreamBuilder builder;
  builder.PushHeartbeatV1(
      "binlog.000001", 0,
      /*nextPosition=*/50);  // StartPosition() is already at 100

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::HeartbeatFailure);
}

TEST(EventStreamReaderTest, ErrPacketEndsTheStreamWithItsCodeAndText) {
  test::ScriptedStreamBuilder builder;
  builder.PushErr(1236, "binlog truncated in the middle of an event");

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::SourceError);
  EXPECT_EQ(result.errorCode, 1236);
  EXPECT_NE(result.errorText.find("truncated"), std::string::npos);
}

TEST(EventStreamReaderTest, TimedOutMapsToTimeoutReason) {
  test::FakeTransport transport;
  transport.scriptedOutcomes = {ReadOutcome::TimedOut};
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();
  EXPECT_EQ(result.reason, StreamEndReason::Timeout);
}

TEST(EventStreamReaderTest, InterruptedMapsToStoppedReason) {
  test::FakeTransport transport;
  transport.scriptedOutcomes = {ReadOutcome::Interrupted};
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();
  EXPECT_EQ(result.reason, StreamEndReason::Stopped);
}

TEST(EventStreamReaderTest,
     SinkStoppingMidEventEndsWithStoppedBySinkAndNoMoreReads) {
  test::ScriptedStreamBuilder builder;
  // Large enough for several OnEventBytes() calls.
  const std::vector<std::uint8_t> body(500, 0x22);
  const std::uint32_t eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + body.size());
  builder.PushEvent(
      33, body, 0,
      static_cast<std::uint32_t>(StartPosition().position + eventLength));
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  transport.maxBytesPerRead = 50;  // several reads within this one event's body

  test::RecordingEventSink sink;
  sink.stopAtEventBytesCall =
      2;  // stop partway through the body, not on the header chunk (call 1)
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::StoppedBySink);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_FALSE(sink.events[0].ended);  // OnEventEnd() must not be called once a
                                       // call already returned false
  const unsigned readsAtStop = transport.readCallCount;
  // Sanity: stopped well before the whole script was consumed.
  EXPECT_LT(readsAtStop, transport.incoming.size());
}

TEST(EventStreamReaderTest, EventLengthShorterThanTheHeaderIsMalformed) {
  // Hand-built rather than through the builder: PushEvent()'s bookkeeping
  // cannot produce an event_length below EVENT_HEADER_LENGTH.
  std::vector<std::uint8_t> payload{0x00};
  payload.insert(payload.end(), 19, 0);
  payload[1 + 9] = 10;  // event_length = 10, less than EVENT_HEADER_LENGTH (19)

  std::uint8_t sequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(payload, sequenceId, transport.incoming);
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::MalformedStream);
  // next_position is zero too, which the position check would also refuse -
  // this message pins down which check fired.
  EXPECT_NE(result.message.find("shorter than the Common-Header"),
            std::string::npos)
      << result.message;
  EXPECT_TRUE(sink.events.empty());  // rejected before OnEventBegin()
}

TEST(EventStreamReaderTest, SequenceMismatchIsMalformed) {
  test::ScriptedStreamBuilder builder;
  builder.PushEvent(33, std::vector<std::uint8_t>{1, 2, 3}, 0);

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  transport.incoming[3] =
      7;  // corrupt the first sub-packet's sequence id (expected 0)

  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::MalformedStream);
  // This event's next_position is zero, which the position check would also
  // refuse - this message pins down which check fired.
  EXPECT_NE(result.message.find("sequence id"), std::string::npos)
      << result.message;
}

TEST(EventStreamReaderTest, SequenceIdsAreNotCheckedOverACompressedStream) {
  test::ScriptedStreamBuilder builder;
  const std::vector<std::uint8_t> body{1, 2, 3};
  // A next_position the reader agrees with, unlike
  // SequenceMismatchIsMalformed's: with the sequence check off, the
  // position check is what would fire next.
  builder.PushEvent(
      33, body, 0,
      static_cast<std::uint32_t>(StartPosition().position +
                                 EVENT_HEADER_LENGTH + body.size()));
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  // The same corruption SequenceMismatchIsMalformed relies on: under
  // compression the source rewrites these ids on flush, so the reader
  // must accept whatever they say.
  transport.incoming[3] = 7;

  StreamReaderOptions options;
  options.verifySequence = false;
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), options);
  const StreamResult result = reader.Run();

  EXPECT_NE(result.reason, StreamEndReason::MalformedStream) << result.message;
  EXPECT_EQ(sink.events.size(), 1u);
}

TEST(EventStreamReaderTest,
     EventClaimingMoreThanMaxEventLengthIsRejectedBeforeReadingAnyBody) {
  // Only marker + Common-Header are on the wire - if the reader tried to
  // read a body, it would report ConnectionClosed instead of MalformedStream.
  std::vector<std::uint8_t> payload{0x00};
  payload.insert(payload.end(), 4, 0);
  payload.push_back(2);  // an arbitrary type code
  payload.insert(payload.end(), 4, 0);
  const std::uint32_t hugeLength = 1500000000;  // > MAX_EVENT_LENGTH (1 GiB)
  for (int i = 0; i < 4; ++i)
    payload.push_back(static_cast<std::uint8_t>(hugeLength >> (8 * i)));
  payload.insert(payload.end(), 4, 0);
  payload.insert(payload.end(), 2, 0);

  std::uint8_t sequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(payload, sequenceId, transport.incoming);
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::MalformedStream);
  // next_position is zero too, which the position check would also refuse -
  // this message pins down which check fired.
  EXPECT_NE(result.message.find("larger than MAX_EVENT_LENGTH"),
            std::string::npos)
      << result.message;
  EXPECT_TRUE(sink.events.empty());
}

TEST(EventStreamReaderTest,
     SubPacketBytesPastTheDeclaredEventLengthAreMalformed) {
  // marker + a 23-byte event (event_length == EVENT_HEADER_LENGTH) + 5
  // extra bytes shaped like a well-formed EOF sub-packet (len=1, seq=1,
  // 0xFE) - must not be silently consumed as the next logical unit.
  std::vector<std::uint8_t> payload{0x00};
  payload.insert(payload.end(), 4, 0);
  payload.push_back(2);  // an arbitrary type code
  payload.insert(payload.end(), 4, 0);
  const std::uint32_t eventLength = EVENT_HEADER_LENGTH;
  for (int i = 0; i < 4; ++i)
    payload.push_back(static_cast<std::uint8_t>(eventLength >> (8 * i)));
  // Must agree with the running position, or the sync check fires first
  // instead of the check this test exercises.
  const std::uint32_t nextPosition =
      static_cast<std::uint32_t>(StartPosition().position + eventLength);
  for (int i = 0; i < 4; ++i)
    payload.push_back(static_cast<std::uint8_t>(nextPosition >> (8 * i)));
  payload.insert(payload.end(), 2, 0);
  for (std::uint8_t b : {0x01, 0x00, 0x00, 0x01, 0xFE})
    payload.push_back(b);  // not part of the event

  std::uint8_t sequenceId = 0;
  test::FakeTransport transport;
  PacketFramer::Encode(payload, sequenceId, transport.incoming);
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::MalformedStream);
  EXPECT_NE(result.message.find("past the declared event length"),
            std::string::npos)
      << result.message;
  EXPECT_EQ(sink.eventEndCalls,
            0u);  // must not be accepted as a complete event
}

TEST(EventStreamReaderTest,
     PositionAccumulatesAcrossThe4GiBBoundaryUsingEventLength) {
  // Keeps its own 64-bit position rather than trusting the header's
  // truncated 32-bit field, which cannot represent this case.
  const std::uint64_t fourGiB = std::uint64_t{1} << 32;
  const StreamPosition start{"binlog.000001", fourGiB - 100};

  test::ScriptedStreamBuilder builder;
  const std::vector<std::uint8_t> bodyA(300 - EVENT_HEADER_LENGTH,
                                        'a');  // whole event 300 bytes
  builder.PushEvent(
      33, bodyA, 0,
      /*nextPosition=*/200);  // low 32 bits of (fourGiB - 100 + 300)
  const std::vector<std::uint8_t> bodyB(50 - EVENT_HEADER_LENGTH,
                                        'b');  // whole event 50 bytes
  builder.PushEvent(
      33, bodyB, 0,
      /*nextPosition=*/250);  // low 32 bits of (fourGiB + 200 + 50)
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, start, {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(sink.events[0].positionBeforeEvent.position, fourGiB - 100);
  EXPECT_EQ(sink.events[1].positionBeforeEvent.position, fourGiB + 200);
  EXPECT_EQ(result.lastPosition.position, fourGiB + 250);
}

TEST(EventStreamReaderTest,
     EventWhoseNextPositionDisagreesWithTheAccumulatedPositionIsMalformed) {
  test::ScriptedStreamBuilder builder;
  const std::vector<std::uint8_t> body(50, 'x');
  builder.PushEvent(
      33, body, 0,
      /*nextPosition=*/999999);  // does not match StartPosition() + eventLength
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::MalformedStream);
  EXPECT_EQ(sink.eventBeginCalls,
            0u);  // rejected before the sink ever hears about this event
  EXPECT_TRUE(sink.events.empty());
  const std::uint32_t eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + body.size());
  EXPECT_NE(result.message.find(std::to_string(999999)), std::string::npos);
  EXPECT_NE(result.message.find(
                std::to_string(StartPosition().position + eventLength)),
            std::string::npos);
}

TEST(EventStreamReaderTest, HeartbeatV2AdvancesAndFailsAcrossThe4GiBBoundary) {
  const std::uint64_t fourGiB = std::uint64_t{1} << 32;
  const StreamPosition start{"binlog.000001", fourGiB - 100};

  test::ScriptedStreamBuilder builder;
  // v2's 64-bit body position wins here since the header's 32 bits can't
  // represent it.
  builder.PushHeartbeatV2("binlog.000001", fourGiB + 1000, 0,
                          /*nextPosition=*/1000);
  builder.PushHeartbeatV2("binlog.000001", fourGiB + 500, 0,
                          /*nextPosition=*/500);  // behind the first

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, start, {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::HeartbeatFailure);
  // Distinguishes "the first heartbeat succeeded then the second failed"
  // from "the first failed already" - both would report the same reason.
  EXPECT_EQ(result.heartbeats, 1u);
  EXPECT_EQ(result.lastPosition.position, fourGiB + 1000);
}

TEST(EventStreamReaderTest,
     EventEndingExactlyOnA4GiBBoundaryCarriesZeroAndStillAdvances) {
  // A writer truncates log_pos to 32 bits - an ordinary event landing
  // exactly on this boundary legitimately carries 0, and must still advance.
  const std::uint64_t fourGiB = std::uint64_t{1} << 32;
  const StreamPosition start{"binlog.000001", fourGiB - 300};

  test::ScriptedStreamBuilder builder;
  const std::vector<std::uint8_t> bodyA(
      300 - EVENT_HEADER_LENGTH,
      'a');  // whole event 300 bytes: fourGiB-300+300 == fourGiB
  builder.PushEvent(33, bodyA, 0, /*nextPosition=*/0);
  const std::vector<std::uint8_t> bodyB(50 - EVENT_HEADER_LENGTH, 'b');
  builder.PushEvent(33, bodyB, 0, /*nextPosition=*/50);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, start, {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(sink.events[1].positionBeforeEvent.position, fourGiB);
  EXPECT_EQ(result.lastPosition.position, fourGiB + 50);
}

TEST(EventStreamReaderTest,
     FormatDescriptionEventWithZeroNextPositionDoesNotAdvanceTheCounter) {
  // Only an FDE gets the zero-log_pos exemption - any other event still
  // advances (previous test).
  test::ScriptedStreamBuilder builder;
  const std::vector<std::uint8_t> body(10, 'x');
  builder.PushEvent(15 /* FORMAT_DESCRIPTION_EVENT */, body, 0,
                    /*nextPosition=*/0);
  builder.PushEof();

  test::FakeTransport transport;
  transport.incoming = builder.Bytes();
  test::RecordingEventSink sink;
  EventStreamReader reader(transport, sink, StartPosition(), {});
  const StreamResult result = reader.Run();

  EXPECT_EQ(result.reason, StreamEndReason::EndOfStream);
  ASSERT_EQ(sink.events.size(),
            1u);  // the FDE was actually delivered, not an empty stream
  EXPECT_EQ(result.lastPosition.position, StartPosition().position);
}

}  // namespace
}  // namespace binlog_streamer
