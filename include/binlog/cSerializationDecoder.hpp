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

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace binlog_streamer {

// Implements only what the tagged GTID event (type 42) body needs, not the
// full percona-server serialization wire format.
class SerializationDecoder {
 public:
  explicit SerializationDecoder(std::span<const std::uint8_t> bytes)
      : m_bytes(bytes) {}

  bool ReadUnsigned(std::uint64_t &value);
  bool ReadSigned(std::int64_t &value);
  // maxLength mirrors the same bound the encoder enforced; a longer
  // length is rejected.
  bool ReadString(std::size_t maxLength, std::string &value);
  // Each element is its own varint (Serializer_array_tag), not a raw
  // byte: 0x80..0xFF take two bytes each.
  bool ReadByteArray(std::span<std::uint8_t> out);

  // version(=1) is encoded in the slot a nested message's field id would
  // occupy.
  bool ReadMessageHeader(std::uint64_t &payloadSize,
                         std::uint64_t &lastNonIgnorableFieldId);
  bool PeekFieldId(std::uint64_t &id);
  bool SkipFieldId();

  std::size_t Position() const { return m_position; }
  bool AtEnd() const { return m_position >= m_bytes.size(); }

 private:
  std::span<const std::uint8_t> m_bytes;
  std::size_t m_position = 0;
};

}  // namespace binlog_streamer
