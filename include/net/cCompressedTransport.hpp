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
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "net/cByteBuffer.hpp"
#include "net/eReadOutcome.hpp"
#include "net/iTransport.hpp"
#include "protocol/eCompressionAlgorithm.hpp"

// Keeps <zstd.h> out of this header, so consumers need no include path
// for the submodule.
struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace binlog_streamer {

// Header of a compressed packet: 3-byte compressed length, 1-byte
// sequence id of its own, 3-byte length before compression.
inline constexpr std::size_t COMPRESSED_HEADER_SIZE = 7;
// Below this, MySQL stores the payload as is and writes 0 as the length
// before compression (MIN_COMPRESS_LENGTH, include/my_sys.h).
inline constexpr std::size_t MIN_COMPRESS_LENGTH = 50;
// Both lengths in the header are three bytes wide.
inline constexpr std::size_t MAX_COMPRESSED_FRAME_PAYLOAD = 0xFFFFFF;

// Wraps a transport in MySQL's compressed protocol: the byte stream
// above it is the ordinary packet stream, the one below it is a series
// of compressed frames. Passes bytes through untouched until Enable(),
// because the wire turns compressed only after the final authentication
// OK.
//
// Frames and the packets inside them are not aligned: one frame may
// carry several packets, and a packet may span frames. The sequence ids
// of those inner packets are neither checked nor monotonic under
// compression - only the frame counter here is (sql-common/net_serv.cc).
class CompressedTransport final : public Transport {
 public:
  // continuationTimeout applies to every read that finishes a frame
  // already begun; the read that starts one uses the caller's timeout.
  CompressedTransport(Transport &inner,
                      std::chrono::milliseconds continuationTimeout)
      : m_inner(inner), m_continuationTimeout(continuationTimeout) {}
  ~CompressedTransport() override;

  CompressedTransport(const CompressedTransport &) = delete;
  CompressedTransport &operator=(const CompressedTransport &) = delete;

  void Enable(CompressionAlgorithm algorithm, int level);
  bool Enabled() const { return m_algorithm != CompressionAlgorithm::None; }

  // The frame counter resets with the packet sequence id, on the same
  // command boundary.
  void ResetSequence() { m_frameSequenceId = 0; }

  bool Connect(const std::string &host, std::uint16_t port,
               std::chrono::milliseconds timeout, std::string &error) override;

  ReadOutcome Read(std::span<std::uint8_t> buffer, std::size_t &bytesRead,
                   std::chrono::milliseconds timeout,
                   std::string &error) override;

  bool Write(std::span<const std::uint8_t> data,
             std::chrono::milliseconds timeout, std::string &error) override;

  void Close() override;

 private:
  // Data once a whole frame has been decoded into m_plain; anything else
  // leaves the partly-read frame buffered for the next call.
  ReadOutcome ReadFrame(std::chrono::milliseconds timeout, std::string &error);
  ReadOutcome FillFrom(std::uint8_t *destination, std::size_t size,
                       std::size_t &filled, std::chrono::milliseconds timeout,
                       std::string &error);
  bool WriteFrame(std::span<const std::uint8_t> chunk,
                  std::chrono::milliseconds timeout, std::string &error);
  void DiscardFrame();

  Transport &m_inner;
  std::chrono::milliseconds m_continuationTimeout;

  CompressionAlgorithm m_algorithm = CompressionAlgorithm::None;
  int m_level = DEFAULT_ZSTD_COMPRESSION_LEVEL;
  std::uint8_t m_frameSequenceId = 0;

  ZSTD_CCtx_s *m_compressContext = nullptr;
  ZSTD_DCtx_s *m_decompressContext = nullptr;

  std::uint8_t m_header[COMPRESSED_HEADER_SIZE] = {};
  std::size_t m_headerFilled = 0;
  ByteBuffer m_body;
  std::size_t m_bodyFilled = 0;
  bool m_bodySized = false;
  // Set by the first read that delivers any byte of the current frame;
  // later reads of that frame no longer wait on the caller's timeout.
  bool m_frameStarted = false;

  ByteBuffer m_plain;
  std::size_t m_plainOffset = 0;
  ByteBuffer m_frameOut;
};

}  // namespace binlog_streamer
