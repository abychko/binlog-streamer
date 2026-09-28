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

#include "cDumpSender.hpp"
#include "status/cStreamProgress.hpp"

#include "binlog/cCrc32.hpp"
#include "binlog/cEventHeaderCodec.hpp"
#include "binlog/cFormatDescriptionEventCodec.hpp"
#include "binlog/cGtidEventCodec.hpp"
#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "protocol/cEofPacketCodec.hpp"
#include "protocol/cErrPacketCodec.hpp"
#include "protocol/cLengthEncodedInteger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>

namespace binlog_streamer {

namespace {

constexpr std::uint64_t FIRST_EVENT_OFFSET =
    4;  // right after the file's magic number
constexpr std::size_t FLAGS_OFFSET = 17;
// Format_description body: binlog version (2), server version (50), then
// the creation time.
constexpr std::size_t CREATED_OFFSET = EVENT_HEADER_LENGTH + 2 + 50;
constexpr std::chrono::milliseconds WAIT_STEP{1000};
constexpr std::size_t WINDOW_SIZE = 64 * 1024;
// An event buffer that grew past this for a large event is let go once the
// dump catches up.
constexpr std::size_t EVENT_BUFFER_KEEP = 1024 * 1024;
// The OK marker every event packet of a dump starts with.
constexpr std::uint8_t EVENT_PACKET_MARKER[] = {0x00};

// The events that decide whether what follows them is skipped: a GTID by
// whether the replica has it, the others by standing outside any
// transaction.
bool DecidesSkipping(std::uint8_t type) {
  switch (static_cast<EventType>(type)) {
    case EventType::Gtid:
    case EventType::GtidTagged:
    case EventType::AnonymousGtid:
    case EventType::Rotate:
    case EventType::FormatDescription:
    case EventType::PreviousGtids:
      return true;
    default:
      return false;
  }
}

void AppendLE(std::vector<std::uint8_t> &out, std::uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void ReleaseIfLarge(ByteBuffer &buffer) {
  if (buffer.capacity() > EVENT_BUFFER_KEEP) ByteBuffer().swap(buffer);
}

void WriteChecksum(std::span<std::uint8_t> event) {
  const std::size_t covered = event.size() - CHECKSUM_LENGTH;
  const std::uint32_t crc =
      Crc32::Compute(std::span<const std::uint8_t>(event.data(), covered));
  for (std::size_t i = 0; i < CHECKSUM_LENGTH; ++i)
    event[covered + i] = static_cast<std::uint8_t>(crc >> (8 * i));
}

}  // namespace

DumpSender::DumpSender(BinlogStorageReader &reader, PacketChannel &channel,
                       DumpSenderOptions options)
    : m_reader(reader), m_channel(channel), m_options(std::move(options)) {}

DumpSender::ReadStatus DumpSender::ReadHeader(
    const FileCursor &cursor, std::uint64_t offset,
    std::uint8_t (&headerBytes)[EVENT_HEADER_LENGTH], EventHeader &header,
    std::string &error) {
  error.clear();
  std::size_t got = 0;
  while (got < EVENT_HEADER_LENGTH) {
    const std::size_t count = ReadAhead(
        cursor, offset + got,
        std::span<std::uint8_t>(headerBytes + got, EVENT_HEADER_LENGTH - got),
        error);
    if (count == 0)
      return error.empty() ? ReadStatus::NothingYet : ReadStatus::Failed;
    got += count;
  }
  if (!EventHeaderCodec::Parse(
          std::span<const std::uint8_t, EVENT_HEADER_LENGTH>(headerBytes),
          header, error)) {
    return ReadStatus::Failed;
  }
  if (header.eventLength < EVENT_HEADER_LENGTH) {
    error = "stored event shorter than its own header";
    return ReadStatus::Failed;
  }
  return ReadStatus::Event;
}

DumpSender::ReadStatus DumpSender::ReadEvent(const FileCursor &cursor,
                                             std::uint64_t offset,
                                             ByteBuffer &event,
                                             std::string &error) {
  std::uint8_t headerBytes[EVENT_HEADER_LENGTH];
  EventHeader header;
  const ReadStatus status =
      ReadHeader(cursor, offset, headerBytes, header, error);
  if (status != ReadStatus::Event) return status;
  event.resize(header.eventLength);
  std::memcpy(event.data(), headerBytes, EVENT_HEADER_LENGTH);
  std::size_t have = EVENT_HEADER_LENGTH;
  while (have < event.size()) {
    const std::size_t count = ReadAhead(
        cursor, offset + have,
        std::span<std::uint8_t>(event.data() + have, event.size() - have),
        error);
    if (count == 0)
      return error.empty() ? ReadStatus::NothingYet : ReadStatus::Failed;
    have += count;
  }
  return ReadStatus::Event;
}

DumpSender::ReadStatus DumpSender::PeekEvent(const FileCursor &cursor,
                                             std::uint64_t offset,
                                             EventHeader &header,
                                             std::string &error) {
  std::uint8_t headerBytes[EVENT_HEADER_LENGTH];
  const ReadStatus status =
      ReadHeader(cursor, offset, headerBytes, header, error);
  if (status != ReadStatus::Event) return status;
  // The window, holding the header now, walks on to the event's last byte.
  // Reading past what may be read right now is an error, so it steps from
  // its own end rather than jumping there.
  const std::uint64_t last = offset + header.eventLength - 1;
  while (last >= m_windowOffset + m_windowSize) {
    std::uint8_t byte = 0;
    if (ReadAhead(cursor, m_windowOffset + m_windowSize,
                  std::span<std::uint8_t>(&byte, 1), error) == 0)
      return error.empty() ? ReadStatus::NothingYet : ReadStatus::Failed;
  }
  return ReadStatus::Event;
}

std::size_t DumpSender::ReadAhead(const FileCursor &cursor,
                                  std::uint64_t offset,
                                  std::span<std::uint8_t> out,
                                  std::string &error) {
  if (cursor.FileName() != m_windowFile || offset < m_windowOffset ||
      offset >= m_windowOffset + m_windowSize) {
    if (!m_window)
      m_window = std::make_unique_for_overwrite<std::uint8_t[]>(WINDOW_SIZE);
    m_windowSize = m_reader.Read(
        cursor, offset, std::span<std::uint8_t>(m_window.get(), WINDOW_SIZE),
        error);
    m_windowFile = cursor.FileName();
    m_windowOffset = offset;
    if (m_windowSize == 0) return 0;
  }
  const std::size_t from = static_cast<std::size_t>(offset - m_windowOffset);
  const std::size_t count = std::min(out.size(), m_windowSize - from);
  std::memcpy(out.data(), m_window.get() + from, count);
  return count;
}

bool DumpSender::SendEvent(std::span<const std::uint8_t> event,
                           std::string &error) {
  m_lastSent = std::chrono::steady_clock::now();
  // Events go out together until there is nothing more to read, when
  // Send() flushes them before it waits.
  return m_channel.QueuePacket(EVENT_PACKET_MARKER, event, error);
}

bool DumpSender::SendRotate(const std::string &fileName, std::string &error) {
  // Rotate_log_event as Binlog_sender::fake_rotate_event() builds it: no
  // timestamp, no position of its own, flagged artificial, and pointing
  // at the first event of the file.
  const std::size_t length = EVENT_HEADER_LENGTH + 8 + fileName.size() +
                             (m_eventChecksum ? CHECKSUM_LENGTH : 0);
  std::vector<std::uint8_t> event;
  event.reserve(length);
  AppendLE(event, 0, 4);
  event.push_back(static_cast<std::uint8_t>(EventType::Rotate));
  AppendLE(event, m_options.serverId, 4);
  AppendLE(event, length, 4);
  AppendLE(event, 0, 4);
  AppendLE(event, EVENT_FLAG_ARTIFICIAL, 2);
  AppendLE(event, FIRST_EVENT_OFFSET, 8);
  event.insert(event.end(), fileName.begin(), fileName.end());
  if (m_eventChecksum) {
    event.resize(length);
    WriteChecksum(event);
  }
  return SendEvent(event, error);
}

bool DumpSender::SendHeartbeat(const std::string &fileName,
                               std::uint64_t position, std::string &error) {
  // Binlog_sender::send_heartbeat_event(): no timestamp, no flags, the
  // header's position cut to 32 bits. The first form carries the file
  // name alone; the second adds the full position as tagged fields.
  std::vector<std::uint8_t> body;
  if (m_options.heartbeatV2) {
    LengthEncodedInteger::Encode(1, body);
    LengthEncodedInteger::Encode(fileName.size(), body);
    body.insert(body.end(), fileName.begin(), fileName.end());
    std::vector<std::uint8_t> encodedPosition;
    LengthEncodedInteger::Encode(position, encodedPosition);
    LengthEncodedInteger::Encode(2, body);
    LengthEncodedInteger::Encode(encodedPosition.size(), body);
    body.insert(body.end(), encodedPosition.begin(), encodedPosition.end());
    LengthEncodedInteger::Encode(0, body);
  } else {
    body.assign(fileName.begin(), fileName.end());
  }
  const std::size_t length = EVENT_HEADER_LENGTH + body.size() +
                             (m_eventChecksum ? CHECKSUM_LENGTH : 0);
  std::vector<std::uint8_t> event;
  event.reserve(length);
  AppendLE(event, 0, 4);
  event.push_back(static_cast<std::uint8_t>(
      m_options.heartbeatV2 ? EventType::HeartbeatV2 : EventType::Heartbeat));
  AppendLE(event, m_options.serverId, 4);
  AppendLE(event, length, 4);
  AppendLE(event, position & 0xFFFFFFFFULL, 4);
  AppendLE(event, 0, 2);
  event.insert(event.end(), body.begin(), body.end());
  if (m_eventChecksum) {
    event.resize(length);
    WriteChecksum(event);
  }
  m_heartbeatQueued = true;
  return SendEvent(event, error);
}

bool DumpSender::HeartbeatDue() const {
  return m_options.heartbeatPeriod.count() > 0 &&
         std::chrono::steady_clock::now() - m_lastSent >=
             m_options.heartbeatPeriod;
}

bool DumpSender::ReplicaHas(const GtidSet &replicaSet,
                            std::span<const std::uint8_t> gtidEvent,
                            std::size_t checksumLength) {
  GtidEvent gtid;
  std::string error;
  const std::span<const std::uint8_t> body(
      gtidEvent.data() + EVENT_HEADER_LENGTH,
      gtidEvent.size() - EVENT_HEADER_LENGTH);
  const bool tagged =
      gtidEvent[4] == static_cast<std::uint8_t>(EventType::GtidTagged);
  const bool parsed =
      tagged ? GtidEventCodec::ParseTagged(body, checksumLength, gtid, error)
             : GtidEventCodec::Parse(body, checksumLength, gtid, error);
  if (!parsed)
    return false;  // an event that cannot be read is sent, not dropped
  return replicaSet.Contains(GtidSource{gtid.uuid, gtid.tag}, gtid.gno);
}

void DumpSender::PrepareFormatDescription(std::span<std::uint8_t> event,
                                          bool zeroCreated) {
  // A source computes this event's checksum with the flag already clear
  // (Log_event_footer::event_checksum_test() masks it), so clearing it
  // needs no new checksum.
  event[FLAGS_OFFSET] = static_cast<std::uint8_t>(event[FLAGS_OFFSET] &
                                                  ~EVENT_FLAG_BINLOG_IN_USE);
  if (!zeroCreated || event.size() < CREATED_OFFSET + 4) return;
  for (std::size_t i = 0; i < 4; ++i) event[CREATED_OFFSET + i] = 0;

  FormatDescriptionEvent description;
  std::string error;
  const std::span<const std::uint8_t> body(event.data() + EVENT_HEADER_LENGTH,
                                           event.size() - EVENT_HEADER_LENGTH);
  if (FormatDescriptionEventCodec::Parse(body, description, error) &&
      description.checksumAlgorithm == "CRC32") {
    WriteChecksum(event);
  }
}

bool DumpSender::ShouldStop(DumpEnd &end) const {
  if (m_options.stopRequested != nullptr && m_options.stopRequested->load()) {
    end.kind = DumpEndKind::Stopped;
    end.message = "the relay is stopping";
    return true;
  }
  if (m_options.superseded && m_options.superseded->load()) {
    end.kind = DumpEndKind::Superseded;
    end.message = "the same replica started a newer dump";
    return true;
  }
  return false;
}

DumpEnd DumpSender::Fail(DumpEnd end, const std::string &message) {
  end.kind = DumpEndKind::StorageFailure;
  end.message = message;
  // ER_SOURCE_FATAL_ERROR_READING_BINLOG, as a source reports a file it
  // cannot read.
  ErrPacket err;
  err.errorCode = 1236;
  err.sqlState = "HY000";
  err.message = message;
  std::vector<std::uint8_t> packet;
  ErrPacketCodec::Encode(err, packet);
  std::string ignored;
  m_channel.WritePacket(packet, ignored);
  return end;
}

DumpEnd DumpSender::Run(std::unique_ptr<FileCursor> cursor,
                        const GtidSet &replicaSet) {
  DumpEnd end = Send(std::move(cursor), replicaSet);
  std::string error;
  m_channel.Flush(
      error);  // whatever the end, what was queued before it goes out
  return end;
}

DumpEnd DumpSender::Send(std::unique_ptr<FileCursor> cursor,
                         const GtidSet &replicaSet) {
  DumpEnd end;
  m_lastSent = std::chrono::steady_clock::now();
  m_eventChecksum = m_options.checksum;
  std::string error;
  bool startFile = true;
  // When the dump caught up with something queued: the linger counts from
  // there.
  std::optional<std::chrono::steady_clock::time_point> lingerSince;
  for (;;) {
    const std::string fileName = cursor->FileName();
    if (!SendRotate(fileName, error)) {
      end.kind = DumpEndKind::ReplicaGone;
      end.message = "sending the rotate event for " + fileName + ": " + error;
      return end;
    }

    std::uint64_t offset = FIRST_EVENT_OFFSET;
    if (m_options.progress != nullptr)
      m_options.progress->SetFile(fileName, offset);
    ByteBuffer event;
    ByteBuffer lookahead;
    int idleRounds = 0;
    // Binlog_sender::send_events(): inside a transaction the replica already
    // has, every event is skipped until the next GTID says otherwise. A
    // heartbeat stands in for what was skipped, so the replica can resume
    // right.
    bool skipping = false;
    std::uint64_t heartbeatOwedAt = 0;
    std::size_t fileChecksumLength = 0;
    auto settleHeartbeat = [&]() {
      if (heartbeatOwedAt == 0) return true;
      const std::uint64_t position = heartbeatOwedAt;
      heartbeatOwedAt = 0;
      return SendHeartbeat(fileName, position, error);
    };
    // A skipped event is one the replica already has: it stands past it as
    // if it had been sent.
    auto passOver = [&](std::uint64_t length, std::uint32_t timestamp) {
      ++end.skipped;
      offset += length;
      if (m_options.progress != nullptr)
        m_options.progress->Advance(offset, timestamp);
      if (!HeartbeatDue()) {
        heartbeatOwedAt = offset;
        return true;
      }
      heartbeatOwedAt = 0;
      return SendHeartbeat(fileName, offset, error);
    };
    auto replicaGone = [&](const std::string &what) {
      end.kind = DumpEndKind::ReplicaGone;
      end.message = what + " of " + fileName + ": " + error;
      return end;
    };
    for (;;) {
      if (ShouldStop(end)) return end;

      ReadStatus status = ReadStatus::Event;
      if (!lookahead.empty()) {
        event.swap(lookahead);
        lookahead.clear();
      } else if (skipping) {
        // Inside a transaction the replica has, only an event that can end
        // the skip is read whole; the rest are passed over by their header.
        EventHeader header;
        status = PeekEvent(*cursor, offset, header, error);
        if (status == ReadStatus::Event && !DecidesSkipping(header.type)) {
          if (!passOver(header.eventLength, header.timestamp))
            return replicaGone("sending a heartbeat");
          continue;
        }
        if (status == ReadStatus::Event)
          status = ReadEvent(*cursor, offset, event, error);
      } else {
        status = ReadEvent(*cursor, offset, event, error);
      }
      if (status == ReadStatus::Failed)
        return Fail(end, "reading " + fileName + " at " +
                             std::to_string(offset) + ": " + error);

      if (status == ReadStatus::NothingYet) {
        std::unique_ptr<FileCursor> next;
        const NextFileOutcome outcome =
            m_reader.Next(*cursor, offset, next, error);
        if (outcome == NextFileOutcome::Failed)
          return Fail(end, "moving on from " + fileName + ": " + error);
        // Only at the live end of the history, one wait per linger rather
        // than one per event: what arrives meanwhile goes in the same
        // write. A closed file is followed without a pause, and a queued
        // heartbeat is not held back.
        if (outcome != NextFileOutcome::Found &&
            m_options.sendLinger.count() > 0 && !m_options.nonBlocking &&
            m_channel.HasQueued() && !m_heartbeatQueued) {
          const auto now = std::chrono::steady_clock::now();
          if (!lingerSince) lingerSince = now;
          if (now - *lingerSince < m_options.sendLinger) {
            std::this_thread::sleep_for(*lingerSince + m_options.sendLinger -
                                        now);
            continue;
          }
        }
        lingerSince.reset();
        if (!settleHeartbeat()) return replicaGone("sending a heartbeat");
        if (!m_channel.Flush(error)) return replicaGone("sending events");
        m_heartbeatQueued = false;
        ReleaseIfLarge(event);
        ReleaseIfLarge(lookahead);
        if (outcome == NextFileOutcome::Found) {
          cursor = std::move(next);
          break;
        }
        if (m_options.nonBlocking) {
          std::vector<std::uint8_t> eof;
          EofPacketCodec::Encode(EofPacket{}, eof);
          m_channel.WritePacket(eof, error);
          end.kind = DumpEndKind::EndOfHistory;
          end.message = "end of the stored history";
          return end;
        }
        if (m_options.progress != nullptr) m_options.progress->Idle(offset);
        const WaitOutcome waited = m_reader.WaitForNewEvents(
            PublishedPosition{fileName, offset}, WAIT_STEP,
            m_options.sendLinger.count() > 0 ? WaitStyle::Block
                                             : WaitStyle::PollFirst);
        // Published moved but nothing is readable and no next file
        // yet: this file is about to be closed.
        const bool stillNothing =
            waited == WaitOutcome::Advanced && ++idleRounds > 1;
        // Nothing to send for a heartbeat period: tell the replica
        // the connection is alive; no limit on how long this continues.
        if ((waited == WaitOutcome::TimedOut || stillNothing) &&
            HeartbeatDue() && !SendHeartbeat(fileName, offset, error)) {
          return replicaGone("sending a heartbeat");
        }
        if (stillNothing)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      idleRounds = 0;

      const std::uint64_t eventLength = event.size();
      if (event[4] == static_cast<std::uint8_t>(EventType::FormatDescription)) {
        // Whether the replica already holds part of this file shows
        // in the following Previous_gtids event: it does when it
        // has anything beyond them.
        bool zeroCreated = false;
        if (startFile &&
            ReadEvent(*cursor, offset + eventLength, lookahead, error) ==
                ReadStatus::Event &&
            lookahead[4] ==
                static_cast<std::uint8_t>(EventType::PreviousGtids)) {
          FormatDescriptionEvent description;
          std::string parseError;
          const std::span<const std::uint8_t> fdeBody(
              event.data() + EVENT_HEADER_LENGTH,
              event.size() - EVENT_HEADER_LENGTH);
          const bool crc = FormatDescriptionEventCodec::Parse(
                               fdeBody, description, parseError) &&
                           description.checksumAlgorithm == "CRC32";
          const std::size_t trailer = crc ? CHECKSUM_LENGTH : 0;
          GtidSet previous;
          if (lookahead.size() >= EVENT_HEADER_LENGTH + trailer &&
              previous.AddFromEncoding(
                  std::span<const std::uint8_t>(
                      lookahead.data() + EVENT_HEADER_LENGTH,
                      lookahead.size() - EVENT_HEADER_LENGTH - trailer),
                  parseError)) {
            zeroCreated = !replicaSet.IsSubsetOf(previous);
          }
        } else {
          lookahead.clear();
        }
        PrepareFormatDescription(event, zeroCreated);
        FormatDescriptionEvent sent;
        std::string parseError;
        const std::span<const std::uint8_t> body(
            event.data() + EVENT_HEADER_LENGTH,
            event.size() - EVENT_HEADER_LENGTH);
        fileChecksumLength =
            FormatDescriptionEventCodec::Parse(body, sent, parseError) &&
                    sent.checksumAlgorithm == "CRC32"
                ? CHECKSUM_LENGTH
                : 0;
        // From here on the relay's own events carry this file's checksum
        // algorithm, whatever the replica asked for - Binlog_sender takes it
        // from each file's Format_description; a client that asked for none
        // would otherwise cut four bytes off a file name.
        m_eventChecksum = fileChecksumLength != 0;
      }

      const auto type = static_cast<EventType>(event[4]);
      if (type == EventType::Gtid || type == EventType::GtidTagged) {
        skipping = ReplicaHas(replicaSet, event, fileChecksumLength);
      } else if (type == EventType::AnonymousGtid ||
                 type == EventType::Rotate ||
                 type == EventType::FormatDescription ||
                 type == EventType::PreviousGtids) {
        skipping = false;
      }
      const std::uint32_t timestamp =
          static_cast<std::uint32_t>(event[0]) |
          (static_cast<std::uint32_t>(event[1]) << 8) |
          (static_cast<std::uint32_t>(event[2]) << 16) |
          (static_cast<std::uint32_t>(event[3]) << 24);
      if (skipping) {
        if (!passOver(eventLength, timestamp))
          return replicaGone("sending a heartbeat");
        continue;
      }
      if (!settleHeartbeat()) return replicaGone("sending a heartbeat");

      if (!SendEvent(event, error)) {
        end.kind = DumpEndKind::ReplicaGone;
        end.message = "sending an event of " + fileName + ": " + error;
        return end;
      }
      ++end.events;
      offset += eventLength;
      if (m_options.progress != nullptr)
        m_options.progress->Advance(offset, timestamp);
    }
    startFile = false;
  }
}

}  // namespace binlog_streamer
