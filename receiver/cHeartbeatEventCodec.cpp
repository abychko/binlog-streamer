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

#include "receiver/cHeartbeatEventCodec.hpp"

#include "protocol/cLengthEncodedInteger.hpp"

namespace binlog_streamer {
namespace {
constexpr std::uint64_t FIELD_END_MARK = 0;
constexpr std::uint64_t FIELD_LOG_FILENAME = 1;
constexpr std::uint64_t FIELD_LOG_POSITION = 2;
}  // namespace

void HeartbeatEventCodec::ParseV1(std::span<const std::uint8_t> body,
                                  std::size_t checksumLength,
                                  HeartbeatEvent &value) {
  const std::size_t nameLength =
      body.size() > checksumLength ? body.size() - checksumLength : 0;
  value.fileName.assign(body.begin(),
                        body.begin() + static_cast<std::ptrdiff_t>(nameLength));
  value.position.reset();
}

bool HeartbeatEventCodec::ParseV2(std::span<const std::uint8_t> body,
                                  std::size_t checksumLength,
                                  HeartbeatEvent &value, std::string &error) {
  if (body.size() < checksumLength) {
    error = "heartbeat v2 body shorter than the negotiated checksum";
    return false;
  }
  const std::span<const std::uint8_t> data =
      body.first(body.size() - checksumLength);
  std::size_t pos = 0;
  for (;;) {
    std::uint64_t type = 0;
    bool isNull = false;
    std::size_t consumed =
        LengthEncodedInteger::Decode(data.subspan(pos), type, isNull);
    if (consumed == 0 || isNull) {
      error = "malformed or truncated field type in heartbeat v2 body";
      return false;
    }
    pos += consumed;
    if (type == FIELD_END_MARK)
      break;  // no length or value follows the end marker

    std::uint64_t length = 0;
    consumed = LengthEncodedInteger::Decode(data.subspan(pos), length, isNull);
    if (consumed == 0 || isNull) {
      error = "malformed or truncated field length in heartbeat v2 body";
      return false;
    }
    pos += consumed;
    if (length > data.size() - pos) {
      error = "heartbeat v2 field value runs past the end of the event body";
      return false;
    }
    const std::span<const std::uint8_t> fieldValue = data.subspan(pos, length);
    if (type == FIELD_LOG_FILENAME) {
      value.fileName.assign(fieldValue.begin(), fieldValue.end());
    } else if (type == FIELD_LOG_POSITION) {
      // The value is itself a lenenc integer (net_store_length() in the
      // encoder); `length` is that inner encoding's size, not the position.
      std::uint64_t position = 0;
      bool positionIsNull = false;
      if (LengthEncodedInteger::Decode(fieldValue, position, positionIsNull) ==
              0 ||
          positionIsNull) {
        error = "malformed position value in heartbeat v2 body";
        return false;
      }
      value.position = position;
    }
    pos += length;
  }
  return true;
}

}  // namespace binlog_streamer
