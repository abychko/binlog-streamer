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

class PacketChannel {
 public:
  PacketChannel(Transport &transport, PacketChannelOptions options)
      : m_transport(transport), m_options(options) {}

  PacketChannel(CompressedTransport &transport, PacketChannelOptions options)
      : m_transport(transport), m_options(options), m_compressed(&transport) {}

  // Call before writing a command packet: the protocol resets the sequence id
  // to 0 there.
  void ResetSequence() {
    m_sequenceId = 0;
    if (m_compressed != nullptr) m_compressed->ResetSequence();
  }

  bool ReadPacket(
      std::vector<std::uint8_t> &payload, std::string &error,
      std::optional<std::chrono::milliseconds> idleTimeout = std::nullopt);

  bool WritePacket(std::span<const std::uint8_t> payload, std::string &error);

  bool QueuePacket(std::span<const std::uint8_t> payload, std::string &error);
  bool QueuePacket(std::span<const std::uint8_t> head,
                   std::span<const std::uint8_t> body, std::string &error);

  bool Flush(std::string &error);
  bool HasQueued() const { return !m_writeBuffer.empty(); }

  bool WasInterrupted() const { return m_interrupted; }

  std::uint8_t NextSequenceId() const { return m_sequenceId; }

  // Bytes read past the last packet, handed over for a TLS handshake that
  // starts right after it.
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
  std::size_t m_readOffset = 0;
  bool m_interrupted = false;
  std::vector<std::uint8_t> m_writeBuffer;
};

}  // namespace binlog_streamer
