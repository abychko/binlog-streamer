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
#include "config/cAddressRangeMatcher.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

AddressRange ParseRange(std::string_view text) {
  AddressRange range{};
  std::string error;
  EXPECT_TRUE(AddressRangeParser::ParseRange(text, range, error))
      << text << ": " << error;
  return range;
}

IpAddress ParseAddress(std::string_view text) {
  IpAddress address{};
  std::string error;
  EXPECT_TRUE(AddressRangeParser::ParseAddress(text, address, error))
      << text << ": " << error;
  return address;
}

TEST(AddressRangeMatcherTest, MatchesAnExactAddress) {
  EXPECT_TRUE(AddressRangeMatcher::Contains(ParseRange("192.0.2.10"),
                                            ParseAddress("192.0.2.10")));
  EXPECT_FALSE(AddressRangeMatcher::Contains(ParseRange("192.0.2.10"),
                                             ParseAddress("192.0.2.11")));
}

TEST(AddressRangeMatcherTest, MatchesWithinACidrRangeButNotOutsideIt) {
  const AddressRange range = ParseRange("192.0.2.0/24");
  EXPECT_TRUE(AddressRangeMatcher::Contains(range, ParseAddress("192.0.2.0")));
  EXPECT_TRUE(
      AddressRangeMatcher::Contains(range, ParseAddress("192.0.2.255")));
  EXPECT_FALSE(AddressRangeMatcher::Contains(range, ParseAddress("192.0.3.0")));
}

TEST(AddressRangeMatcherTest, ZeroLengthPrefixMatchesEveryAddressOfItsFamily) {
  const AddressRange range = ParseRange("0.0.0.0/0");
  EXPECT_TRUE(
      AddressRangeMatcher::Contains(range, ParseAddress("255.255.255.255")));
  EXPECT_TRUE(AddressRangeMatcher::Contains(range, ParseAddress("192.0.2.1")));
}

TEST(AddressRangeMatcherTest, MatchesOnANonByteAlignedPrefixBoundary) {
  const AddressRange range = ParseRange("192.0.2.128/26");
  EXPECT_TRUE(
      AddressRangeMatcher::Contains(range, ParseAddress("192.0.2.191")));
  EXPECT_FALSE(
      AddressRangeMatcher::Contains(range, ParseAddress("192.0.2.192")));
}

TEST(AddressRangeMatcherTest,
     Ipv6RangeNeverMatchesAnIpv4AddressEvenWhenMapped) {
  const AddressRange range = ParseRange("::ffff:192.0.2.10/128");
  EXPECT_FALSE(
      AddressRangeMatcher::Contains(range, ParseAddress("192.0.2.10")));
}

TEST(AddressRangeMatcherTest, MatchesWithinAnIpv6CidrRange) {
  const AddressRange range = ParseRange("2001:db8::/64");
  EXPECT_TRUE(
      AddressRangeMatcher::Contains(range, ParseAddress("2001:db8::1")));
  EXPECT_FALSE(
      AddressRangeMatcher::Contains(range, ParseAddress("2001:db8:1::1")));
}

}  // namespace
}  // namespace binlog_streamer
