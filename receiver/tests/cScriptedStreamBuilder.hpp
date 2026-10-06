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

#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "protocol/cPacketFramer.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer::test {

class ScriptedStreamBuilder {
 public:
  const std::vector<std::uint8_t> &Bytes() const { return m_bytes; }
  std::uint8_t SequenceId() const { return m_sequenceId; }

  void PushEvent(std::uint8_t type, std::span<const std::uint8_t> body,
                 std::size_t checksumLength, std::uint32_t nextPosition = 0,
                 std::uint16_t flags = 0, std::uint32_t serverId = 1,
                 std::uint32_t timestamp = 0) {
    const std::uint32_t eventLength = static_cast<std::uint32_t>(
        EVENT_HEADER_LENGTH + body.size() + checksumLength);
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + eventLength);
    payload.push_back(0x00);
    AppendUint32LE(payload, timestamp);
    payload.push_back(type);
    AppendUint32LE(payload, serverId);
    AppendUint32LE(payload, eventLength);
    AppendUint32LE(payload, nextPosition);
    AppendUint16LE(payload, flags);
    payload.insert(payload.end(), body.begin(), body.end());
    payload.insert(payload.end(), checksumLength, std::uint8_t{0});
    PacketFramer::Encode(payload, m_sequenceId, m_bytes);
  }

  void PushRotate(std::uint64_t position, const std::string &fileName,
                  bool artificial, std::size_t checksumLength) {
    std::vector<std::uint8_t> body;
    AppendUint64LE(body, position);
    body.insert(body.end(), fileName.begin(), fileName.end());
    PushEvent(4 /* ROTATE_EVENT */, body, checksumLength,
              artificial ? 0 : position,
              artificial ? EVENT_FLAG_ARTIFICIAL : std::uint16_t{0}, 1,
              artificial ? 0 : 1700000000);
  }

  void PushHeartbeatV1(const std::string &fileName, std::size_t checksumLength,
                       std::uint32_t nextPosition = 0) {
    const std::vector<std::uint8_t> body(fileName.begin(), fileName.end());
    PushEvent(27 /* HEARTBEAT_LOG_EVENT */, body, checksumLength, nextPosition);
  }

  void PushHeartbeatV2(const std::string &fileName,
                       std::optional<std::uint64_t> position,
                       std::size_t checksumLength,
                       std::uint32_t nextPosition = 0) {
    std::vector<std::uint8_t> body;
    AppendLenenc(body, 1);  // OTW_HB_LOG_FILENAME_FIELD
    AppendLenenc(body, fileName.size());
    body.insert(body.end(), fileName.begin(), fileName.end());
    if (position.has_value()) {
      std::vector<std::uint8_t> encodedPosition;
      AppendLenenc(encodedPosition, *position);
      AppendLenenc(body, 2);  // OTW_HB_LOG_POSITION_FIELD
      AppendLenenc(body, encodedPosition.size());
      body.insert(body.end(), encodedPosition.begin(), encodedPosition.end());
    }
    AppendLenenc(body, 0);  // OTW_HB_HEADER_END_MARK
    PushEvent(41 /* HEARTBEAT_LOG_EVENT_V2 */, body, checksumLength,
              nextPosition);
  }

  void PushErr(std::uint16_t code, const std::string &message,
               const std::string &sqlState = "HY000") {
    std::vector<std::uint8_t> payload;
    payload.push_back(0xFF);
    AppendUint16LE(payload, code);
    payload.push_back('#');
    payload.insert(payload.end(), sqlState.begin(), sqlState.end());
    payload.insert(payload.end(), message.begin(), message.end());
    PacketFramer::Encode(payload, m_sequenceId, m_bytes);
  }

  // mysql_binlog_fetch()'s EOF: a packet shorter than 9 bytes disambiguates
  // 0xFE from a length-encoded integer.
  void PushEof() {
    PacketFramer::Encode(std::vector<std::uint8_t>{0xFE}, m_sequenceId,
                         m_bytes);
  }

 private:
  static void AppendUint16LE(std::vector<std::uint8_t> &out,
                             std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
  }
  static void AppendUint32LE(std::vector<std::uint8_t> &out,
                             std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  static void AppendUint64LE(std::vector<std::uint8_t> &out,
                             std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
      out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  }
  static void AppendLenenc(std::vector<std::uint8_t> &out,
                           std::uint64_t value) {
    if (value < 251) {
      out.push_back(static_cast<std::uint8_t>(value));
    } else if (value < 65536) {
      out.push_back(0xFC);
      AppendUint16LE(out, static_cast<std::uint16_t>(value));
    } else {
      out.push_back(0xFE);
      AppendUint64LE(out, value);
    }
  }

  std::uint8_t m_sequenceId = 0;
  std::vector<std::uint8_t> m_bytes;
};

}  // namespace binlog_streamer::test
