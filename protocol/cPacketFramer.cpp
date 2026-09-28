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

#include "protocol/cPacketFramer.hpp"

#include <algorithm>
#include "protocol/hProtocolLimits.hpp"

namespace binlog_streamer {

void PacketFramer::Encode(std::span<const std::uint8_t> payload,
                          std::uint8_t &sequenceId,
                          std::vector<std::uint8_t> &out) {
  Encode({}, payload, sequenceId, out);
}

void PacketFramer::Encode(std::span<const std::uint8_t> head,
                          std::span<const std::uint8_t> body,
                          std::uint8_t &sequenceId,
                          std::vector<std::uint8_t> &out) {
  const std::size_t total = head.size() + body.size();
  std::size_t offset = 0;
  std::size_t chunkSize = 0;
  do {
    chunkSize = std::min(total - offset, MAX_PAYLOAD_PER_PACKET);
    out.push_back(static_cast<std::uint8_t>(chunkSize));
    out.push_back(static_cast<std::uint8_t>(chunkSize >> 8));
    out.push_back(static_cast<std::uint8_t>(chunkSize >> 16));
    out.push_back(sequenceId++);
    const std::size_t end = offset + chunkSize;
    if (offset < head.size())
      out.insert(out.end(), head.begin() + static_cast<std::ptrdiff_t>(offset),
                 head.begin() +
                     static_cast<std::ptrdiff_t>(std::min(end, head.size())));
    if (end > head.size())
      out.insert(
          out.end(),
          body.begin() + static_cast<std::ptrdiff_t>(
                             std::max(offset, head.size()) - head.size()),
          body.begin() + static_cast<std::ptrdiff_t>(end - head.size()));
    offset = end;
    // A chunk exactly MAX_PAYLOAD_PER_PACKET long is ambiguous on the wire
    // (whole payload, or first sub-packet of a larger one), so another
    // sub-packet always follows - zero-length if nothing remains.
  } while (chunkSize == MAX_PAYLOAD_PER_PACKET);
}

PacketDecodeResult PacketFramer::Decode(std::span<const std::uint8_t> data,
                                        std::uint8_t &sequenceId,
                                        std::vector<std::uint8_t> &payload,
                                        bool verifySequence) {
  std::vector<std::uint8_t> reassembled;
  std::size_t pos = 0;
  std::uint8_t nextSequenceId = sequenceId;
  while (true) {
    if (data.size() - pos < PACKET_HEADER_SIZE) {
      return {PacketDecodeStatus::NeedMoreBytes, 0,
              PACKET_HEADER_SIZE - (data.size() - pos)};
    }
    const std::size_t subPacketLength =
        static_cast<std::size_t>(data[pos]) |
        (static_cast<std::size_t>(data[pos + 1]) << 8) |
        (static_cast<std::size_t>(data[pos + 2]) << 16);
    const std::uint8_t subPacketSequenceId = data[pos + 3];
    if (verifySequence && subPacketSequenceId != nextSequenceId) {
      return {PacketDecodeStatus::SequenceMismatch, 0, 0};
    }
    const std::size_t available = data.size() - pos - PACKET_HEADER_SIZE;
    if (available < subPacketLength) {
      return {PacketDecodeStatus::NeedMoreBytes, 0,
              subPacketLength - available};
    }
    reassembled.insert(
        reassembled.end(),
        data.begin() + static_cast<std::ptrdiff_t>(pos + PACKET_HEADER_SIZE),
        data.begin() + static_cast<std::ptrdiff_t>(pos + PACKET_HEADER_SIZE +
                                                   subPacketLength));
    pos += PACKET_HEADER_SIZE + subPacketLength;
    ++nextSequenceId;
    if (subPacketLength < MAX_PAYLOAD_PER_PACKET) break;
  }
  sequenceId = nextSequenceId;
  payload = std::move(reassembled);
  return {PacketDecodeStatus::Complete, pos, 0};
}

PacketDecodeResult PacketFramer::Measure(std::span<const std::uint8_t> data,
                                         std::uint8_t sequenceId,
                                         bool verifySequence) {
  std::size_t pos = 0;
  std::uint8_t nextSequenceId = sequenceId;
  while (true) {
    if (data.size() - pos < PACKET_HEADER_SIZE) {
      return {PacketDecodeStatus::NeedMoreBytes, 0,
              PACKET_HEADER_SIZE - (data.size() - pos)};
    }
    const std::size_t subPacketLength =
        static_cast<std::size_t>(data[pos]) |
        (static_cast<std::size_t>(data[pos + 1]) << 8) |
        (static_cast<std::size_t>(data[pos + 2]) << 16);
    const std::uint8_t subPacketSequenceId = data[pos + 3];
    if (verifySequence && subPacketSequenceId != nextSequenceId) {
      return {PacketDecodeStatus::SequenceMismatch, 0, 0};
    }
    const std::size_t available = data.size() - pos - PACKET_HEADER_SIZE;
    if (available < subPacketLength) {
      return {PacketDecodeStatus::NeedMoreBytes, 0,
              subPacketLength - available};
    }
    pos += PACKET_HEADER_SIZE + subPacketLength;
    ++nextSequenceId;
    if (subPacketLength < MAX_PAYLOAD_PER_PACKET) break;
  }
  return {PacketDecodeStatus::Complete, pos, 0};
}

}  // namespace binlog_streamer
