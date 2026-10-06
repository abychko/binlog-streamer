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

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

IpAddress MakeIpv4(std::uint8_t a, std::uint8_t b, std::uint8_t c,
                   std::uint8_t d) {
  IpAddress address{AddressFamily::Ipv4, {}};
  address.bytes[0] = a;
  address.bytes[1] = b;
  address.bytes[2] = c;
  address.bytes[3] = d;
  return address;
}

void ExpectAddressParses(std::string_view text, const IpAddress &expected) {
  IpAddress address{};
  std::string error;
  ASSERT_TRUE(AddressRangeParser::ParseAddress(text, address, error)) << text;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(address, expected) << text;
}

void ExpectAddressFails(std::string_view text) {
  IpAddress address{};
  std::string error;
  EXPECT_FALSE(AddressRangeParser::ParseAddress(text, address, error)) << text;
  EXPECT_FALSE(error.empty());
}

void ExpectRangeParses(std::string_view text, const IpAddress &expectedAddress,
                       std::uint8_t expectedPrefix) {
  AddressRange range{};
  std::string error;
  ASSERT_TRUE(AddressRangeParser::ParseRange(text, range, error)) << text;
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(range.address, expectedAddress) << text;
  EXPECT_EQ(range.prefixLength, expectedPrefix) << text;
}

void ExpectRangeFails(std::string_view text) {
  AddressRange range{};
  std::string error;
  EXPECT_FALSE(AddressRangeParser::ParseRange(text, range, error)) << text;
  EXPECT_FALSE(error.empty());
}

TEST(AddressRangeParserTest, ParsesPlainAddresses) {
  ExpectAddressParses("10.0.1.15", MakeIpv4(10, 0, 1, 15));
  ExpectRangeParses("10.0.1.15", MakeIpv4(10, 0, 1, 15), 32);
  IpAddress documentation{AddressFamily::Ipv6, {}};
  documentation.bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                         0,    0,    0,    0,    0, 0, 0, 1};
  ExpectAddressParses("2001:db8::1", documentation);
}

TEST(AddressRangeParserTest, ParsesCidrRanges) {
  ExpectRangeParses("10.0.2.0/24", MakeIpv4(10, 0, 2, 0), 24);
  ExpectRangeParses("0.0.0.0/0", MakeIpv4(0, 0, 0, 0), 0);
  IpAddress network{AddressFamily::Ipv6, {}};
  network.bytes = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  ExpectRangeParses("2001:db8::/64", network, 64);
}

TEST(AddressRangeParserTest, ParsesIpv4MappedIpv6WithMaximalPrefix) {
  IpAddress mapped{AddressFamily::Ipv6, {}};
  mapped.bytes = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 1};
  ExpectRangeParses("::ffff:10.0.0.1/128", mapped, 128);
}

TEST(AddressRangeParserTest, RejectsNonZeroHostBits) {
  ExpectRangeFails("10.0.2.5/24");
}

TEST(AddressRangeParserTest, RejectsPrefixOutOfRange) {
  ExpectRangeFails("10.0.0.0/33");
  ExpectRangeFails("2001:db8::/129");
}

TEST(AddressRangeParserTest, RejectsHostNamesAndZoneIdentifiers) {
  ExpectAddressFails("host.example");
  ExpectAddressFails("10.0.1.%");
  ExpectAddressFails("");
  ExpectAddressFails("fe80::1%en0");
}

TEST(AddressRangeParserTest, RejectsLeadingZeroOctets) {
  ExpectAddressFails("010.0.0.1");
}

}  // namespace
}  // namespace binlog_streamer
