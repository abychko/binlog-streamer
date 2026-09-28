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

#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace binlog_streamer {
namespace {

// Byte offsets of the fixed prefix (percona-server dfc6d1f, binlog_event.h:
// 429-445): binlog_version(2)+server_version(50)+created(4)+
// common_header_len(1). post_header_len[] follows and isn't stored here.
constexpr std::size_t BINLOG_VERSION_OFFSET = 0;
constexpr std::size_t SERVER_VERSION_OFFSET = 2;
constexpr std::size_t SERVER_VERSION_LENGTH = 50;  // ST_SERVER_VER_LEN
constexpr std::size_t CREATED_OFFSET =
    SERVER_VERSION_OFFSET + SERVER_VERSION_LENGTH;
constexpr std::size_t COMMON_HEADER_LENGTH_OFFSET = CREATED_OFFSET + 4;
constexpr std::size_t FIXED_PREFIX_LENGTH = COMMON_HEADER_LENGTH_OFFSET + 1;
constexpr std::size_t CHECKSUM_ALGORITHM_DESCRIPTOR_LENGTH =
    1;  // BINLOG_CHECKSUM_ALG_DESC_LEN
constexpr std::size_t CHECKSUM_TRAILER_LENGTH =
    CHECKSUM_ALGORITHM_DESCRIPTOR_LENGTH + CHECKSUM_LENGTH;

// Checksums were introduced in server version 5.6.1 (percona-server
// dfc6d1f, binlog_event.cpp:31-34), folded into one base-256 number.
constexpr unsigned long CHECKSUM_VERSION_PRODUCT = (5UL * 256 + 6) * 256 + 1;

std::uint32_t ReadLittleEndian(std::span<const std::uint8_t> data,
                               std::size_t length) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < length; ++i)
    value |= static_cast<std::uint32_t>(data[i]) << (8 * i);
  return value;
}

// server_version is a fixed 50-byte NUL-terminated buffer (percona-server
// dfc6d1f, control_events.cpp:216-220), not 50 bytes of text.
std::string ReadServerVersion(std::span<const std::uint8_t> data) {
  const auto nulAt = std::find(data.begin(), data.end(), std::uint8_t{0});
  return std::string(data.begin(), nulAt);
}

// Mirrors do_server_version_split()/version_product() (percona-server
// dfc6d1f, binlog_event.h:192-244). An unparsable/out-of-range version
// resolves to 0 - the source's own rule, which also reads as "pre-checksum".
unsigned long ComputeVersionProduct(const std::string &version) {
  unsigned char parts[3] = {0, 0, 0};
  const char *p = version.c_str();
  for (int i = 0; i <= 2; ++i) {
    char *end = nullptr;
    const unsigned long number = std::strtoul(p, &end, 10);
    if (number < 256 && (*end == '.' || i != 0)) {
      parts[static_cast<std::size_t>(i)] = static_cast<unsigned char>(number);
    } else {
      parts[0] = parts[1] = parts[2] = 0;
      break;
    }
    p = end;
    if (*end == '.') ++p;
  }
  return (static_cast<unsigned long>(parts[0]) * 256 + parts[1]) * 256 +
         parts[2];
}

std::string DescribeChecksumAlgorithm(std::uint8_t algorithm) {
  switch (algorithm) {
    case 0:
      return "OFF";  // BINLOG_CHECKSUM_ALG_OFF
    case 1:
      return "CRC32";  // BINLOG_CHECKSUM_ALG_CRC32
    default:
      return "UNKNOWN(" + std::to_string(algorithm) + ")";
  }
}

}  // namespace

bool FormatDescriptionEventCodec::Parse(std::span<const std::uint8_t> body,
                                        FormatDescriptionEvent &value,
                                        std::string &error) {
  if (body.size() < FIXED_PREFIX_LENGTH) {
    error = "Format description event body shorter than its fixed prefix";
    return false;
  }

  FormatDescriptionEvent parsed;
  parsed.binlogVersion = static_cast<std::uint16_t>(
      ReadLittleEndian(body.subspan(BINLOG_VERSION_OFFSET), 2));
  parsed.serverVersion = ReadServerVersion(
      body.subspan(SERVER_VERSION_OFFSET, SERVER_VERSION_LENGTH));
  parsed.created = ReadLittleEndian(body.subspan(CREATED_OFFSET), 4);
  parsed.commonHeaderLength = body[COMMON_HEADER_LENGTH_OFFSET];
  if (parsed.commonHeaderLength < EVENT_HEADER_LENGTH) {
    error =
        "Format description event declares a common header shorter than the "
        "Common-Header itself";
    return false;
  }

  // Mirrors the source's write/read gate: since 5.6.1 the checksum room is
  // always appended regardless of the checksum setting, so this must be
  // derived from serverVersion (percona-server dfc6d1f,
  // binlog_event.cpp:130-159).
  const bool hasChecksum =
      ComputeVersionProduct(parsed.serverVersion) >= CHECKSUM_VERSION_PRODUCT;
  if (hasChecksum) {
    if (body.size() < FIXED_PREFIX_LENGTH + CHECKSUM_TRAILER_LENGTH) {
      error =
          "Format description event's server version implies a checksum "
          "trailer that does not fit";
      return false;
    }
    // Algorithm descriptor is the body's last byte before the checksum
    // room, wherever post_header_len[] ends - no need to walk that array.
    parsed.checksumAlgorithm =
        DescribeChecksumAlgorithm(body[body.size() - CHECKSUM_TRAILER_LENGTH]);
  } else {
    parsed.checksumAlgorithm = "UNDEF";
  }

  value = parsed;
  error.clear();
  return true;
}

bool FormatDescriptionEventCodec::EqualForResume(
    std::span<const std::uint8_t> storedEvent,
    std::span<const std::uint8_t> incomingEvent, std::size_t checksumLength,
    std::string &error) {
  if (storedEvent.size() != incomingEvent.size()) {
    error = "length differs (stored " + std::to_string(storedEvent.size()) +
            ", incoming " + std::to_string(incomingEvent.size()) + ")";
    return false;
  }
  // Common-Header's flags low byte (sEventHeader.hpp: "data[17]") - where
  // LOG_EVENT_BINLOG_IN_USE_F lives, the one bit this comparison ignores.
  // Absolute within the whole event, not relative like the offsets above.
  constexpr std::size_t FLAGS_OFFSET = 17;
  constexpr std::size_t CREATED_EVENT_OFFSET =
      EVENT_HEADER_LENGTH + CREATED_OFFSET;
  if (storedEvent.size() < CREATED_EVENT_OFFSET + 4 + checksumLength) {
    error =
        "shorter than a Format_description_event with this checksum length can "
        "be";
    return false;
  }
  for (std::size_t i = 0; i < storedEvent.size(); ++i) {
    if (i == FLAGS_OFFSET) {
      if ((storedEvent[i] & ~EVENT_FLAG_BINLOG_IN_USE) !=
          (incomingEvent[i] & ~EVENT_FLAG_BINLOG_IN_USE)) {
        error = "flags differ at byte " + std::to_string(i);
        return false;
      }
      continue;
    }
    if (i >= CREATED_EVENT_OFFSET && i < CREATED_EVENT_OFFSET + 4)
      continue;  // `created` - zeroed on resume, allowed to differ
    if (i >= storedEvent.size() - checksumLength)
      continue;  // trailing checksum - computed over `created`, differs with it
    if (storedEvent[i] != incomingEvent[i]) {
      error = "byte " + std::to_string(i) + " differs";
      return false;
    }
  }
  error.clear();
  return true;
}

}  // namespace binlog_streamer
