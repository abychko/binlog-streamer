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

#include "binlog/cGtidEventCodec.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace binlog_streamer {
namespace {

void AppendLittleEndian(std::vector<std::uint8_t> &out, std::uint64_t value,
                        std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::vector<std::uint8_t> Lenenc(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  if (value < 251) {
    out.push_back(static_cast<std::uint8_t>(value));
  } else if (value < 65536) {
    out.push_back(0xFC);
    AppendLittleEndian(out, value, 2);
  } else {
    out.push_back(0xFE);
    AppendLittleEndian(out, value, 8);
  }
  return out;
}

const std::array<std::uint8_t, 16> SAMPLE_UUID_BYTES{
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};

std::vector<std::uint8_t> BuildBody(std::int64_t gno, bool withLogicalClock,
                                    bool originalTimestampDiffers,
                                    std::uint64_t transactionLength) {
  std::vector<std::uint8_t> body;
  body.push_back(0x01);
  body.insert(body.end(), SAMPLE_UUID_BYTES.begin(), SAMPLE_UUID_BYTES.end());
  AppendLittleEndian(body, static_cast<std::uint64_t>(gno), 8);
  body.push_back(withLogicalClock ? 0x02 : 0x00);
  if (!withLogicalClock) return body;
  AppendLittleEndian(body, 12345, 8);
  AppendLittleEndian(body, 12346, 8);
  std::uint64_t immediate = 0x0001'2345'6789ULL;
  if (originalTimestampDiffers) immediate |= (1ULL << 55);
  AppendLittleEndian(body, immediate, 7);
  if (originalTimestampDiffers)
    AppendLittleEndian(body, 0x0000'ABCD'EF01ULL, 7);
  const auto encodedLength = Lenenc(transactionLength);
  body.insert(body.end(), encodedLength.begin(), encodedLength.end());
  return body;
}

TEST(GtidEventCodecTest,
     ParsesUuidAndGnoWithoutTransactionLengthWhenNoLogicalClock) {
  const auto body = BuildBody(/*gno=*/22934, /*withLogicalClock=*/false,
                              /*originalTimestampDiffers=*/false, 0);
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 0, value, error)) << error;
  EXPECT_EQ(value.uuid.bytes, SAMPLE_UUID_BYTES);
  EXPECT_EQ(value.gno, 22934);
  EXPECT_FALSE(value.hasTransactionLength);
}

TEST(GtidEventCodecTest, ParsesTransactionLengthWithSevenByteTimestamp) {
  const auto body = BuildBody(22934, /*withLogicalClock=*/true,
                              /*originalTimestampDiffers=*/false, 281);
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 0, value, error)) << error;
  EXPECT_TRUE(value.hasTransactionLength);
  EXPECT_EQ(value.transactionLength, 281u);
}

TEST(GtidEventCodecTest, ParsesTransactionLengthWithFourteenByteTimestamp) {
  const auto body = BuildBody(22935, /*withLogicalClock=*/true,
                              /*originalTimestampDiffers=*/true, 1000);
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 0, value, error)) << error;
  EXPECT_TRUE(value.hasTransactionLength);
  EXPECT_EQ(value.transactionLength, 1000u);
}

TEST(GtidEventCodecTest, ExcludesTrailingChecksumFromParsing) {
  auto body = BuildBody(22934, /*withLogicalClock=*/true,
                        /*originalTimestampDiffers=*/false, 281);
  body.insert(body.end(), {0xDE, 0xAD, 0xBE, 0xEF});
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 4, value, error)) << error;
  EXPECT_EQ(value.transactionLength, 281u);
}

TEST(GtidEventCodecTest, RejectsBodyShorterThanTheFixedPrefix) {
  std::vector<std::uint8_t> body{0x01};
  body.insert(body.end(), SAMPLE_UUID_BYTES.begin(), SAMPLE_UUID_BYTES.end());
  AppendLittleEndian(body, 22934, 8);
  ASSERT_EQ(body.size(), 25u);
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::Parse(body, 0, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidEventCodecTest, RejectsALogicalClockTruncatedBeforeSequenceNumber) {
  std::vector<std::uint8_t> body{0x01};
  body.insert(body.end(), SAMPLE_UUID_BYTES.begin(), SAMPLE_UUID_BYTES.end());
  AppendLittleEndian(body, 22934, 8);
  body.push_back(0x02);
  AppendLittleEndian(body, 12345, 8);
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::Parse(body, 0, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidEventCodecTest,
     RejectsAnOriginalTimestampThatTheBitPromisesButTheBodyLacks) {
  auto body = BuildBody(22934, /*withLogicalClock=*/true,
                        /*originalTimestampDiffers=*/true, 281);
  body.resize(1 + 16 + 8 + 1 + 8 + 8 + 7);
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::Parse(body, 0, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidEventCodecTest,
     TreatsTrailingChecksumAsChecksumWhenTransactionLengthIsAbsent) {
  auto body =
      BuildBody(22934, /*withLogicalClock=*/true,
                /*originalTimestampDiffers=*/false, /*transactionLength=*/0);
  constexpr std::size_t THROUGH_IMMEDIATE_TIMESTAMP =
      1 + 16 + 8 + 1 + 8 + 8 + 7;
  body.resize(THROUGH_IMMEDIATE_TIMESTAMP);
  body.insert(body.end(), {0xDE, 0xAD, 0xBE, 0xEF});
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 4, value, error)) << error;
  EXPECT_FALSE(value.hasTransactionLength);
}

TEST(GtidEventCodecTest, AcceptsALogicalClockWithoutCommitTimestamps) {
  // No commit timestamps means no transaction_length attempt: still a success
  // with a trailing checksum.
  auto body =
      BuildBody(22934, /*withLogicalClock=*/true,
                /*originalTimestampDiffers=*/false, /*transactionLength=*/0);
  constexpr std::size_t THROUGH_LOGICAL_CLOCK = 1 + 16 + 8 + 1 + 8 + 8;
  body.resize(THROUGH_LOGICAL_CLOCK);
  body.insert(body.end(), {0xDE, 0xAD, 0xBE, 0xEF});
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 4, value, error)) << error;
  EXPECT_FALSE(value.hasTransactionLength);
}

TEST(GtidEventCodecTest, RejectsATruncatedTransactionLengthLenenc) {
  auto body = BuildBody(22934, /*withLogicalClock=*/true,
                        /*originalTimestampDiffers=*/false, 70000);
  constexpr std::size_t PREFIX_THROUGH_TIMESTAMP = 1 + 16 + 8 + 1 + 8 + 8 + 7;
  ASSERT_EQ(body.size(), PREFIX_THROUGH_TIMESTAMP + 9);
  body.resize(body.size() - 8);
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::Parse(body, 0, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidEventCodecTest,
     RealGtidEventTransactionLengthMatchesItsGroupsEventLengthSum) {
  const std::vector<std::uint8_t> body{
      0x01, 0xdb, 0x06, 0xab, 0x0a, 0x39, 0xaa, 0x11, 0xf1, 0x90, 0xc3, 0x19,
      0x59, 0x73, 0x35, 0xb2, 0xdb, 0x96, 0x59, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdb, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa1, 0x1d, 0x4b, 0x70, 0x93, 0x5b,
      0x06, 0xfc, 0x19, 0x01, 0x1b, 0x3a, 0x01, 0x00, 0x7f, 0xe0, 0x45, 0xf1};
  const std::array<std::uint8_t, 16> expectedUuid{
      0xdb, 0x06, 0xab, 0x0a, 0x39, 0xaa, 0x11, 0xf1,
      0x90, 0xc3, 0x19, 0x59, 0x73, 0x35, 0xb2, 0xdb};
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::Parse(body, 4, value, error)) << error;
  EXPECT_EQ(value.uuid.bytes, expectedUuid);
  EXPECT_EQ(value.gno, 22934);
  ASSERT_TRUE(value.hasTransactionLength);
  constexpr std::uint64_t GTID_EVENT_LENGTH = 79;
  constexpr std::uint64_t QUERY_EVENT_LENGTH = 202;
  EXPECT_EQ(value.transactionLength, GTID_EVENT_LENGTH + QUERY_EVENT_LENGTH);
}

std::vector<std::uint8_t> Varint(std::uint64_t value) {
  std::size_t byteCount = 1;
  while (byteCount < 9 && (value >> (7 * byteCount)) != 0) ++byteCount;
  std::vector<std::uint8_t> out;
  if (byteCount == 9) {
    out.push_back(0xFF);
    AppendLittleEndian(out, value, 8);
    return out;
  }
  out.push_back(static_cast<std::uint8_t>(((1u << (byteCount - 1)) - 1) |
                                          (value << byteCount)));
  AppendLittleEndian(out, value >> (8 - byteCount), byteCount - 1);
  return out;
}

std::vector<std::uint8_t> SignedVarint(std::int64_t value) {
  const std::uint64_t encoded =
      value < 0 ? (static_cast<std::uint64_t>(-(value + 1)) << 1) | 1
                : static_cast<std::uint64_t>(value) << 1;
  return Varint(encoded);
}

void Append(std::vector<std::uint8_t> &out,
            const std::vector<std::uint8_t> &bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

struct TaggedBodyOptions {
  std::string tag = "test_tag";
  std::int64_t gno = 1;
  bool withOriginalCommitTimestamp = false;
  bool withTransactionLength = true;
  std::uint64_t transactionLength = 337;
};

std::vector<std::uint8_t> BuildTaggedBody(const TaggedBodyOptions &options) {
  std::vector<std::uint8_t> fields;
  Append(fields, Varint(0));
  Append(fields, Varint(1));
  Append(fields, Varint(1));
  for (const auto byte : SAMPLE_UUID_BYTES) Append(fields, Varint(byte));
  Append(fields, Varint(2));
  Append(fields, SignedVarint(options.gno));
  Append(fields, Varint(3));
  Append(fields, Varint(options.tag.size()));
  fields.insert(fields.end(), options.tag.begin(), options.tag.end());
  Append(fields, Varint(4));
  Append(fields, SignedVarint(0));
  Append(fields, Varint(5));
  Append(fields, SignedVarint(1));
  Append(fields, Varint(6));
  Append(fields, Varint(1789979169209856));
  if (options.withOriginalCommitTimestamp) {
    Append(fields, Varint(7));
    Append(fields, Varint(1789979169000000));
  }
  if (options.withTransactionLength) {
    Append(fields, Varint(8));
    Append(fields, Varint(options.transactionLength));
  }
  Append(fields, Varint(9));
  Append(fields, Varint(80411));

  std::vector<std::uint8_t> body;
  Append(body, Varint(1));
  const std::size_t sizeBytes = Varint(fields.size() + 3).size();
  Append(body, Varint(fields.size() + 2 + sizeBytes));
  Append(body, Varint(0));
  Append(body, fields);
  return body;
}

const std::vector<std::uint8_t> REAL_TAGGED_BODY_WITH_CRC{
    0x02, 0x7c, 0x00, 0x00, 0x02, 0x02, 0xee, 0x45, 0x03, 0x41, 0x02,
    0x58, 0x02, 0x69, 0x03, 0x22, 0xc5, 0x03, 0x69, 0x02, 0x82, 0x41,
    0x02, 0x35, 0x02, 0xdc, 0xbe, 0xdc, 0x35, 0x02, 0x04, 0x04, 0x06,
    0x10, 0x74, 0x65, 0x73, 0x74, 0x5f, 0x74, 0x61, 0x67, 0x08, 0x00,
    0x0a, 0x04, 0x0c, 0x7f, 0x00, 0x3e, 0x89, 0x00, 0xfa, 0x5b, 0x06,
    0x10, 0x45, 0x05, 0x12, 0xdb, 0xd0, 0x09, 0xe7, 0x3e, 0xac, 0x47};
const std::array<std::uint8_t, 16> REAL_SOURCE_UUID_BYTES{
    0x77, 0xd1, 0x90, 0x2c, 0x01, 0xda, 0x11, 0xf1,
    0x9a, 0x41, 0x90, 0x8d, 0x6e, 0x5f, 0x6e, 0x8d};

TEST(GtidEventCodecTest, ParsesARealTaggedEventFromPerconaServer8411) {
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(
      GtidEventCodec::ParseTagged(REAL_TAGGED_BODY_WITH_CRC, 4, value, error))
      << error;
  EXPECT_EQ(value.uuid.bytes, REAL_SOURCE_UUID_BYTES);
  EXPECT_EQ(value.tag, "test_tag");
  EXPECT_EQ(value.gno, 1);
  ASSERT_TRUE(value.hasTransactionLength);
  EXPECT_EQ(value.transactionLength, 337u);
}

TEST(GtidEventCodecTest,
     ParsesASyntheticTaggedBodyWithEveryOptionalFieldPresent) {
  TaggedBodyOptions options;
  options.gno = 1234567;
  options.tag = "primary";
  options.withOriginalCommitTimestamp = true;
  options.transactionLength = 100000;
  const auto body = BuildTaggedBody(options);
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::ParseTagged(body, 0, value, error)) << error;
  EXPECT_EQ(value.uuid.bytes, SAMPLE_UUID_BYTES);
  EXPECT_EQ(value.tag, "primary");
  EXPECT_EQ(value.gno, 1234567);
  ASSERT_TRUE(value.hasTransactionLength);
  EXPECT_EQ(value.transactionLength, 100000u);
}

TEST(GtidEventCodecTest, ATaggedBodyWithoutTransactionLengthLeavesItUnset) {
  TaggedBodyOptions options;
  options.withTransactionLength = false;
  const auto body = BuildTaggedBody(options);
  GtidEvent value;
  std::string error;
  ASSERT_TRUE(GtidEventCodec::ParseTagged(body, 0, value, error)) << error;
  EXPECT_EQ(value.tag, "test_tag");
  EXPECT_FALSE(value.hasTransactionLength);
  EXPECT_EQ(value.transactionLength, 0u);
}

TEST(GtidEventCodecTest, TheSyntheticEncoderMatchesTheRealBytes) {
  TaggedBodyOptions options;
  auto body = BuildTaggedBody(options);
  std::vector<std::uint8_t> expected(REAL_TAGGED_BODY_WITH_CRC.begin(),
                                     REAL_TAGGED_BODY_WITH_CRC.end() - 4);
  std::vector<std::uint8_t> realUuidFields;
  for (const auto byte : REAL_SOURCE_UUID_BYTES)
    Append(realUuidFields, Varint(byte));
  std::vector<std::uint8_t> sampleUuidFields;
  for (const auto byte : SAMPLE_UUID_BYTES)
    Append(sampleUuidFields, Varint(byte));
  const auto at = std::search(body.begin(), body.end(),
                              sampleUuidFields.begin(), sampleUuidFields.end());
  ASSERT_NE(at, body.end());
  body.erase(at, at + static_cast<std::ptrdiff_t>(sampleUuidFields.size()));
  body.insert(at, realUuidFields.begin(), realUuidFields.end());
  // The size byte (index 1) is excluded: the uuid swap changes the message
  // size.
  EXPECT_EQ(std::vector<std::uint8_t>(body.begin() + 3, body.end()),
            std::vector<std::uint8_t>(expected.begin() + 3, expected.end()));
  EXPECT_EQ(body[0], expected[0]);
  EXPECT_EQ(body[2], expected[2]);
}

TEST(GtidEventCodecTest, RefusesATaggedBodyWhoseTagIsLongerThanThirtyTwo) {
  TaggedBodyOptions options;
  options.tag = std::string(33, 'x');
  const auto body = BuildTaggedBody(options);
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::ParseTagged(body, 0, value, error));
  EXPECT_NE(error.find("tag"), std::string::npos) << error;
}

TEST(GtidEventCodecTest, RefusesATaggedBodyMissingItsUuidGnoOrTag) {
  std::vector<std::uint8_t> body;
  Append(body, Varint(1));
  Append(body, Varint(5));
  Append(body, Varint(0));
  Append(body, Varint(4));
  Append(body, SignedVarint(0));
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::ParseTagged(body, 0, value, error));
  EXPECT_NE(error.find("uuid"), std::string::npos) << error;
}

TEST(GtidEventCodecTest,
     RefusesATaggedBodyThatIsTruncatedOrHasAnotherFormatVersion) {
  const auto whole = BuildTaggedBody(TaggedBodyOptions{});
  GtidEvent value;
  std::string error;
  for (std::size_t length = 0; length < whole.size(); ++length) {
    const std::vector<std::uint8_t> cut(
        whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
    // Stop before field 9: cutting after transaction_length is a shorter
    // message, not a truncation.
    if (length >= whole.size() - 4) break;
    EXPECT_FALSE(GtidEventCodec::ParseTagged(cut, 0, value, error))
        << "length " << length;
  }
  auto otherVersion = whole;
  otherVersion[0] = 0x04;
  EXPECT_FALSE(GtidEventCodec::ParseTagged(otherVersion, 0, value, error));
  EXPECT_NE(error.find("header"), std::string::npos) << error;

  EXPECT_FALSE(
      GtidEventCodec::ParseTagged(whole, whole.size() + 1, value, error));
}

TEST(GtidEventCodecTest, RefusesATaggedBodyWhoseFieldIdsDoNotAscend) {
  std::vector<std::uint8_t> fields;
  Append(fields, Varint(1));
  for (const auto byte : SAMPLE_UUID_BYTES) Append(fields, Varint(byte));
  Append(fields, Varint(0));
  Append(fields, Varint(1));
  std::vector<std::uint8_t> body;
  Append(body, Varint(1));
  Append(body, Varint(fields.size() + 3));
  Append(body, Varint(0));
  Append(body, fields);
  GtidEvent value;
  std::string error;
  EXPECT_FALSE(GtidEventCodec::ParseTagged(body, 0, value, error));
  EXPECT_NE(error.find("out of order"), std::string::npos) << error;
}
}  // namespace
}  // namespace binlog_streamer
