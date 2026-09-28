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

#include "binlog/cFormatDescriptionEventCodec.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace binlog_streamer {
namespace {

// created=0x0 below is not a capture error - it's only ever nonzero on the
// FDE written at server startup.
const std::vector<std::uint8_t> REAL_FDE_BODY{
    0x04, 0x00, 0x38, 0x2e, 0x34, 0x2e, 0x31, 0x31, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x0d, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x63,
    0x00, 0x04, 0x1a, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x0a, 0x0a, 0x0a, 0x2a, 0x2a, 0x00, 0x12, 0x34, 0x00, 0x0a,
    0x28, 0x00, 0x00, 0x01, 0xf4, 0xab, 0xb8, 0xe5,
};

TEST(FormatDescriptionEventCodecTest, ParsesARealCrc32FormatDescriptionEvent) {
  FormatDescriptionEvent value;
  std::string error;
  ASSERT_TRUE(FormatDescriptionEventCodec::Parse(REAL_FDE_BODY, value, error))
      << error;
  EXPECT_EQ(value.binlogVersion, 4u);
  EXPECT_EQ(value.serverVersion, "8.4.11");
  EXPECT_EQ(value.created, 0u);  // see REAL_FDE_BODY comment
  EXPECT_EQ(value.commonHeaderLength, 19u);
  // The codec derives "has a checksum trailer" from serverVersion, not
  // from the algorithm byte.
  EXPECT_EQ(value.checksumAlgorithm, "CRC32");
}

// 57-byte fixed prefix only; post_header_len[] and any checksum trailer
// are each test's own job to append.
std::vector<std::uint8_t> BuildFixedPrefix(std::uint16_t binlogVersion,
                                           const std::string &serverVersion,
                                           std::uint32_t created,
                                           std::uint8_t commonHeaderLength) {
  std::vector<std::uint8_t> body(57, 0x00);
  body[0] = static_cast<std::uint8_t>(binlogVersion);
  body[1] = static_cast<std::uint8_t>(binlogVersion >> 8);
  for (std::size_t i = 0; i < serverVersion.size() && i < 50; ++i)
    body[2 + i] = static_cast<std::uint8_t>(serverVersion[i]);
  for (int i = 0; i < 4; ++i)
    body[52 + i] = static_cast<std::uint8_t>(created >> (8 * i));
  body[56] = commonHeaderLength;
  return body;
}

TEST(FormatDescriptionEventCodecTest,
     ParsesAPreChecksumServerVersionWithoutAChecksumTrailer) {
  const auto body = BuildFixedPrefix(3, "5.5.62", 1700000000, 19);
  FormatDescriptionEvent value;
  std::string error;
  ASSERT_TRUE(FormatDescriptionEventCodec::Parse(body, value, error)) << error;
  EXPECT_EQ(value.binlogVersion, 3u);
  EXPECT_EQ(value.serverVersion, "5.5.62");
  EXPECT_EQ(value.created, 1700000000u);
  EXPECT_EQ(value.commonHeaderLength, 19u);
  EXPECT_EQ(value.checksumAlgorithm, "UNDEF");
}

TEST(FormatDescriptionEventCodecTest, RejectsBodyShorterThanTheFixedPrefix) {
  const std::vector<std::uint8_t> body(56, 0x00);
  FormatDescriptionEvent value;
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::Parse(body, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(FormatDescriptionEventCodecTest,
     RejectsAChecksumTrailerImpliedByVersionThatDoesNotFit) {
  // 8.4.11 implies a checksum trailer (>= 5.6.1), but the body ends right
  // at the fixed prefix.
  const auto body = BuildFixedPrefix(4, "8.4.11", 0, 19);
  ASSERT_EQ(body.size(), 57u);
  FormatDescriptionEvent value;
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::Parse(body, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(FormatDescriptionEventCodecTest,
     RejectsACommonHeaderLengthShorterThanTheCommonHeaderItself) {
  const auto body = BuildFixedPrefix(4, "5.5.62", 0, 18);
  FormatDescriptionEvent value;
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::Parse(body, value, error));
  EXPECT_FALSE(error.empty());
}

TEST(FormatDescriptionEventCodecTest, TreatsOffAsADistinctAlgorithmFromUndef) {
  auto body = BuildFixedPrefix(4, "8.0.36", 0, 19);
  body.push_back(0x00);
  // The source always writes checksum room, even for algorithm OFF.
  for (int i = 0; i < 4; ++i) body.push_back(0x00);
  FormatDescriptionEvent value;
  std::string error;
  ASSERT_TRUE(FormatDescriptionEventCodec::Parse(body, value, error)) << error;
  EXPECT_EQ(value.checksumAlgorithm, "OFF");
}

// The 5.6.1 boundary itself: one version below it must not expect a
// trailer, the version at it must.
TEST(FormatDescriptionEventCodecTest, TreatsVersion560AsPreChecksum) {
  const auto body = BuildFixedPrefix(4, "5.6.0", 0, 19);
  FormatDescriptionEvent value;
  std::string error;
  ASSERT_TRUE(FormatDescriptionEventCodec::Parse(body, value, error)) << error;
  EXPECT_EQ(value.checksumAlgorithm, "UNDEF");
}

TEST(FormatDescriptionEventCodecTest, TreatsVersion561AsChecksumAware) {
  auto body = BuildFixedPrefix(4, "5.6.1", 0, 19);
  body.push_back(0x01);
  for (int i = 0; i < 4; ++i) body.push_back(0xAA);
  FormatDescriptionEvent value;
  std::string error;
  ASSERT_TRUE(FormatDescriptionEventCodec::Parse(body, value, error)) << error;
  EXPECT_EQ(value.checksumAlgorithm, "CRC32");
}

std::vector<std::uint8_t> BuildWholeEvent(std::uint16_t flags,
                                          std::vector<std::uint8_t> body) {
  std::vector<std::uint8_t> event(19, 0x00);
  event[4] = 15;
  event[17] = static_cast<std::uint8_t>(flags);
  event[18] = static_cast<std::uint8_t>(flags >> 8);
  event.insert(event.end(), body.begin(), body.end());
  return event;
}

TEST(FormatDescriptionEventCodecTest,
     EqualForResumeAcceptsTwoByteIdenticalEvents) {
  const auto body = BuildFixedPrefix(4, "8.4.11", 0, 19);
  const auto event = BuildWholeEvent(0, body);
  std::string error;
  EXPECT_TRUE(FormatDescriptionEventCodec::EqualForResume(
      event, event, /*checksumLength=*/0, error))
      << error;
}

// The "file in use" bit is this relay's own byte (cBinlogFileWriter.hpp's
// Create()), not the source's - ignored regardless of which side sets it.
TEST(FormatDescriptionEventCodecTest, EqualForResumeIgnoresOnlyTheInUseBit) {
  const auto body = BuildFixedPrefix(4, "8.4.11", 0, 19);
  const auto stored = BuildWholeEvent(0x01, body);
  const auto incoming = BuildWholeEvent(0x00, body);
  std::string error;
  EXPECT_TRUE(FormatDescriptionEventCodec::EqualForResume(
      stored, incoming, /*checksumLength=*/0, error))
      << error;
}

// Every *other* flags bit still has to match - only bit 0 (IN_USE) is
// excluded, not the whole byte, and not the high flags byte either.
TEST(FormatDescriptionEventCodecTest,
     EqualForResumeRejectsADifferenceInAnyOtherFlagsBit) {
  const auto body = BuildFixedPrefix(4, "8.4.11", 0, 19);
  const auto stored = BuildWholeEvent(0x00, body);
  const auto incoming = BuildWholeEvent(0x02, body);
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::EqualForResume(
      stored, incoming, /*checksumLength=*/0, error));
  EXPECT_FALSE(error.empty());
}

TEST(FormatDescriptionEventCodecTest,
     EqualForResumeRejectsADifferenceInTheHighFlagsByte) {
  const auto body = BuildFixedPrefix(4, "8.4.11", 0, 19);
  auto incomingEvent = BuildWholeEvent(0x00, body);
  incomingEvent[18] = 0x01;
  const auto stored = BuildWholeEvent(0x00, body);
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::EqualForResume(
      stored, incomingEvent, /*checksumLength=*/0, error));
}

TEST(FormatDescriptionEventCodecTest,
     EqualForResumeIgnoresOnlyTheCreatedField) {
  const auto stored =
      BuildWholeEvent(0x00, BuildFixedPrefix(4, "8.4.11", 1700000000, 19));
  // created zeroed, as on resume.
  const auto incoming =
      BuildWholeEvent(0x00, BuildFixedPrefix(4, "8.4.11", 0, 19));
  std::string error;
  EXPECT_TRUE(FormatDescriptionEventCodec::EqualForResume(
      stored, incoming, /*checksumLength=*/0, error))
      << error;
}

TEST(FormatDescriptionEventCodecTest,
     EqualForResumeRejectsADifferenceInAnyOtherBodyByte) {
  const auto stored =
      BuildWholeEvent(0x00, BuildFixedPrefix(4, "8.4.11", 0, 19));
  const auto incoming =
      BuildWholeEvent(0x00, BuildFixedPrefix(4, "8.4.12", 0, 19));
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::EqualForResume(
      stored, incoming, /*checksumLength=*/0, error));
  EXPECT_FALSE(error.empty());
}

// Trailing CRC is excluded too (computed over `created`, so it legitimately
// differs) - but only exactly checksumLength bytes at the end, so a
// body-byte difference right before the trailer is still caught.
TEST(FormatDescriptionEventCodecTest,
     EqualForResumeIgnoresOnlyTheTrailingChecksumNotBodyBytesBeforeIt) {
  auto storedBody = BuildFixedPrefix(4, "8.4.11", 1700000000, 19);
  storedBody.push_back(0x01);
  // A plausible (not recomputed) CRC room.
  storedBody.insert(storedBody.end(), 4, 0xAA);
  auto incomingBody = BuildFixedPrefix(4, "8.4.11", 0, 19);
  incomingBody.push_back(0x01);
  incomingBody.insert(incomingBody.end(), 4, 0xBB);
  const auto stored = BuildWholeEvent(0x00, storedBody);
  const auto incoming = BuildWholeEvent(0x00, incomingBody);
  std::string error;
  EXPECT_TRUE(FormatDescriptionEventCodec::EqualForResume(
      stored, incoming, /*checksumLength=*/4, error))
      << error;

  // Also differ right before the checksum room - not excluded, must still
  // be caught. .at() not operator[]: g++ 14.2 -O2 flags operator[] here
  // with a false -Warray-bounds despite the copy holding 62 bytes.
  auto incomingBodyWithBadDescriptor = incomingBody;
  incomingBodyWithBadDescriptor.at(incomingBodyWithBadDescriptor.size() - 5) =
      0x00;
  const auto incomingWithBadDescriptor =
      BuildWholeEvent(0x00, incomingBodyWithBadDescriptor);
  EXPECT_FALSE(FormatDescriptionEventCodec::EqualForResume(
      stored, incomingWithBadDescriptor, /*checksumLength=*/4, error));
}

TEST(FormatDescriptionEventCodecTest,
     EqualForResumeRejectsEventsOfDifferentLength) {
  const auto stored =
      BuildWholeEvent(0x00, BuildFixedPrefix(4, "8.4.11", 0, 19));
  auto incoming = stored;
  incoming.push_back(0x00);
  std::string error;
  EXPECT_FALSE(FormatDescriptionEventCodec::EqualForResume(
      stored, incoming, /*checksumLength=*/0, error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace binlog_streamer
