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
#include "config/cIpAddressText.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(IpAddressTextTest, FormatsAnIpv4Address) {
  IpAddress address{};
  std::string error;
  ASSERT_TRUE(AddressRangeParser::ParseAddress("192.0.2.10", address, error))
      << error;
  EXPECT_EQ(IpAddressText::Format(address), "192.0.2.10");
}

TEST(IpAddressTextTest, FormatsAnIpv6AddressInCanonicalForm) {
  IpAddress address{};
  std::string error;
  ASSERT_TRUE(AddressRangeParser::ParseAddress(
      "2001:0db8:0000:0000:0000:0000:0000:0001", address, error))
      << error;
  EXPECT_EQ(
      IpAddressText::Format(address),
      "2001:db8::1");  // inet_ntop's own canonical (zero-run-compressed) form
}

TEST(IpAddressTextTest, RoundTripsThroughParseAddress) {
  for (const auto *text :
       {"0.0.0.0", "255.255.255.255", "192.0.2.15", "::1", "2001:db8::"}) {
    IpAddress address{};
    std::string error;
    ASSERT_TRUE(AddressRangeParser::ParseAddress(text, address, error))
        << text << ": " << error;
    IpAddress roundTripped{};
    ASSERT_TRUE(AddressRangeParser::ParseAddress(IpAddressText::Format(address),
                                                 roundTripped, error))
        << IpAddressText::Format(address) << ": " << error;
    EXPECT_EQ(roundTripped, address) << text;
  }
}

}  // namespace
}  // namespace binlog_streamer
