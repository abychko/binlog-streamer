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

#include "gtid/cGtidSet.hpp"

#include <gtest/gtest.h>
#include <limits>
#include "cUuidText.hpp"
#include "gtid/sGtidInterval.hpp"
#include "gtid/sGtidSource.hpp"
#include "gtid/sUuid.hpp"

namespace binlog_streamer {
namespace {

// Independent of GtidSet's own WriteUint64LE(), so this doesn't just echo
// the production code.
std::uint64_t ReadUint64LE(const std::vector<std::uint8_t> &bytes,
                           std::size_t pos) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
    value |=
        static_cast<std::uint64_t>(bytes[pos + static_cast<std::size_t>(i)])
        << (8 * i);
  return value;
}

Uuid MakeUuid(std::uint8_t firstByte) {
  Uuid uuid;
  for (std::size_t i = 0; i < uuid.bytes.size(); ++i)
    uuid.bytes[i] = static_cast<std::uint8_t>(firstByte + i);
  return uuid;
}

// Matches WriteUint64LE() in production code, kept separate for the same
// reason as ReadUint64LE() above.
void AppendUint64LE(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void AppendUuidBytes(std::vector<std::uint8_t> &out, const Uuid &uuid) {
  out.insert(out.end(), uuid.bytes.begin(), uuid.bytes.end());
}

// Matches Encode()'s tagged-format header layout for one source - used to
// hand-build inputs Encode() itself would never produce (out-of-order
// intervals, an invalid tag).
constexpr std::uint64_t ONE_TAGGED_SOURCE_HEADER =
    (std::uint64_t{1} << 56) | (std::uint64_t{1} << 8) | std::uint64_t{1};

TEST(GtidSetTest, TextRoundTripsThroughAddAndToText) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(
      set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(set.ToText(), "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5");
}

TEST(GtidSetTest, SingleGtidPrintsWithoutDash) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:7", error));
  EXPECT_EQ(set.ToText(), "3e11fa47-71ca-11e1-9e33-c80aa9429562:7");
}

TEST(GtidSetTest, MultipleIntervalsForOneSourcePrintInOrder) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(
      set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-3:10-12", error));
  EXPECT_EQ(set.ToText(), "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-3:10-12");
}

TEST(GtidSetTest, MultipleSourcesSeparatedByCommaAndNewline) {
  // ",\n" is the server's default_string_format separator
  // (sql/rpl_gtid_set.cc).
  GtidSet set;
  std::string error;
  ASSERT_TRUE(
      set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5,5f069f6f-71ca-"
                      "11e1-9e33-c80aa9429562:1-3",
                      error));
  EXPECT_EQ(set.ToText(),
            "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5,\n5f069f6f-71ca-11e1-"
            "9e33-c80aa9429562:1-3");
}

TEST(GtidSetTest, AddFromTextAcceptsCommaNewlineSeparator) {
  // Whitespace is only skipped right after a comma (sql/rpl_gtid_set.cc),
  // so this checks exactly what ToText() emits, not tolerance in general.
  const std::string text =
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1,\n5f069f6f-71ca-11e1-9e33-"
      "c80aa9429562:2";
  GtidSet set;
  std::string error;
  ASSERT_TRUE(set.AddFromText(text, error)) << error;
  EXPECT_EQ(set.ToText(), text);
}

TEST(GtidSetTest, TaggedGtidRoundTrips) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(set.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:primary:1-5", error));
  EXPECT_EQ(set.ToText(), "3e11fa47-71ca-11e1-9e33-c80aa9429562:primary:1-5");
}

TEST(GtidSetTest, OverlappingIntervalsMerge) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(set.AddInterval(source, 1, 10));
  ASSERT_TRUE(set.AddInterval(source, 5, 15));
  const auto intervals = set.GetIntervals(source);
  ASSERT_EQ(intervals.size(), 1u);
  EXPECT_EQ(intervals[0], (GtidInterval{1, 15}));
}

TEST(GtidSetTest, AdjacentIntervalsMerge) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(set.AddInterval(source, 1, 5));
  ASSERT_TRUE(set.AddInterval(source, 5, 10));
  const auto intervals = set.GetIntervals(source);
  ASSERT_EQ(intervals.size(), 1u);
  EXPECT_EQ(intervals[0], (GtidInterval{1, 10}));
}

TEST(GtidSetTest, DisjointIntervalsStaySeparate) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(set.AddInterval(source, 1, 5));
  ASSERT_TRUE(set.AddInterval(source, 10, 15));
  const auto intervals = set.GetIntervals(source);
  ASSERT_EQ(intervals.size(), 2u);
  EXPECT_EQ(intervals[0], (GtidInterval{1, 5}));
  EXPECT_EQ(intervals[1], (GtidInterval{10, 15}));
}

TEST(GtidSetTest, ContainsOnlyWhatAnIntervalOfItsOwnSourceCovers) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(set.AddInterval(source, 1, 5));
  ASSERT_TRUE(set.AddInterval(source, 10, 15));
  for (const std::int64_t gno : {1, 4, 10, 14})
    EXPECT_TRUE(set.Contains(source, gno)) << gno;
  for (const std::int64_t gno : {0, 5, 9, 15, 100})
    EXPECT_FALSE(set.Contains(source, gno)) << gno;
  EXPECT_FALSE(set.Contains(GtidSource{MakeUuid(1), "tag"}, 1));
  EXPECT_FALSE(set.Contains(GtidSource{MakeUuid(2), ""}, 1));
}

TEST(GtidSetTest, AddIntervalRejectsEndNotAfterStart) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), ""};
  EXPECT_FALSE(set.AddInterval(source, 10, 5));
  EXPECT_FALSE(set.AddInterval(source, 5, 5));
  EXPECT_TRUE(set.GetIntervals(source).empty());
}

TEST(GtidSetTest, AddIntervalRejectsTagLongerThanEncodable) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), std::string(33, 'a')};
  EXPECT_FALSE(set.AddInterval(source, 1, 2));
  EXPECT_TRUE(set.GetIntervals(source).empty());
}

TEST(GtidSetTest, EncodeUntaggedLayoutMatchesServerFormat) {
  GtidSet set;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(set.AddInterval(source, 5, 10));

  const std::vector<std::uint8_t> encoded =
      set.Encode(/*skipTaggedGtids=*/true);
  ASSERT_EQ(encoded.size(), set.GetEncodedLength(true));
  ASSERT_EQ(encoded.size(),
            48u);  // 8 header + 16 uuid + 8 n_intervals + 16 interval

  EXPECT_EQ(ReadUint64LE(encoded, 0),
            1u);  // 1 source, untagged format byte is 0
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(encoded[8 + i], source.uuid.bytes[i]);
  EXPECT_EQ(ReadUint64LE(encoded, 24), 1u);  // n_intervals
  EXPECT_EQ(ReadUint64LE(encoded, 32), 5u);  // start
  EXPECT_EQ(ReadUint64LE(encoded, 40),
            10u);  // end - distinct from start, catches a swap
}

TEST(GtidSetTest, EncodeTaggedLayoutMatchesServerFormat) {
  GtidSet set;
  const GtidSource source{MakeUuid(0xA1), "primary"};
  ASSERT_TRUE(set.AddInterval(source, 1, 2));

  const std::vector<std::uint8_t> encoded =
      set.Encode(/*skipTaggedGtids=*/false);
  ASSERT_EQ(encoded.size(), set.GetEncodedLength(false));
  ASSERT_EQ(
      encoded.size(),
      56u);  // 8 header + 16 uuid + 8 tag(1+7) + 8 n_intervals + 16 interval

  // 1 source, tagged format (format byte in both low and high byte).
  const std::uint64_t expectedHeader =
      (std::uint64_t{1} << 56) | (std::uint64_t{1} << 8) | std::uint64_t{1};
  EXPECT_EQ(ReadUint64LE(encoded, 0), expectedHeader);

  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(encoded[8 + i], source.uuid.bytes[i]);

  EXPECT_EQ(encoded[24],
            7u << 1);  // tag length prefix: length 7, single-byte varint form
  const std::string tagBytes(reinterpret_cast<const char *>(&encoded[25]), 7);
  EXPECT_EQ(tagBytes, "primary");

  EXPECT_EQ(ReadUint64LE(encoded, 32), 1u);  // n_intervals
  EXPECT_EQ(ReadUint64LE(encoded, 40), 1u);  // start
  EXPECT_EQ(ReadUint64LE(encoded, 48), 2u);  // end
}

TEST(GtidSetTest, SkipTaggedGtidsExcludesTaggedSourceEntirely) {
  GtidSet set;
  const GtidSource untagged{MakeUuid(1), ""};
  const GtidSource tagged{MakeUuid(2), "primary"};
  ASSERT_TRUE(set.AddInterval(untagged, 1, 5));
  ASSERT_TRUE(set.AddInterval(tagged, 1, 5));

  const std::vector<std::uint8_t> encoded =
      set.Encode(/*skipTaggedGtids=*/true);
  // Only the untagged source is encoded: header n_sids == 1, not 2.
  EXPECT_EQ(ReadUint64LE(encoded, 0), 1u);
  ASSERT_EQ(encoded.size(), 48u);
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(encoded[8 + i], untagged.uuid.bytes[i]);
}

TEST(GtidSetTest, AddFromEncodingRoundTripsEmptySet) {
  GtidSet original;
  const std::vector<std::uint8_t> encoded =
      original.Encode(/*skipTaggedGtids=*/true);

  GtidSet decoded;
  std::string error;
  ASSERT_TRUE(decoded.AddFromEncoding(encoded, error)) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(decoded.IsEmpty());
}

TEST(GtidSetTest, AddFromEncodingRoundTripsSingleSource) {
  GtidSet original;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(original.AddInterval(source, 5, 10));
  const std::vector<std::uint8_t> encoded =
      original.Encode(/*skipTaggedGtids=*/true);

  GtidSet decoded;
  std::string error;
  ASSERT_TRUE(decoded.AddFromEncoding(encoded, error)) << error;
  EXPECT_EQ(decoded.GetIntervals(source), original.GetIntervals(source));
}

TEST(GtidSetTest, AddFromEncodingRoundTripsMultipleSources) {
  GtidSet original;
  const GtidSource first{MakeUuid(1), ""};
  const GtidSource second{MakeUuid(2), ""};
  ASSERT_TRUE(original.AddInterval(first, 1, 5));
  ASSERT_TRUE(original.AddInterval(first, 10, 15));
  ASSERT_TRUE(original.AddInterval(second, 100, 101));
  const std::vector<std::uint8_t> encoded =
      original.Encode(/*skipTaggedGtids=*/true);

  GtidSet decoded;
  std::string error;
  ASSERT_TRUE(decoded.AddFromEncoding(encoded, error)) << error;
  EXPECT_EQ(decoded.GetIntervals(first), original.GetIntervals(first));
  EXPECT_EQ(decoded.GetIntervals(second), original.GetIntervals(second));
}

TEST(GtidSetTest, AddFromEncodingRoundTripsTaggedSource) {
  GtidSet original;
  const GtidSource untagged{MakeUuid(1), ""};
  const GtidSource tagged{MakeUuid(2), "primary"};
  ASSERT_TRUE(original.AddInterval(untagged, 1, 5));
  ASSERT_TRUE(original.AddInterval(tagged, 1, 3));
  ASSERT_TRUE(original.AddInterval(tagged, 10, 11));
  const std::vector<std::uint8_t> encoded =
      original.Encode(/*skipTaggedGtids=*/false);

  GtidSet decoded;
  std::string error;
  ASSERT_TRUE(decoded.AddFromEncoding(encoded, error)) << error;
  EXPECT_EQ(decoded.GetIntervals(untagged), original.GetIntervals(untagged));
  EXPECT_EQ(decoded.GetIntervals(tagged), original.GetIntervals(tagged));
  EXPECT_EQ(decoded.ToText(), original.ToText());
}

TEST(GtidSetTest, AddFromEncodingRejectsUnknownFormatByte) {
  std::vector<std::uint8_t> encoded(8, 0);
  encoded[7] = 2;  // top byte: neither 0 (untagged) nor 1 (tagged)
  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsTruncatedHeader) {
  const std::vector<std::uint8_t> encoded{1, 0, 0};  // header needs all 8 bytes
  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(decoded.IsEmpty());
}

TEST(GtidSetTest, AddFromEncodingRejectsTruncatedUuid) {
  std::vector<std::uint8_t> encoded(
      8 + 10, 0);  // header claims 1 source, only 10 of 16 UUID bytes follow
  encoded[0] = 1;
  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
}

TEST(GtidSetTest, AddFromEncodingRejectsTruncatedIntervalCount) {
  std::vector<std::uint8_t> encoded(
      8 + 16 + 4, 0);  // header + full UUID, only 4 of 8 interval-count bytes
  encoded[0] = 1;
  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
}

TEST(GtidSetTest, AddFromEncodingRejectsTruncatedIntervalData) {
  GtidSet original;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(original.AddInterval(source, 5, 10));
  std::vector<std::uint8_t> encoded = original.Encode(/*skipTaggedGtids=*/true);
  ASSERT_EQ(encoded.size(), 48u);
  encoded.pop_back();  // one byte short of the one declared interval

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
}

TEST(GtidSetTest, AddFromEncodingRejectsTrailingBytes) {
  GtidSet original;
  const GtidSource source{MakeUuid(1), ""};
  ASSERT_TRUE(original.AddInterval(source, 5, 10));
  std::vector<std::uint8_t> encoded = original.Encode(/*skipTaggedGtids=*/true);
  encoded.push_back(0);  // one byte more than a complete encoding

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
}

TEST(GtidSetTest, AddFromEncodingParsesRealPreviousGtidsEvent) {
  const std::vector<std::uint8_t> encoded{
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdb, 0x06, 0xab, 0x0a,
      0x39, 0xaa, 0x11, 0xf1, 0x90, 0xc3, 0x19, 0x59, 0x73, 0x35, 0xb2, 0xdb,
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xbc, 0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };

  GtidSet decoded;
  std::string error;
  ASSERT_TRUE(decoded.AddFromEncoding(encoded, error)) << error;
  EXPECT_EQ(decoded.ToText(), "db06ab0a-39aa-11f1-90c3-19597335b2db:1-22715");
}

TEST(GtidSetTest, AddFromEncodingRejectsOutOfOrderIntervals) {
  // Encode() would merge or reorder these, so build the bytes directly:
  // [10,20) then [5,30) - second start doesn't exceed first end, which
  // the "start <= last" check (mirrors add_gtid_encoding) exists to catch.
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, 1);
  AppendUuidBytes(encoded, MakeUuid(1));
  AppendUint64LE(encoded, 2);
  AppendUint64LE(encoded, 10);
  AppendUint64LE(encoded, 20);
  AppendUint64LE(encoded, 5);
  AppendUint64LE(encoded, 30);

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsIntervalWithEndNotAfterStart) {
  // [50,30): end doesn't exceed start. previousEnd starts at 0, so the
  // ordering check alone wouldn't catch this - only AddInterval()'s own
  // end<=start rejection does.
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, 1);
  AppendUuidBytes(encoded, MakeUuid(1));
  AppendUint64LE(encoded, 1);
  AppendUint64LE(encoded, 50);
  AppendUint64LE(encoded, 30);

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsTaggedTruncationAtTagLength) {
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, ONE_TAGGED_SOURCE_HEADER);
  AppendUuidBytes(encoded, MakeUuid(1));

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsTaggedTruncationInsideTag) {
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, ONE_TAGGED_SOURCE_HEADER);
  AppendUuidBytes(encoded, MakeUuid(1));
  encoded.push_back(std::uint8_t{10} << 1);
  encoded.push_back('a');
  encoded.push_back('b');
  encoded.push_back('c');

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsMultiByteTagLengthPrefix) {
  // Low bit set = the multi-byte varint form Encode() never writes. A
  // complete, well-formed source follows so a naive reading (0x03 as
  // length 1) would wrongly succeed instead of failing.
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, ONE_TAGGED_SOURCE_HEADER);
  AppendUuidBytes(encoded, MakeUuid(1));
  encoded.push_back(0x03);
  encoded.push_back('a');
  AppendUint64LE(encoded, 1);  // n_intervals=1
  AppendUint64LE(encoded, 1);  // start
  AppendUint64LE(encoded, 2);  // end

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsInvalidTagCharacters) {
  // "1bad" starts with a digit, invalid per TagText::Parse's grammar
  // ([a-zA-Z_][a-zA-Z0-9_]{0,31}). A complete source follows so ignoring
  // TagText::Parse's result would wrongly succeed as untagged.
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, ONE_TAGGED_SOURCE_HEADER);
  AppendUuidBytes(encoded, MakeUuid(1));
  encoded.push_back(std::uint8_t{4} << 1);
  encoded.push_back('1');
  encoded.push_back('b');
  encoded.push_back('a');
  encoded.push_back('d');
  AppendUint64LE(encoded, 1);  // n_intervals=1
  AppendUint64LE(encoded, 1);  // start
  AppendUint64LE(encoded, 2);  // end

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, AddFromEncodingRejectsHugeIntervalCountAgainstShortBuffer) {
  // n_intervals claims UINT64_MAX with no interval bytes following. The
  // bound check divides remaining bytes rather than multiplying
  // intervalCount up, so nothing overflows while rejecting it.
  std::vector<std::uint8_t> encoded;
  AppendUint64LE(encoded, 1);
  AppendUuidBytes(encoded, MakeUuid(1));
  AppendUint64LE(encoded, std::numeric_limits<std::uint64_t>::max());

  GtidSet decoded;
  std::string error;
  EXPECT_FALSE(decoded.AddFromEncoding(encoded, error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, IsEmptyReflectsContents) {
  GtidSet set;
  EXPECT_TRUE(set.IsEmpty());
  ASSERT_TRUE(set.AddInterval(GtidSource{MakeUuid(1), ""}, 1, 2));
  EXPECT_FALSE(set.IsEmpty());
}

TEST(GtidSetTest, RejectsMalformedUuid) {
  GtidSet set;
  std::string error;
  EXPECT_FALSE(set.AddFromText("not-a-uuid:1-5", error));
  EXPECT_FALSE(error.empty());
}

TEST(GtidSetTest, TagAfterIntervalSwitchesSourceForLaterIntervals) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(set.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:primary:1-3:10", error))
      << error;

  Uuid uuid;
  std::string uuidError;
  ASSERT_TRUE(
      UuidText::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562", uuid, uuidError))
      << uuidError;

  const auto untaggedIntervals = set.GetIntervals(GtidSource{uuid, ""});
  ASSERT_EQ(untaggedIntervals.size(), 1u);
  EXPECT_EQ(untaggedIntervals[0], (GtidInterval{1, 6}));

  const auto primaryIntervals = set.GetIntervals(GtidSource{uuid, "primary"});
  ASSERT_EQ(primaryIntervals.size(), 2u);
  EXPECT_EQ(primaryIntervals[0], (GtidInterval{1, 4}));
  EXPECT_EQ(primaryIntervals[1], (GtidInterval{10, 11}));

  EXPECT_EQ(set.ToText(),
            "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:primary:1-3:10");
}

TEST(GtidSetTest, RejectsIntervalEndBeforeStart) {
  GtidSet set;
  std::string error;
  EXPECT_FALSE(
      set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:10-5", error));
}

TEST(GtidSetTest, RejectsRangeEndAtGnoEnd) {
  // GNO_END (sql/rpl_gtid.h) is itself INT64_MAX; a valid GNO must be
  // strictly less than it.
  GtidSet set;
  std::string error;
  EXPECT_FALSE(set.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9223372036854775807", error));
  // The exact message proves the GNO parser itself rejected the value; a
  // later interval check would also fail, but only after an overflow.
  EXPECT_EQ(error, "expected a GTID number no smaller than the range start");
}

TEST(GtidSetTest, RejectsSingleGtidAtGnoEnd) {
  GtidSet set;
  std::string error;
  EXPECT_FALSE(set.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:9223372036854775807", error));
  EXPECT_EQ(error, "expected a positive GTID number");
}

TEST(GtidSetTest, AcceptsRangeEndOneBelowGnoEnd) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(set.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9223372036854775806", error))
      << error;
  Uuid uuid;
  std::string uuidError;
  ASSERT_TRUE(
      UuidText::Parse("3e11fa47-71ca-11e1-9e33-c80aa9429562", uuid, uuidError));
  const auto intervals = set.GetIntervals(GtidSource{uuid, ""});
  ASSERT_EQ(intervals.size(), 1u);
  EXPECT_EQ(intervals[0].start, 1);
  EXPECT_EQ(intervals[0].end,
            9223372036854775807LL);  // half-open: end = 9223372036854775806 + 1
}

TEST(GtidSetTest, EmptyTextIsValidAndEmpty) {
  GtidSet set;
  std::string error;
  EXPECT_TRUE(set.AddFromText("", error));
  EXPECT_TRUE(set.IsEmpty());
}

TEST(GtidSetTest, EmptySetIsSubsetOfAnything) {
  GtidSet empty;
  GtidSet other;
  std::string error;
  ASSERT_TRUE(
      other.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  EXPECT_TRUE(empty.IsSubsetOf(other));
  EXPECT_TRUE(empty.IsSubsetOf(empty));
}

TEST(GtidSetTest, NonEmptySetIsNotSubsetOfEmpty) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(
      set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  EXPECT_FALSE(set.IsSubsetOf(GtidSet{}));
}

TEST(GtidSetTest, IsSubsetOfTrueWhenFullyCoveredByOneWiderInterval) {
  GtidSet subset;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      subset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:3-5", error))
      << error;
  ASSERT_TRUE(
      superset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-10", error))
      << error;
  EXPECT_TRUE(subset.IsSubsetOf(superset));
  EXPECT_FALSE(superset.IsSubsetOf(
      subset));  // not symmetric: 1-10 is not covered by 3-5
}

TEST(GtidSetTest, IsSubsetOfFalseWhenAnIntervalRunsPastEveryCoveringInterval) {
  // superset has 1-5 and 8-10 (gap at 6-7); candidate's endpoints each
  // land inside one interval but the span between isn't covered.
  GtidSet candidate;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      candidate.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:5-8", error))
      << error;
  ASSERT_TRUE(superset.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:8-10", error))
      << error;
  EXPECT_FALSE(candidate.IsSubsetOf(superset));
}

TEST(GtidSetTest, IsSubsetOfFalseWhenSupersetIsMissingAWholeSource) {
  GtidSet subset;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      subset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5,5f069f6f-"
                         "71ca-11e1-9e33-c80aa9429562:1-3",
                         error))
      << error;
  ASSERT_TRUE(
      superset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  EXPECT_FALSE(subset.IsSubsetOf(superset));
}

TEST(GtidSetTest,
     IsSubsetOfDistinguishesTaggedFromUntaggedSourceWithTheSameUuid) {
  GtidSet subset;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(subset.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:primary:1-5", error))
      << error;
  ASSERT_TRUE(
      superset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  EXPECT_FALSE(subset.IsSubsetOf(superset));
}

TEST(GtidSetTest, IsSubsetOfTrueForEqualSets) {
  GtidSet set;
  std::string error;
  ASSERT_TRUE(
      set.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  EXPECT_TRUE(set.IsSubsetOf(set));
}

TEST(GtidSetTest, IsSubsetOfTrueAcrossTwoSourcesEachWithTwoIntervals) {
  // U is covered by two of other's intervals (1-5, 8-10); otherIndex must
  // advance past U's first interval, then reset to 0 for V's own - a
  // stale index carried over would wrongly fail V's covered interval.
  GtidSet subset;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      subset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-2:8-9,"
                         "5f069f6f-71ca-11e1-9e33-c80aa9429562:1-3",
                         error))
      << error;
  ASSERT_TRUE(
      superset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:8-10,"
                           "5f069f6f-71ca-11e1-9e33-c80aa9429562:1-3",
                           error))
      << error;
  EXPECT_TRUE(subset.IsSubsetOf(superset));
}

TEST(GtidSetTest, IsSubsetOfTrueWhenCoveringIntervalIsTheThirdOne) {
  // Covering interval (20-30) is other's third - forces the skip-ahead
  // loop to advance past two non-covering intervals first.
  GtidSet subset;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      subset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:20-21", error))
      << error;
  ASSERT_TRUE(superset.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-2:5-6:20-30", error))
      << error;
  EXPECT_TRUE(subset.IsSubsetOf(superset));
}

TEST(GtidSetTest,
     IsSubsetOfFalseWhenCandidateRunsOneGtidPastTheCoveringIntervalsEnd) {
  // 1-6 runs one GTID past other's first interval (1-5) into the gap
  // before its second (7-10) - an off-by-one on either endpoint would
  // wrongly call this covered.
  GtidSet candidate;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      candidate.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-6", error))
      << error;
  ASSERT_TRUE(superset.AddFromText(
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5:7-10", error))
      << error;
  EXPECT_FALSE(candidate.IsSubsetOf(superset));
}

TEST(GtidSetTest,
     IsSubsetOfFalseWhenCandidateRunsOneGtidPastSupersetsOnlyInterval) {
  GtidSet candidate;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      candidate.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-6", error))
      << error;
  ASSERT_TRUE(
      superset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5", error))
      << error;
  EXPECT_FALSE(candidate.IsSubsetOf(superset));
}

TEST(GtidSetTest,
     IsSubsetOfFalseWhenCandidateStartsOneGtidBeforeSupersetsOnlyInterval) {
  GtidSet candidate;
  GtidSet superset;
  std::string error;
  ASSERT_TRUE(
      candidate.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:4-6", error))
      << error;
  ASSERT_TRUE(
      superset.AddFromText("3e11fa47-71ca-11e1-9e33-c80aa9429562:5-10", error))
      << error;
  EXPECT_FALSE(candidate.IsSubsetOf(superset));
}

}  // namespace
}  // namespace binlog_streamer
