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

#include "binlog/cSerializationDecoder.hpp"

#include <bit>

namespace binlog_streamer {
namespace {

constexpr std::uint64_t SERIALIZATION_FORMAT_VERSION = 1;

}  // namespace

bool SerializationDecoder::ReadUnsigned(std::uint64_t &value) {
  if (m_position >= m_bytes.size()) return false;
  const std::uint8_t first = m_bytes[m_position];
  // 0xFF is the nine-byte form; std::countr_one(0xFF) is 8, so a byte is never
  // shifted by more than its width.
  const std::size_t byteCount =
      static_cast<std::size_t>(std::countr_one(first)) + 1;
  if (m_bytes.size() - m_position < byteCount) return false;
  std::uint64_t result =
      byteCount == 9 ? 0 : static_cast<std::uint64_t>(first >> byteCount);
  std::uint64_t rest = 0;
  for (std::size_t i = 1; i < byteCount; ++i)
    rest |= static_cast<std::uint64_t>(m_bytes[m_position + i])
            << (8 * (i - 1));
  // The value bits the first byte did not hold continue in the rest, shifted by
  // 8 - byteCount; in the nine-byte form the rest is the whole 64-bit value.
  result |= byteCount == 9 ? rest : rest << (8 - byteCount);
  value = result;
  m_position += byteCount;
  return true;
}

bool SerializationDecoder::ReadSigned(std::int64_t &value) {
  std::uint64_t encoded = 0;
  if (!ReadUnsigned(encoded)) return false;
  const std::uint64_t magnitude = encoded >> 1;
  // (x ^ sign_mask) with sign_mask all ones is -(x + 1).
  value = (encoded & 1) != 0 ? static_cast<std::int64_t>(~magnitude)
                             : static_cast<std::int64_t>(magnitude);
  return true;
}

bool SerializationDecoder::ReadString(std::size_t maxLength,
                                      std::string &value) {
  const std::size_t start = m_position;
  std::uint64_t length = 0;
  if (!ReadUnsigned(length)) return false;
  if (length > maxLength || length > m_bytes.size() - m_position) {
    m_position = start;
    return false;
  }
  value.assign(reinterpret_cast<const char *>(m_bytes.data() + m_position),
               static_cast<std::size_t>(length));
  m_position += static_cast<std::size_t>(length);
  return true;
}

bool SerializationDecoder::ReadByteArray(std::span<std::uint8_t> out) {
  const std::size_t start = m_position;
  for (auto &byte : out) {
    std::uint64_t element = 0;
    if (!ReadUnsigned(element) || element > 0xFF) {
      m_position = start;
      return false;
    }
    byte = static_cast<std::uint8_t>(element);
  }
  return true;
}

bool SerializationDecoder::ReadMessageHeader(
    std::uint64_t &payloadSize, std::uint64_t &lastNonIgnorableFieldId) {
  const std::size_t start = m_position;
  std::uint64_t version = 0;
  if (!ReadUnsigned(version) || version != SERIALIZATION_FORMAT_VERSION ||
      !ReadUnsigned(payloadSize) || !ReadUnsigned(lastNonIgnorableFieldId)) {
    m_position = start;
    return false;
  }
  return true;
}

bool SerializationDecoder::PeekFieldId(std::uint64_t &id) {
  const std::size_t start = m_position;
  const bool ok = ReadUnsigned(id);
  m_position = start;
  return ok;
}

bool SerializationDecoder::SkipFieldId() {
  std::uint64_t id = 0;
  return ReadUnsigned(id);
}

}  // namespace binlog_streamer
