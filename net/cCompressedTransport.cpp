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

#include <algorithm>
#include <cstring>

namespace binlog_streamer {
namespace {
constexpr std::size_t FRAME_BUFFER_KEEP = 1024 * 1024;

std::size_t Load3(const std::uint8_t *bytes) {
  return static_cast<std::size_t>(bytes[0]) |
         (static_cast<std::size_t>(bytes[1]) << 8) |
         (static_cast<std::size_t>(bytes[2]) << 16);
}

void Store3(std::uint8_t *bytes, std::size_t value) {
  bytes[0] = static_cast<std::uint8_t>(value);
  bytes[1] = static_cast<std::uint8_t>(value >> 8);
  bytes[2] = static_cast<std::uint8_t>(value >> 16);
}
}  // namespace

CompressedTransport::~CompressedTransport() {
  if (m_compressContext != nullptr) ZSTD_freeCCtx(m_compressContext);
  if (m_decompressContext != nullptr) ZSTD_freeDCtx(m_decompressContext);
}

void CompressedTransport::Enable(CompressionAlgorithm algorithm, int level) {
  m_algorithm = algorithm;
  m_level = algorithm == CompressionAlgorithm::Zlib
                ? std::clamp(level, MIN_ZLIB_COMPRESSION_LEVEL,
                             MAX_ZLIB_COMPRESSION_LEVEL)
                : std::clamp(level, MIN_ZSTD_COMPRESSION_LEVEL,
                             MAX_ZSTD_COMPRESSION_LEVEL);
}

bool CompressedTransport::Connect(const std::string &host, std::uint16_t port,
                                  std::chrono::milliseconds timeout,
                                  std::string &error) {
  m_algorithm = CompressionAlgorithm::None;
  m_frameSequenceId = 0;
  DiscardFrame();
  m_plain.clear();
  m_plainOffset = 0;
  return m_inner.Connect(host, port, timeout, error);
}

void CompressedTransport::Close() { m_inner.Close(); }

void CompressedTransport::DiscardFrame() {
  m_headerFilled = 0;
  m_bodyFilled = 0;
  m_bodySized = false;
  m_frameStarted = false;
  m_body.clear();
  if (m_body.capacity() > FRAME_BUFFER_KEEP) m_body.shrink_to_fit();
}

ReadOutcome CompressedTransport::Read(std::span<std::uint8_t> buffer,
                                      std::size_t &bytesRead,
                                      std::chrono::milliseconds timeout,
                                      std::string &error) {
  bytesRead = 0;
  if (!Enabled()) return m_inner.Read(buffer, bytesRead, timeout, error);
  if (buffer.empty()) return ReadOutcome::Data;

  // An empty frame delivers no bytes, and Data with none would look like end of
  // stream; read on.
  while (m_plainOffset == m_plain.size()) {
    const ReadOutcome outcome = ReadFrame(timeout, error);
    if (outcome != ReadOutcome::Data) return outcome;
  }

  const std::size_t count =
      std::min(buffer.size(), m_plain.size() - m_plainOffset);
  std::copy_n(m_plain.begin() + static_cast<std::ptrdiff_t>(m_plainOffset),
              count, buffer.begin());
  m_plainOffset += count;
  if (m_plainOffset == m_plain.size()) {
    m_plain.clear();
    m_plainOffset = 0;
    if (m_plain.capacity() > FRAME_BUFFER_KEEP) m_plain.shrink_to_fit();
  }
  bytesRead = count;
  return ReadOutcome::Data;
}

ReadOutcome CompressedTransport::FillFrom(std::uint8_t *destination,
                                          std::size_t size, std::size_t &filled,
                                          std::chrono::milliseconds timeout,
                                          std::string &error) {
  while (filled < size) {
    std::size_t bytesRead = 0;
    const ReadOutcome outcome = m_inner.Read(
        std::span<std::uint8_t>(destination + filled, size - filled), bytesRead,
        m_frameStarted ? m_continuationTimeout : timeout, error);
    if (outcome != ReadOutcome::Data) return outcome;
    filled += bytesRead;
    m_frameStarted = true;
  }
  return ReadOutcome::Data;
}

ReadOutcome CompressedTransport::ReadFrame(std::chrono::milliseconds timeout,
                                           std::string &error) {
  const ReadOutcome headerOutcome = FillFrom(m_header, COMPRESSED_HEADER_SIZE,
                                             m_headerFilled, timeout, error);
  if (headerOutcome != ReadOutcome::Data) return headerOutcome;

  const std::size_t compressedLength = Load3(m_header);
  const std::size_t plainLength = Load3(m_header + 4);
  if (!m_bodySized) {
    if (m_header[3] != m_frameSequenceId) {
      error = "protocol desync: unexpected compressed packet sequence id";
      return ReadOutcome::Failed;
    }
    if (compressedLength == 0 && plainLength != 0) {
      error = "malformed compressed packet: empty payload declares " +
              std::to_string(plainLength) + " bytes before compression";
      return ReadOutcome::Failed;
    }
    m_body.resize(compressedLength);
    m_bodyFilled = 0;
    m_bodySized = true;
  }

  const ReadOutcome bodyOutcome =
      FillFrom(m_body.data(), m_body.size(), m_bodyFilled, timeout, error);
  if (bodyOutcome != ReadOutcome::Data) return bodyOutcome;

  if (plainLength == 0) {
    m_plain.swap(m_body);
  } else {
    m_plain.resize(plainLength);
    std::size_t produced = 0;
    if (m_algorithm == CompressionAlgorithm::Zlib) {
      uLongf inflated = static_cast<uLongf>(plainLength);
      const int status =
          uncompress(reinterpret_cast<Bytef *>(m_plain.data()), &inflated,
                     reinterpret_cast<const Bytef *>(m_body.data()),
                     static_cast<uLong>(m_body.size()));
      if (status != Z_OK) {
        error =
            "cannot decompress a packet: zlib error " + std::to_string(status);
        return ReadOutcome::Failed;
      }
      produced = inflated;
    } else {
      if (m_decompressContext == nullptr)
        m_decompressContext = ZSTD_createDCtx();
      produced = ZSTD_decompressDCtx(m_decompressContext, m_plain.data(),
                                     plainLength, m_body.data(), m_body.size());
      if (ZSTD_isError(produced) != 0) {
        error = std::string("cannot decompress a packet: ") +
                ZSTD_getErrorName(produced);
        return ReadOutcome::Failed;
      }
    }
    if (produced != plainLength) {
      error = "malformed compressed packet: decompressed to " +
              std::to_string(produced) + " bytes, header declares " +
              std::to_string(plainLength);
      return ReadOutcome::Failed;
    }
  }
  m_plainOffset = 0;
  ++m_frameSequenceId;
  m_body.clear();
  DiscardFrame();
  return ReadOutcome::Data;
}

bool CompressedTransport::Write(std::span<const std::uint8_t> data,
                                std::chrono::milliseconds timeout,
                                std::string &error) {
  if (!Enabled()) return m_inner.Write(data, timeout, error);
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t chunkSize =
        std::min(data.size() - offset, MAX_COMPRESSED_FRAME_PAYLOAD);
    if (!WriteFrame(data.subspan(offset, chunkSize), timeout, error))
      return false;
    offset += chunkSize;
  }
  return true;
}

bool CompressedTransport::WriteFrame(std::span<const std::uint8_t> chunk,
                                     std::chrono::milliseconds timeout,
                                     std::string &error) {
  std::size_t plainLength = 0;
  m_frameOut.resize(COMPRESSED_HEADER_SIZE);
  if (chunk.size() >= MIN_COMPRESS_LENGTH) {
    // zlib gives up at complen >= len, zstd only at res > len
    // (mysys/my_compress.cc); the payload is sent uncompressed either way.
    if (m_algorithm == CompressionAlgorithm::Zlib) {
      const std::size_t bound = compressBound(static_cast<uLong>(chunk.size()));
      m_frameOut.resize(COMPRESSED_HEADER_SIZE + bound);
      uLongf produced = static_cast<uLongf>(bound);
      const int status = compress2(
          reinterpret_cast<Bytef *>(m_frameOut.data() + COMPRESSED_HEADER_SIZE),
          &produced, reinterpret_cast<const Bytef *>(chunk.data()),
          static_cast<uLong>(chunk.size()), m_level);
      if (status == Z_OK && produced < chunk.size()) {
        plainLength = chunk.size();
        m_frameOut.resize(COMPRESSED_HEADER_SIZE + produced);
      }
    } else {
      if (m_compressContext == nullptr) m_compressContext = ZSTD_createCCtx();
      const std::size_t bound = ZSTD_compressBound(chunk.size());
      m_frameOut.resize(COMPRESSED_HEADER_SIZE + bound);
      const std::size_t produced = ZSTD_compressCCtx(
          m_compressContext, m_frameOut.data() + COMPRESSED_HEADER_SIZE, bound,
          chunk.data(), chunk.size(), m_level);
      if (ZSTD_isError(produced) == 0 && produced <= chunk.size()) {
        plainLength = chunk.size();
        m_frameOut.resize(COMPRESSED_HEADER_SIZE + produced);
      }
    }
  }
  if (plainLength == 0) {
    m_frameOut.resize(COMPRESSED_HEADER_SIZE + chunk.size());
    std::memcpy(m_frameOut.data() + COMPRESSED_HEADER_SIZE, chunk.data(),
                chunk.size());
  }

  Store3(m_frameOut.data(), m_frameOut.size() - COMPRESSED_HEADER_SIZE);
  m_frameOut[3] = m_frameSequenceId++;
  Store3(m_frameOut.data() + 4, plainLength);
  const bool written = m_inner.Write(m_frameOut, timeout, error);
  if (m_frameOut.capacity() > FRAME_BUFFER_KEEP) {
    m_frameOut.clear();
    m_frameOut.shrink_to_fit();
  }
  return written;
}

}  // namespace binlog_streamer
