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

#include "protocol/cOkPacketCodec.hpp"

#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cLengthEncodedString.hpp"

namespace binlog_streamer {

bool OkPacketCodec::IsOkPacket(std::uint8_t headerByte,
                               bool clientDeprecateEof) {
  return headerByte == 0x00 || (clientDeprecateEof && headerByte == 0xFE);
}

bool OkPacketCodec::Parse(std::span<const std::uint8_t> payload,
                          bool clientSessionTrack, OkPacket &value,
                          std::string &error) {
  if (payload.empty()) {
    error = "OK packet is empty";
    return false;
  }
  std::size_t pos = 1;
  std::uint64_t affectedRows = 0;
  bool isNull = false;
  std::size_t consumed =
      LengthEncodedInteger::Decode(payload.subspan(pos), affectedRows, isNull);
  if (consumed == 0 || isNull) {
    error = "OK packet: malformed affected_rows";
    return false;
  }
  pos += consumed;

  std::uint64_t lastInsertId = 0;
  consumed =
      LengthEncodedInteger::Decode(payload.subspan(pos), lastInsertId, isNull);
  if (consumed == 0 || isNull) {
    error = "OK packet: malformed last_insert_id";
    return false;
  }
  pos += consumed;

  if (payload.size() - pos < 4) {
    error = "OK packet: too short for status flags and warnings";
    return false;
  }
  const std::uint16_t statusFlags = static_cast<std::uint16_t>(
      payload[pos] | (static_cast<std::uint32_t>(payload[pos + 1]) << 8));
  const std::uint16_t warnings = static_cast<std::uint16_t>(
      payload[pos + 2] | (static_cast<std::uint32_t>(payload[pos + 3]) << 8));
  pos += 4;

  value.affectedRows = affectedRows;
  value.lastInsertId = lastInsertId;
  value.statusFlags = statusFlags;
  value.warnings = warnings;

  if (clientSessionTrack) {
    // net_send_ok() writes nothing at all here when there is neither a
    // message nor a session state change, so an empty remainder is
    // "no info", not a malformed missing length prefix.
    if (pos == payload.size()) {
      value.info.clear();
    } else {
      std::string info;
      if (LengthEncodedString::Decode(payload.subspan(pos), info) == 0) {
        error = "OK packet: malformed info under CLIENT_SESSION_TRACK";
        return false;
      }
      value.info = std::move(info);
      // Whatever follows is session state change data (SessionStateInfo),
      // not part of info - not decoded here.
    }
  } else {
    value.info.assign(reinterpret_cast<const char *>(payload.data() + pos),
                      payload.size() - pos);
  }
  error.clear();
  return true;
}

void OkPacketCodec::Encode(const OkPacket &value, bool clientSessionTrack,
                           std::vector<std::uint8_t> &out) {
  out.push_back(0x00);
  LengthEncodedInteger::Encode(value.affectedRows, out);
  LengthEncodedInteger::Encode(value.lastInsertId, out);
  out.push_back(static_cast<std::uint8_t>(value.statusFlags));
  out.push_back(static_cast<std::uint8_t>(value.statusFlags >> 8));
  out.push_back(static_cast<std::uint8_t>(value.warnings));
  out.push_back(static_cast<std::uint8_t>(value.warnings >> 8));
  if (value.info.empty()) return;
  if (clientSessionTrack) {
    LengthEncodedString::Encode(value.info, out);
  } else {
    out.insert(out.end(), value.info.begin(), value.info.end());
  }
}

}  // namespace binlog_streamer
