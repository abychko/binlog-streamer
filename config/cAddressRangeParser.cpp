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

#include "cAddressRangeParser.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <system_error>

namespace binlog_streamer {
namespace {
bool ValidIpv4Text(std::string_view text) {
  for (int part = 0; part < 4; ++part) {
    const auto dot = text.find('.');
    const auto octet = text.substr(0, dot);
    if (octet.empty() || octet.size() > 3 ||
        octet.find_first_not_of("0123456789") != std::string_view::npos ||
        (octet.size() > 1 && octet.front() == '0'))
      return false;
    if (part == 3) return dot == std::string_view::npos;
    if (dot == std::string_view::npos) return false;
    text.remove_prefix(dot + 1);
  }
  return false;
}
}  // namespace
bool AddressRangeParser::ParseAddress(std::string_view text, IpAddress &address,
                                      std::string &error) {
  error = "expected an IPv4 or IPv6 address without a zone identifier";
  if (text.empty() || text.find_first_not_of("0123456789abcdefABCDEF:.") !=
                          std::string_view::npos)
    return false;
  const bool ipv6 = text.find(':') != std::string_view::npos;
  if (!ipv6 && !ValidIpv4Text(text)) return false;
  if (ipv6 && text.find('.') != std::string_view::npos &&
      !ValidIpv4Text(text.substr(text.rfind(':') + 1)))
    return false;
  IpAddress parsed;
  parsed.family = ipv6 ? AddressFamily::Ipv6 : AddressFamily::Ipv4;
  const std::string terminated(text);
  if (inet_pton(ipv6 ? AF_INET6 : AF_INET, terminated.c_str(),
                parsed.bytes.data()) != 1)
    return false;
  address = parsed;
  error.clear();
  return true;
}
bool AddressRangeParser::ParseRange(std::string_view text, AddressRange &range,
                                    std::string &error) {
  const auto slash = text.find('/');
  AddressRange parsed;
  if (!ParseAddress(text.substr(0, slash), parsed.address, error)) return false;
  unsigned int prefix = parsed.address.family == AddressFamily::Ipv4 ? 32 : 128;
  const unsigned int bits = prefix;
  if (slash != std::string_view::npos) {
    const auto suffix = text.substr(slash + 1);
    error = "expected a CIDR prefix in the address-family range";
    if (suffix.empty() ||
        suffix.find_first_not_of("0123456789") != std::string_view::npos)
      return false;
    const auto [end, status] =
        std::from_chars(suffix.data(), suffix.data() + suffix.size(), prefix);
    if (status != std::errc{} || end != suffix.data() + suffix.size() ||
        prefix > bits)
      return false;
  }
  for (unsigned int bit = prefix; bit < bits; ++bit) {
    if ((parsed.address.bytes[bit / 8] & (1U << (7 - bit % 8)))) {
      error = "CIDR address has non-zero host bits";
      return false;
    }
  }
  parsed.prefixLength = static_cast<std::uint8_t>(prefix);
  range = parsed;
  error.clear();
  return true;
}
}  // namespace binlog_streamer
