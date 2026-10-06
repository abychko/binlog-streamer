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
#include "status/cStreamProgress.hpp"

#include "binlog/cEventHeaderCodec.hpp"
#include "binlog/cRotateEventCodec.hpp"
#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "protocol/cErrPacketCodec.hpp"
#include "protocol/hProtocolLimits.hpp"
#include "receiver/cHeartbeatEventCodec.hpp"
#include "receiver/hSessionDefaults.hpp"

#include <algorithm>
#include <optional>

namespace binlog_streamer {
namespace {

constexpr std::uint64_t MAX_INTERNALLY_INSPECTED_EVENT_LENGTH =
    MAX_COMMAND_RESPONSE_SIZE;

bool NeedsInternalInspection(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(EventType::Rotate) ||
         type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
         type == static_cast<std::uint8_t>(EventType::HeartbeatV2);
}

}  // namespace

EventStreamReader::EventStreamReader(Transport &transport, EventSink &sink,
                                     StreamPosition startPosition,
                                     StreamReaderOptions options)
    : m_transport(transport),
      m_sink(sink),
      m_options(options),
      m_buffer(options.bufferSize),
      m_sequenceId(options.sequenceId),
      m_position(std::move(startPosition)) {
  if (m_options.progress != nullptr)
    m_options.progress->SetFile(m_position.fileName, m_position.position);
}

void EventStreamReader::Compact() {
  if (m_dataStart == 0) return;
  std::copy(m_buffer.begin() + static_cast<std::ptrdiff_t>(m_dataStart),
            m_buffer.begin() + static_cast<std::ptrdiff_t>(m_dataEnd),
            m_buffer.begin());
  m_dataEnd -= m_dataStart;
  m_dataStart = 0;
}

EventStreamReader::FillOutcome EventStreamReader::FillMore(std::string &error) {
  if (m_dataEnd == m_buffer.size()) Compact();
  if (m_dataEnd == m_buffer.size()) {
    error =
        "event stream read buffer full without a header/body boundary to "
        "consume";
    return FillOutcome::Failed;
  }
  std::size_t bytesRead = 0;
  const ReadOutcome outcome =
      m_transport.Read(std::span<std::uint8_t>(m_buffer.data() + m_dataEnd,
                                               m_buffer.size() - m_dataEnd),
                       bytesRead, m_options.readTimeout, error);
  switch (outcome) {
    case ReadOutcome::Data:
      m_dataEnd += bytesRead;
      return FillOutcome::Ready;
    case ReadOutcome::TimedOut:
      return FillOutcome::TimedOut;
    case ReadOutcome::Interrupted:
      return FillOutcome::Interrupted;
    case ReadOutcome::Closed:
      return FillOutcome::Closed;
    case ReadOutcome::Failed:
      return FillOutcome::Failed;
  }
  return FillOutcome::Failed;
}

EventStreamReader::FillOutcome EventStreamReader::EnsureBytes(
    std::size_t count, std::string &error) {
  while (m_dataEnd - m_dataStart < count) {
    const FillOutcome outcome = FillMore(error);
    if (outcome != FillOutcome::Ready) return outcome;
  }
  return FillOutcome::Ready;
}

StreamResult EventStreamReader::MakeResult(StreamEndReason reason,
                                           std::string message) const {
  StreamResult result;
  result.reason = reason;
  result.message = std::move(message);
  result.firstPosition = m_hasFirstPosition ? m_firstPosition : m_position;
  result.lastPosition = m_position;
  result.events = m_events;
  result.heartbeats = m_heartbeats;
  result.artificial = m_artificial;
  result.bytes = m_bytes;
  result.largestEventLength = m_largestEventLength;
  result.largestEventSubPackets = m_largestEventSubPackets;
  return result;
}

StreamResult EventStreamReader::TerminalFromFill(
    FillOutcome outcome, const std::string &error) const {
  switch (outcome) {
    case FillOutcome::TimedOut:
      return MakeResult(StreamEndReason::Timeout,
                        "timed out waiting for a response from the source");
    case FillOutcome::Interrupted:
      return MakeResult(StreamEndReason::Stopped, "read interrupted");
    case FillOutcome::Closed:
      return MakeResult(StreamEndReason::ConnectionClosed,
                        "connection closed by source");
    case FillOutcome::Failed:
    case FillOutcome::Ready:
      return MakeResult(StreamEndReason::ConnectionClosed, error);
  }
  return MakeResult(StreamEndReason::ConnectionClosed, error);
}

bool EventStreamReader::ReadSubPacketHeader(std::size_t &subPacketLength,
                                            StreamResult &terminalResult) {
  std::string error;
  const FillOutcome outcome = EnsureBytes(PACKET_HEADER_SIZE, error);
  if (outcome != FillOutcome::Ready) {
    terminalResult = TerminalFromFill(outcome, error);
    return false;
  }
  const std::uint8_t *header = &m_buffer[m_dataStart];
  subPacketLength = static_cast<std::size_t>(header[0]) |
                    (static_cast<std::size_t>(header[1]) << 8) |
                    (static_cast<std::size_t>(header[2]) << 16);
  const std::uint8_t sequenceId = header[3];
  if (m_options.verifySequence && sequenceId != m_sequenceId) {
    terminalResult = MakeResult(
        StreamEndReason::MalformedStream,
        "protocol desync: unexpected packet sequence id from source");
    return false;
  }
  m_dataStart += PACKET_HEADER_SIZE;
  ++m_sequenceId;
  return true;
}

void EventStreamReader::ConsumeErrPacket(
    std::size_t firstSubPacketPayloadLength, bool firstSubPacketWasFull,
    StreamResult &terminalResult) {
  // Run() already consumed the 0xFF marker and ErrPacketCodec::Parse() expects
  // it, so it is re-added.
  std::vector<std::uint8_t> payload{0xFF};
  std::size_t remainingInSubPacket = firstSubPacketPayloadLength;
  bool subPacketWasFull = firstSubPacketWasFull;
  for (;;) {
    while (remainingInSubPacket > 0) {
      std::string error;
      const FillOutcome outcome = EnsureBytes(1, error);
      if (outcome != FillOutcome::Ready) {
        terminalResult = TerminalFromFill(outcome, error);
        return;
      }
      const std::size_t take =
          std::min({m_dataEnd - m_dataStart, remainingInSubPacket});
      if (payload.size() + take > MAX_COMMAND_RESPONSE_SIZE) {
        terminalResult =
            MakeResult(StreamEndReason::MalformedStream,
                       "ERR packet larger than MAX_COMMAND_RESPONSE_SIZE");
        return;
      }
      payload.insert(
          payload.end(),
          m_buffer.begin() + static_cast<std::ptrdiff_t>(m_dataStart),
          m_buffer.begin() + static_cast<std::ptrdiff_t>(m_dataStart + take));
      m_dataStart += take;
      remainingInSubPacket -= take;
    }
    if (!subPacketWasFull) break;
    if (!ReadSubPacketHeader(remainingInSubPacket, terminalResult)) return;
    subPacketWasFull = (remainingInSubPacket == MAX_PAYLOAD_PER_PACKET);
  }
  ErrPacket err;
  std::string parseError;
  if (!ErrPacketCodec::Parse(payload, err, parseError)) {
    terminalResult = MakeResult(StreamEndReason::MalformedStream,
                                "malformed ERR packet: " + parseError);
    return;
  }
  terminalResult = MakeResult(StreamEndReason::SourceError, err.message);
  terminalResult.errorCode = err.errorCode;
  terminalResult.errorText = err.message;
}

bool EventStreamReader::ConsumeEvent(std::size_t firstSubPacketPayloadLength,
                                     bool firstSubPacketWasFull,
                                     StreamResult &terminalResult) {
  std::size_t remainingInSubPacket = firstSubPacketPayloadLength;
  bool subPacketWasFull = firstSubPacketWasFull;

  // EnsureBytes() ignores sub-packet boundaries, so a header split across
  // sub-packets is still caught below as malformed.
  std::string error;
  FillOutcome outcome = EnsureBytes(EVENT_HEADER_LENGTH, error);
  if (outcome != FillOutcome::Ready) {
    terminalResult = TerminalFromFill(outcome, error);
    return false;
  }
  EventHeader header;
  std::string headerError;
  EventHeaderCodec::Parse(std::span<const std::uint8_t, EVENT_HEADER_LENGTH>(
                              &m_buffer[m_dataStart], EVENT_HEADER_LENGTH),
                          header, headerError);
  if (header.eventLength < EVENT_HEADER_LENGTH) {
    terminalResult =
        MakeResult(StreamEndReason::MalformedStream,
                   "event length shorter than the Common-Header itself");
    return false;
  }
  if (header.eventLength > MAX_EVENT_LENGTH) {
    // Rejected before any body byte is read, as sql/rpl_binlog_sender.cc does.
    terminalResult = MakeResult(StreamEndReason::MalformedStream,
                                "event length larger than MAX_EVENT_LENGTH");
    return false;
  }
  if (NeedsInternalInspection(header.type) &&
      header.eventLength > MAX_INTERNALLY_INSPECTED_EVENT_LENGTH) {
    terminalResult =
        MakeResult(StreamEndReason::MalformedStream,
                   "ROTATE/heartbeat event larger than expected for its type");
    return false;
  }

  // ROTATE/heartbeats carry their own position; an FDE with log_pos == 0 is the
  // only other exemption (sql/rpl_replica.cc, queue_event()).
  const bool isRotate =
      header.type == static_cast<std::uint8_t>(EventType::Rotate);
  const bool isHeartbeat =
      header.type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
      header.type == static_cast<std::uint8_t>(EventType::HeartbeatV2);
  const bool isFdeWithZeroPos =
      header.type == static_cast<std::uint8_t>(EventType::FormatDescription) &&
      header.nextPosition == 0;

  // Unlike a real replica, refuse a source whose log_pos disagrees with the
  // accumulated position (compared as 32 bits) before the sink sees a byte.
  if (!isRotate && !isHeartbeat && !isFdeWithZeroPos) {
    const std::uint64_t expectedPosition =
        m_position.position + header.eventLength;
    if (static_cast<std::uint32_t>(expectedPosition) != header.nextPosition) {
      terminalResult = MakeResult(
          StreamEndReason::MalformedStream,
          "event header log_pos " + std::to_string(header.nextPosition) +
              " disagrees with the accumulated position " +
              std::to_string(expectedPosition) + " (low 32 bits) at " +
              m_position.fileName);
      return false;
    }
  }

  std::optional<StreamPosition> firstPosition;
  if (!m_hasFirstPosition) firstPosition = m_position;
  if (!m_sink.OnEventBegin(header, m_position)) {
    terminalResult = MakeResult(StreamEndReason::StoppedBySink,
                                "EventSink::OnEventBegin returned false");
    return false;
  }

  const bool inspectLocally = NeedsInternalInspection(header.type);
  std::vector<std::uint8_t> localCopy;
  if (inspectLocally) localCopy.reserve(header.eventLength);

  std::uint64_t totalRemaining = header.eventLength;
  std::size_t subPacketsUsed = 1;
  bool stoppedBySink = false;
  while (totalRemaining > 0) {
    if (remainingInSubPacket == 0) {
      if (!subPacketWasFull) {
        terminalResult = MakeResult(
            StreamEndReason::MalformedStream,
            "sub-packets ended before the declared event length was reached");
        return false;
      }
      if (!ReadSubPacketHeader(remainingInSubPacket, terminalResult))
        return false;
      subPacketWasFull = (remainingInSubPacket == MAX_PAYLOAD_PER_PACKET);
      ++subPacketsUsed;
      continue;
    }
    if (m_dataEnd == m_dataStart) {
      const FillOutcome fillOutcome = FillMore(error);
      if (fillOutcome != FillOutcome::Ready) {
        terminalResult = TerminalFromFill(fillOutcome, error);
        return false;
      }
      continue;
    }
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(
        std::min(m_dataEnd - m_dataStart, remainingInSubPacket),
        totalRemaining));
    const std::span<const std::uint8_t> chunk(&m_buffer[m_dataStart], take);
    if (inspectLocally)
      localCopy.insert(localCopy.end(), chunk.begin(), chunk.end());
    if (!m_sink.OnEventBytes(chunk)) {
      stoppedBySink = true;
      m_dataStart += take;
      remainingInSubPacket -= take;
      totalRemaining -= take;
      break;
    }
    m_dataStart += take;
    remainingInSubPacket -= take;
    totalRemaining -= take;
  }
  if (stoppedBySink) {
    terminalResult = MakeResult(StreamEndReason::StoppedBySink,
                                "EventSink::OnEventBytes returned false");
    return false;
  }
  if (remainingInSubPacket != 0) {
    // The source declared a longer sub-packet than eventLength accounts for;
    // accepting it would desync the stream.
    terminalResult =
        MakeResult(StreamEndReason::MalformedStream,
                   "sub-packet carries bytes past the declared event length");
    return false;
  }

  // A sub-packet of exactly MAX_PAYLOAD_PER_PACKET bytes is always followed by
  // a continuation, even an empty one.
  if (remainingInSubPacket == 0 && subPacketWasFull) {
    std::size_t terminatorLength = 0;
    if (!ReadSubPacketHeader(terminatorLength, terminalResult)) return false;
    ++subPacketsUsed;
    if (terminatorLength != 0) {
      terminalResult = MakeResult(
          StreamEndReason::MalformedStream,
          "unexpected non-empty sub-packet after the declared event length");
      return false;
    }
  }

  if (!m_sink.OnEventEnd()) {
    terminalResult = MakeResult(StreamEndReason::StoppedBySink,
                                "EventSink::OnEventEnd returned false");
    return false;
  }

  // Tracks a 64-bit running position instead of the header's truncated 32-bit
  // field; heartbeats are excluded because their check needs the position
  // before this event.

  const std::span<const std::uint8_t> eventBody =
      inspectLocally ? std::span<const std::uint8_t>(localCopy).subspan(
                           EVENT_HEADER_LENGTH)
                     : std::span<const std::uint8_t>();

  if (isRotate) {
    RotateEvent rotate;
    std::string rotateError;
    if (!RotateEventCodec::Parse(eventBody, m_options.checksumLength, rotate,
                                 rotateError)) {
      terminalResult = MakeResult(StreamEndReason::MalformedStream,
                                  "malformed ROTATE event: " + rotateError);
      return false;
    }
    m_position.fileName = rotate.fileName;
    m_position.position = rotate.position;
    if (m_options.progress != nullptr)
      m_options.progress->SetFile(rotate.fileName, rotate.position);
  } else if (!isHeartbeat && !isFdeWithZeroPos) {
    m_position.position += header.eventLength;
    if (m_options.progress != nullptr)
      m_options.progress->Advance(m_position.position, header.timestamp);
  }

  if (header.type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
      header.type == static_cast<std::uint8_t>(EventType::HeartbeatV2)) {
    HeartbeatEvent heartbeat;
    bool parsed = false;
    std::string heartbeatError;
    if (header.type == static_cast<std::uint8_t>(EventType::Heartbeat)) {
      HeartbeatEventCodec::ParseV1(eventBody, m_options.checksumLength,
                                   heartbeat);
      parsed = true;
    } else {
      parsed = HeartbeatEventCodec::ParseV2(eventBody, m_options.checksumLength,
                                            heartbeat, heartbeatError);
    }
    if (!parsed) {
      terminalResult =
          MakeResult(StreamEndReason::MalformedStream,
                     "malformed heartbeat event: " + heartbeatError);
      return false;
    }
    // v2's own position, or the header's if the body left it 0 or absent
    // (sql/rpl_replica.cc, heartbeat_queue_event()).
    const std::uint64_t heartbeatPosition =
        (heartbeat.position.has_value() && *heartbeat.position != 0)
            ? *heartbeat.position
            : header.nextPosition;
    if (heartbeat.fileName != m_position.fileName ||
        heartbeatPosition < m_position.position) {
      terminalResult = MakeResult(StreamEndReason::HeartbeatFailure,
                                  "heartbeat named " + heartbeat.fileName +
                                      ":" + std::to_string(heartbeatPosition) +
                                      ", behind the current position " +
                                      m_position.fileName + ":" +
                                      std::to_string(m_position.position));
      return false;
    }
    if (heartbeatPosition > m_position.position)
      m_position.position = heartbeatPosition;
    if (m_options.progress != nullptr)
      m_options.progress->Idle(m_position.position);
    ++m_heartbeats;
  }

  if (firstPosition) {
    m_firstPosition = std::move(*firstPosition);
    m_hasFirstPosition = true;
  }
  ++m_events;
  if ((header.flags & EVENT_FLAG_ARTIFICIAL) != 0) ++m_artificial;
  m_bytes += header.eventLength;
  m_largestEventLength =
      std::max<std::uint64_t>(m_largestEventLength, header.eventLength);
  m_largestEventSubPackets =
      std::max<std::uint64_t>(m_largestEventSubPackets, subPacketsUsed);
  return true;
}

StreamResult EventStreamReader::Run() {
  for (;;) {
    std::size_t subPacketLength = 0;
    StreamResult terminalResult;
    if (!ReadSubPacketHeader(subPacketLength, terminalResult))
      return terminalResult;
    if (subPacketLength == 0) {
      return MakeResult(StreamEndReason::MalformedStream,
                        "empty first sub-packet: no marker byte");
    }

    std::string error;
    const FillOutcome markerOutcome = EnsureBytes(1, error);
    if (markerOutcome != FillOutcome::Ready)
      return TerminalFromFill(markerOutcome, error);
    const std::uint8_t marker = m_buffer[m_dataStart];

    if (marker == 0xFE && subPacketLength < 9) {
      return MakeResult(StreamEndReason::EndOfStream,
                        "source ended the dump stream");
    }
    m_dataStart += 1;
    const bool subPacketWasFull = (subPacketLength == MAX_PAYLOAD_PER_PACKET);
    const std::size_t remainingAfterMarker = subPacketLength - 1;

    if (marker == 0xFF) {
      ConsumeErrPacket(remainingAfterMarker, subPacketWasFull, terminalResult);
      return terminalResult;
    }
    if (marker != 0x00) {
      return MakeResult(StreamEndReason::MalformedStream,
                        "unexpected packet marker byte from source: " +
                            std::to_string(static_cast<int>(marker)));
    }
    if (!ConsumeEvent(remainingAfterMarker, subPacketWasFull, terminalResult))
      return terminalResult;
  }
}

}  // namespace binlog_streamer
