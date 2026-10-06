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

#include "config/cIpAddressText.hpp"

#include <arpa/inet.h>

namespace binlog_streamer {

std::string IpAddressText::Format(const IpAddress &address) {
  char buffer[INET6_ADDRSTRLEN] = {};
  const int family = address.family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET;
  // bytes.data() holds in_addr/in6_addr; an IPv4 address uses only its first 4
  // bytes.
  if (inet_ntop(family, address.bytes.data(), buffer, sizeof(buffer)) ==
      nullptr)
    return {};
  return buffer;
}

}  // namespace binlog_streamer
