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

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "net/cByteBuffer.hpp"
#include "net/cCompressedTransport.hpp"
#include "net/iTransport.hpp"
#include "net/sPacketChannelOptions.hpp"

namespace binlog_streamer {

// Owns the single sequence-id counter a classic-protocol connection
// shares between reads and writes.
class PacketChannel {
 public:
  PacketChannel(Transport &transport, PacketChannelOptions options)
      : m_transport(transport), m_options(options) {}

  // Same channel over a connection that may turn compressed: the frame
  // counter resets together with the packet sequence id, and the ids of
  // packets arriving inside a frame stop being checked once it does.
  PacketChannel(CompressedTransport &transport, PacketChannelOptions options)
      : m_transport(transport), m_options(options), m_compressed(&transport) {}

  // Call before writing a command packet (COM_QUERY, COM_REGISTER_SLAVE,
  // COM_QUIT, ...): the wire protocol resets the sequence id to 0 there
  // (mysys/net_serv.cc).
  void ResetSequence() {
    m_sequenceId = 0;
    if (m_compressed != nullptr) m_compressed->ResetSequence();
  }

  // False covers transport failure, an oversized packet, a sequence-id
  // mismatch, or an interrupted read - check WasInterrupted() to tell
  // those apart.
  bool ReadPacket(
      std::vector<std::uint8_t> &payload, std::string &error,
      std::optional<std::chrono::milliseconds> idleTimeout = std::nullopt);

  bool WritePacket(std::span<const std::uint8_t> payload, std::string &error);

  // Auto-flushes before the next WritePacket() so packet order stays
  // correct; also flushes once enough has queued or on Flush().
  bool QueuePacket(std::span<const std::uint8_t> payload, std::string &error);
  // A packet whose payload is head followed by body.
  bool QueuePacket(std::span<const std::uint8_t> head,
                   std::span<const std::uint8_t> body, std::string &error);

  bool Flush(std::string &error);
  bool HasQueued() const { return !m_writeBuffer.empty(); }

  // Sticky once true: a later successful Read() does not clear it.
  bool WasInterrupted() const { return m_interrupted; }

  // Exposed so a caller can continue the counter across calls that
  // don't ResetSequence() (e.g. the dump/event stream continuing
  // COM_BINLOG_DUMP_GTID's counter).
  std::uint8_t NextSequenceId() const { return m_sequenceId; }

  // Bytes read past the last packet ReadPacket() returned, handed over
  // and forgotten here: what a TLS handshake started right after that
  // packet (an SSL request) has to be fed before it reads the socket.
  std::vector<std::uint8_t> TakeUnread();

 private:
  bool FillBuffer(std::chrono::milliseconds readTimeout, std::string &error);

  bool VerifySequence() const {
    return m_compressed == nullptr || !m_compressed->Enabled();
  }

  Transport &m_transport;
  PacketChannelOptions m_options;
  CompressedTransport *m_compressed = nullptr;
  std::uint8_t m_sequenceId = 0;
  ByteBuffer m_readBuffer;
  std::size_t m_readOffset = 0;  // bytes at the front of m_readBuffer already
                                 // consumed by a completed ReadPacket()
  bool m_interrupted = false;
  std::vector<std::uint8_t> m_writeBuffer;
};

}  // namespace binlog_streamer
