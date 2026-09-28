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

#include "protocol/cAuthMoreDataCodec.hpp"

namespace binlog_streamer {

namespace {
constexpr std::uint8_t FAST_AUTH_SUCCESS = 0x03;
constexpr std::uint8_t PERFORM_FULL_AUTHENTICATION = 0x04;
}  // namespace

bool AuthMoreDataCodec::IsAuthMoreData(std::span<const std::uint8_t> payload) {
  return !payload.empty() && payload[0] == 0x01;
}

bool AuthMoreDataCodec::Parse(std::span<const std::uint8_t> payload,
                              AuthMoreDataSignal &signal,
                              std::span<const std::uint8_t> &data,
                              std::string &error) {
  if (!IsAuthMoreData(payload)) {
    error = "not an AuthMoreData packet";
    return false;
  }
  data = payload.subspan(1);
  if (data.size() == 1 && data[0] == FAST_AUTH_SUCCESS) {
    signal = AuthMoreDataSignal::FastAuthSuccess;
  } else if (data.size() == 1 && data[0] == PERFORM_FULL_AUTHENTICATION) {
    signal = AuthMoreDataSignal::PerformFullAuthentication;
  } else {
    signal = AuthMoreDataSignal::Other;
  }
  error.clear();
  return true;
}

bool AuthMoreDataCodec::EncodeSignal(AuthMoreDataSignal signal,
                                     std::vector<std::uint8_t> &out) {
  std::uint8_t signalByte = 0;
  switch (signal) {
    case AuthMoreDataSignal::FastAuthSuccess:
      signalByte = FAST_AUTH_SUCCESS;
      break;
    case AuthMoreDataSignal::PerformFullAuthentication:
      signalByte = PERFORM_FULL_AUTHENTICATION;
      break;
    case AuthMoreDataSignal::Other:
      return false;
  }
  out.push_back(0x01);
  out.push_back(signalByte);
  return true;
}

}  // namespace binlog_streamer
