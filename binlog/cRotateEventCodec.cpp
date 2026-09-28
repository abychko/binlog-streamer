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

#include "binlog/cRotateEventCodec.hpp"

namespace binlog_streamer {
namespace {
constexpr std::size_t POSITION_LENGTH =
    8;  // R_IDENT_OFFSET: where the file name starts
}  // namespace

bool RotateEventCodec::Parse(std::span<const std::uint8_t> body,
                             std::size_t checksumLength, RotateEvent &value,
                             std::string &error) {
  if (body.size() < checksumLength) {
    error = "Rotate event body shorter than the negotiated checksum";
    return false;
  }
  const std::span<const std::uint8_t> withoutChecksum =
      body.first(body.size() - checksumLength);
  if (withoutChecksum.size() < POSITION_LENGTH) {
    error = "Rotate event body shorter than its fixed position field";
    return false;
  }
  std::uint64_t position = 0;
  for (int i = 0; i < 8; ++i)
    position |=
        static_cast<std::uint64_t>(withoutChecksum[static_cast<std::size_t>(i)])
        << (8 * i);
  value.position = position;
  value.fileName.assign(withoutChecksum.begin() + POSITION_LENGTH,
                        withoutChecksum.end());
  return true;
}

}  // namespace binlog_streamer
