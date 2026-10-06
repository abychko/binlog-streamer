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

#include "protocol/cPacketFramer.hpp"

#include <string>

namespace binlog_streamer {
namespace {
constexpr std::size_t READ_GROWTH_SIZE = 8192;
constexpr std::size_t WRITE_GATHER_SIZE = 64 * 1024;
constexpr std::size_t WRITE_BUFFER_KEEP = 1024 * 1024;
}  // namespace

bool PacketChannel::ReadPacket(
    std::vector<std::uint8_t> &payload, std::string &error,
    std::optional<std::chrono::milliseconds> idleTimeout) {
  // idleTimeout applies only when no part of the next packet is already
  // buffered.
  bool waitingForNewPacket = m_readOffset == m_readBuffer.size();
  for (;;) {
    const std::span<const std::uint8_t> pending(
        m_readBuffer.data() + m_readOffset, m_readBuffer.size() - m_readOffset);
    const PacketDecodeResult measured =
        PacketFramer::Measure(pending, m_sequenceId, VerifySequence());
    if (measured.status == PacketDecodeStatus::Complete) {
      const PacketDecodeResult decoded = PacketFramer::Decode(
          pending, m_sequenceId, payload, VerifySequence());
      m_readOffset += decoded.bytesConsumed;
      if (m_readOffset == m_readBuffer.size()) {
        m_readBuffer.clear();
        m_readOffset = 0;
      }
      error.clear();
      return true;
    }
    if (measured.status == PacketDecodeStatus::SequenceMismatch) {
      error = "protocol desync: unexpected packet sequence id from " +
              std::string(m_options.peerName);
      return false;
    }
    if (pending.size() + measured.bytesNeeded > m_options.maxPacketSize) {
      error = "packet from " + std::string(m_options.peerName) +
              " larger than the " + std::to_string(m_options.maxPacketSize) +
              "-byte limit";
      return false;
    }
    const std::chrono::milliseconds readTimeout =
        (waitingForNewPacket && idleTimeout.has_value())
            ? *idleTimeout
            : m_options.readTimeout;
    if (!FillBuffer(readTimeout, error)) return false;
    waitingForNewPacket = false;
  }
}

std::vector<std::uint8_t> PacketChannel::TakeUnread() {
  std::vector<std::uint8_t> unread(
      m_readBuffer.begin() + static_cast<std::ptrdiff_t>(m_readOffset),
      m_readBuffer.end());
  m_readBuffer.clear();
  m_readOffset = 0;
  return unread;
}

bool PacketChannel::FillBuffer(std::chrono::milliseconds readTimeout,
                               std::string &error) {
  const std::size_t previousSize = m_readBuffer.size();
  m_readBuffer.resize(previousSize + READ_GROWTH_SIZE);
  std::size_t bytesRead = 0;
  const ReadOutcome outcome = m_transport.Read(
      std::span<std::uint8_t>(m_readBuffer.data() + previousSize,
                              READ_GROWTH_SIZE),
      bytesRead, readTimeout, error);
  m_readBuffer.resize(previousSize + bytesRead);
  switch (outcome) {
    case ReadOutcome::Data:
      return true;
    case ReadOutcome::TimedOut:
      error = "timed out waiting for a response from the " +
              std::string(m_options.peerName);
      return false;
    case ReadOutcome::Interrupted:
      m_interrupted = true;
      error = "read interrupted";
      return false;
    case ReadOutcome::Closed:
      error = "connection closed by " + std::string(m_options.peerName);
      return false;
    case ReadOutcome::Failed:
      return false;
  }
  return false;  // unreachable - keeps -Wall/-Wextra quiet
}

bool PacketChannel::WritePacket(std::span<const std::uint8_t> payload,
                                std::string &error) {
  PacketFramer::Encode(payload, m_sequenceId, m_writeBuffer);
  return Flush(error);
}

bool PacketChannel::QueuePacket(std::span<const std::uint8_t> payload,
                                std::string &error) {
  return QueuePacket({}, payload, error);
}

bool PacketChannel::QueuePacket(std::span<const std::uint8_t> head,
                                std::span<const std::uint8_t> body,
                                std::string &error) {
  PacketFramer::Encode(head, body, m_sequenceId, m_writeBuffer);
  return m_writeBuffer.size() < WRITE_GATHER_SIZE || Flush(error);
}

bool PacketChannel::Flush(std::string &error) {
  if (m_writeBuffer.empty()) return true;
  const bool written =
      m_transport.Write(m_writeBuffer, m_options.writeTimeout, error);
  m_writeBuffer.clear();
  if (m_writeBuffer.capacity() > WRITE_BUFFER_KEEP)
    m_writeBuffer.shrink_to_fit();
  return written;
}

}  // namespace binlog_streamer
