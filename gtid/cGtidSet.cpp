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

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <limits>
#include "cTagText.hpp"
#include "cUuidText.hpp"
#include "gtid/hGtidLimits.hpp"

namespace binlog_streamer {
namespace {

// GNO_END (sql/rpl_gtid.h): valid GTID numbers are < INT64_MAX. Checked
// here rather than after "endInclusive + 1" to also avoid signed overflow.
constexpr std::int64_t GNO_END = std::numeric_limits<std::int64_t>::max();

bool ParseGno(std::string_view text, std::size_t &pos, std::int64_t &value) {
  const char *begin = text.data() + pos;
  const char *end = text.data() + text.size();
  std::int64_t parsed = 0;
  const auto [ptr, status] = std::from_chars(begin, end, parsed);
  if (status != std::errc{} || ptr == begin || parsed <= 0 || parsed >= GNO_END)
    return false;
  pos += static_cast<std::size_t>(ptr - begin);
  value = parsed;
  return true;
}

void WriteUint64LE(std::vector<std::uint8_t> &out, std::size_t pos,
                   std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    out[pos + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint64_t ReadUint64LE(std::span<const std::uint8_t> bytes,
                           std::size_t pos) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
    value |=
        static_cast<std::uint64_t>(bytes[pos + static_cast<std::size_t>(i)])
        << (8 * i);
  return value;
}

}  // namespace

bool GtidSet::AddInterval(const GtidSource &source, std::int64_t start,
                          std::int64_t end) {
  if (end <= start) return false;
  // Enforced here: Encode() packs tag length into one byte, and GtidSource
  // itself carries no such limit.
  if (source.tag.size() > GTID_TAG_MAX_LENGTH) return false;
  std::vector<GtidInterval> &intervals = m_bySource[source];
  intervals.push_back(GtidInterval{start, end});
  std::sort(intervals.begin(), intervals.end(),
            [](const GtidInterval &a, const GtidInterval &b) {
              return a.start < b.start;
            });
  std::vector<GtidInterval> merged;
  merged.reserve(intervals.size());
  for (const auto &interval : intervals) {
    // Touching intervals ([1,5)+[5,10)) merge too, not just overlapping ones.
    if (!merged.empty() && interval.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, interval.end);
    } else {
      merged.push_back(interval);
    }
  }
  intervals = std::move(merged);
  return true;
}

bool GtidSet::AddFromText(std::string_view text, std::string &error) {
  error.clear();
  std::size_t pos = 0;
  while (pos < text.size()) {
    // Mirrors add_gtid_text(): only skips whitespace right after a comma,
    // which also swallows ToText()'s trailing newline.
    while (pos < text.size() && text[pos] == ',') {
      ++pos;
      while (pos < text.size() &&
             std::isspace(static_cast<unsigned char>(text[pos])) != 0)
        ++pos;
    }
    if (pos >= text.size()) break;

    if (text.size() - pos < UUID_TEXT_LENGTH) {
      error = "expected a UUID at the start of a GTID set entry";
      return false;
    }
    Uuid uuid;
    std::string uuidError;
    if (!UuidText::Parse(text.substr(pos, UUID_TEXT_LENGTH), uuid, uuidError)) {
      error = uuidError;
      return false;
    }
    pos += UUID_TEXT_LENGTH;

    // A tag token may reappear within one UUID's chain, re-selecting the
    // source for the interval tokens that follow (matches add_gtid_text()).
    std::string tag;
    while (pos < text.size() && text[pos] == ':') {
      ++pos;
      if (pos < text.size() &&
          std::isdigit(static_cast<unsigned char>(text[pos])) != 0) {
        std::int64_t start = 0;
        if (!ParseGno(text, pos, start)) {
          error = "expected a positive GTID number";
          return false;
        }
        std::int64_t end = start + 1;
        if (pos < text.size() && text[pos] == '-') {
          ++pos;
          std::int64_t endInclusive = 0;
          if (!ParseGno(text, pos, endInclusive) || endInclusive < start) {
            error = "expected a GTID number no smaller than the range start";
            return false;
          }
          end = endInclusive + 1;
        }
        if (!AddInterval(GtidSource{uuid, tag}, start, end)) {
          error = "malformed interval";
          return false;
        }
      } else {
        const std::size_t tagStart = pos;
        while (pos < text.size() && text[pos] != ':' && text[pos] != ',') ++pos;
        std::string tagError;
        if (!TagText::Parse(text.substr(tagStart, pos - tagStart), tag,
                            tagError)) {
          error = tagError;
          return false;
        }
      }
    }
    if (pos < text.size() && text[pos] != ',') {
      error = "expected ',' or end of input";
      return false;
    }
  }
  return true;
}

std::string GtidSet::ToText() const {
  std::string text;
  bool firstGroup = true;
  // Relies on m_bySource ordering by uuid first, so each uuid's sources
  // form one run here.
  const Uuid *previousUuid = nullptr;
  for (const auto &[source, intervals] : m_bySource) {
    if (intervals.empty()) continue;
    if (previousUuid == nullptr || source.uuid != *previousUuid) {
      if (!firstGroup) {
        text.push_back(',');
        text.push_back('\n');
      }
      text += UuidText::ToString(source.uuid);
    }
    firstGroup = false;
    previousUuid = &source.uuid;
    if (!source.tag.empty()) {
      text.push_back(':');
      text += source.tag;
    }
    for (const auto &interval : intervals) {
      text.push_back(':');
      text += std::to_string(interval.start);
      if (interval.end - 1 > interval.start) {
        text.push_back('-');
        text += std::to_string(interval.end - 1);
      }
    }
  }
  return text;
}

std::size_t GtidSet::GetEncodedLength(bool skipTaggedGtids) const {
  bool anyTagged = false;
  for (const auto &[source, intervals] : m_bySource) {
    if (!intervals.empty() && !source.tag.empty()) {
      anyTagged = true;
      break;
    }
  }
  const bool tagged = !skipTaggedGtids && anyTagged;

  std::size_t length = 8;
  for (const auto &[source, intervals] : m_bySource) {
    if (intervals.empty()) continue;
    if (!source.tag.empty() && skipTaggedGtids) continue;
    length += 16 + 8 + 2 * 8 * intervals.size();
    if (tagged) length += 1 + source.tag.size();
  }
  return length;
}

std::vector<std::uint8_t> GtidSet::Encode(bool skipTaggedGtids) const {
  bool anyTagged = false;
  for (const auto &[source, intervals] : m_bySource) {
    if (!intervals.empty() && !source.tag.empty()) {
      anyTagged = true;
      break;
    }
  }
  const bool tagged = !skipTaggedGtids && anyTagged;

  std::vector<std::uint8_t> out(GetEncodedLength(skipTaggedGtids), 0);
  std::uint64_t sourceCount = 0;
  std::size_t pos = 8;
  for (const auto &[source, intervals] : m_bySource) {
    if (intervals.empty()) continue;
    if (!source.tag.empty() && skipTaggedGtids) continue;
    ++sourceCount;
    for (const std::uint8_t b : source.uuid.bytes) out[pos++] = b;
    if (tagged) {
      // Server's general varint (value << 1 in one byte), not protocol
      // lenenc - safe since GTID_TAG_MAX_LENGTH (32) fits in one byte.
      out[pos++] = static_cast<std::uint8_t>(source.tag.size() << 1);
      for (const char c : source.tag) out[pos++] = static_cast<std::uint8_t>(c);
    }
    WriteUint64LE(out, pos, static_cast<std::uint64_t>(intervals.size()));
    pos += 8;
    for (const auto &interval : intervals) {
      WriteUint64LE(out, pos, static_cast<std::uint64_t>(interval.start));
      pos += 8;
      WriteUint64LE(out, pos, static_cast<std::uint64_t>(interval.end));
      pos += 8;
    }
  }
  // Matches encode_nsids_format(): untagged uses only the top byte
  // (indistinguishable from plain LE sourceCount); tagged repeats the
  // format byte in the low byte too.
  const std::uint64_t header =
      tagged
          ? ((std::uint64_t{1} << 56) | (sourceCount << 8) | std::uint64_t{1})
          : sourceCount;
  WriteUint64LE(out, 0, header);
  return out;
}

bool GtidSet::AddFromEncoding(std::span<const std::uint8_t> encoded,
                              std::string &error) {
  error.clear();
  constexpr std::size_t INTEGER_LENGTH = 8;
  constexpr std::size_t UUID_LENGTH = 16;
  constexpr std::size_t INTERVAL_LENGTH = 2 * INTEGER_LENGTH;

  if (encoded.size() < INTEGER_LENGTH) {
    error = "GTID set encoding is shorter than its header";
    return false;
  }

  // Mirrors decode_nsids_format(): tagged repeats the format byte in the
  // low byte too - mask both out, or they shift into sourceCount's high bits.
  const std::uint64_t header = ReadUint64LE(encoded, 0);
  const auto formatByte = static_cast<std::uint8_t>(header >> 56);
  constexpr std::uint64_t TOP_BYTE_MASK = std::uint64_t{0xFF} << 56;
  constexpr std::uint64_t BOTTOM_BYTE_MASK = 0xFF;
  bool tagged = false;
  std::uint64_t sourceCount = 0;
  if (formatByte == 0) {
    sourceCount = header & ~TOP_BYTE_MASK;
  } else if (formatByte == 1) {
    tagged = true;
    sourceCount = (header & ~(TOP_BYTE_MASK | BOTTOM_BYTE_MASK)) >> 8;
  } else {
    error = "unknown GTID set encoding format";
    return false;
  }

  std::size_t pos = INTEGER_LENGTH;
  for (std::uint64_t sourceIndex = 0; sourceIndex < sourceCount;
       ++sourceIndex) {
    if (encoded.size() - pos < UUID_LENGTH) {
      error = "GTID set encoding is truncated inside a source UUID";
      return false;
    }
    Uuid uuid;
    for (std::size_t i = 0; i < UUID_LENGTH; ++i)
      uuid.bytes[i] = encoded[pos + i];
    pos += UUID_LENGTH;

    std::string tag;
    if (tagged) {
      if (pos >= encoded.size()) {
        error = "GTID set encoding is truncated at a tag length";
        return false;
      }
      // A set low bit is the multi-byte varint form, which this relay
      // neither produces nor expects from a real source.
      const std::uint8_t lengthByte = encoded[pos];
      if ((lengthByte & 1) != 0) {
        error = "GTID tag length prefix is not a single-byte encoding";
        return false;
      }
      const auto tagLength = static_cast<std::size_t>(lengthByte >> 1);
      ++pos;
      if (encoded.size() - pos < tagLength) {
        error = "GTID set encoding is truncated inside a tag";
        return false;
      }
      // A zero length is an untagged source stored in an otherwise tagged
      // set (Encode() does this for every source once any is tagged); handled
      // directly since TagText::Parse rejects empty text.
      if (tagLength > 0) {
        std::string tagError;
        const std::string_view rawTag(
            reinterpret_cast<const char *>(encoded.data() + pos), tagLength);
        if (!TagText::Parse(rawTag, tag, tagError)) {
          error = tagError;
          return false;
        }
      }
      pos += tagLength;
    }

    if (encoded.size() - pos < INTEGER_LENGTH) {
      error = "GTID set encoding is truncated at an interval count";
      return false;
    }
    const std::uint64_t intervalCount = ReadUint64LE(encoded, pos);
    pos += INTEGER_LENGTH;
    const std::uint64_t maxIntervals = (encoded.size() - pos) / INTERVAL_LENGTH;
    if (intervalCount > maxIntervals) {
      error = "GTID set encoding is truncated inside its intervals";
      return false;
    }

    const GtidSource source{uuid, tag};
    std::int64_t previousEnd = 0;
    for (std::uint64_t intervalIndex = 0; intervalIndex < intervalCount;
         ++intervalIndex) {
      const auto start = static_cast<std::int64_t>(ReadUint64LE(encoded, pos));
      pos += INTEGER_LENGTH;
      const auto end = static_cast<std::int64_t>(ReadUint64LE(encoded, pos));
      pos += INTEGER_LENGTH;
      // Matches add_gtid_encoding(): intervals must arrive strictly
      // increasing and already merged - AddInterval() can't tell corrupt
      // data from mergeable input.
      if (start <= previousEnd || !AddInterval(source, start, end)) {
        error = "GTID set encoding has an out-of-order or malformed interval";
        return false;
      }
      previousEnd = end;
    }
  }

  if (pos != encoded.size()) {
    error = "GTID set encoding has unexpected trailing bytes";
    return false;
  }
  return true;
}

bool GtidSet::IsEmpty() const {
  return std::all_of(m_bySource.begin(), m_bySource.end(),
                     [](const auto &entry) { return entry.second.empty(); });
}

bool GtidSet::IsSubsetOf(const GtidSet &other) const {
  for (const auto &[source, intervals] : m_bySource) {
    if (intervals.empty()) continue;
    // Direct lookup, without the copy GetIntervals() would make.
    const auto otherIt = other.m_bySource.find(source);
    if (otherIt == other.m_bySource.end()) return false;
    const std::vector<GtidInterval> &otherIntervals = otherIt->second;
    // Both lists are ascending, so the cursor only moves forward.
    std::size_t otherIndex = 0;
    for (const auto &interval : intervals) {
      // Skip intervals that end before this one starts (half-open ends).
      while (otherIndex < otherIntervals.size() &&
             interval.start > otherIntervals[otherIndex].end)
        ++otherIndex;
      // Merged intervals: only this one can cover the whole interval.
      if (otherIndex >= otherIntervals.size()) return false;
      if (interval.start < otherIntervals[otherIndex].start ||
          interval.end > otherIntervals[otherIndex].end)
        return false;
    }
  }
  return true;
}

bool GtidSet::Contains(const GtidSource &source, std::int64_t gno) const {
  const auto it = m_bySource.find(source);
  if (it == m_bySource.end()) return false;
  // Merged and ascending: the only candidate is the last interval starting
  // at or before gno.
  const auto &intervals = it->second;
  const auto after =
      std::upper_bound(intervals.begin(), intervals.end(), gno,
                       [](std::int64_t value, const GtidInterval &interval) {
                         return value < interval.start;
                       });
  return after != intervals.begin() && gno < std::prev(after)->end;
}

std::vector<GtidInterval> GtidSet::GetIntervals(
    const GtidSource &source) const {
  const auto it = m_bySource.find(source);
  if (it == m_bySource.end()) return {};
  return it->second;
}

}  // namespace binlog_streamer
