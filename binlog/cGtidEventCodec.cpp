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

#include "binlog/cSerializationDecoder.hpp"
#include "protocol/cLengthEncodedInteger.hpp"

namespace binlog_streamer {
namespace {

// Byte offsets of the untagged Gtid_event body (percona-server dfc6d1f,
// control_events.cpp:461-511). Requires lt_type present, stricter than the
// source's own can_read(), which also accepts an older 25-byte body.
constexpr std::size_t UUID_LENGTH = 16;
constexpr std::size_t GNO_LENGTH = 8;
constexpr std::size_t FIXED_PREFIX_LENGTH =
    1 /*flags*/ + UUID_LENGTH + GNO_LENGTH + 1 /*lt_type*/;
constexpr std::uint8_t LOGICAL_TIMESTAMP_TYPECODE = 2;
constexpr std::size_t LOGICAL_CLOCK_LENGTH =
    8 + 8;  // last_committed + sequence_number, unused by GtidEvent
constexpr std::size_t COMMIT_TIMESTAMP_LENGTH =
    7;  // IMMEDIATE_COMMIT_TIMESTAMP_LENGTH / ORIGINAL_COMMIT_TIMESTAMP_LENGTH
// Bit 55 of immediate_commit_timestamp: set when the transaction
// originated elsewhere, and a second 7-byte original_commit_timestamp
// follows (percona-server dfc6d1f, control_events.cpp:570-582).
constexpr std::uint64_t ORIGINAL_TIMESTAMP_FOLLOWS_BIT = 1ULL << 55;

std::uint64_t ReadLittleEndian(std::span<const std::uint8_t> data,
                               std::size_t length) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < length; ++i)
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  return value;
}

// Field ids of the tagged form (control_events.h:1111-1137, in definition
// order) and the tag's bound (libs/mysql/gtid/gtid_constants.h,
// tag_max_length).
constexpr std::uint64_t TAGGED_FIELD_FLAGS = 0;
constexpr std::uint64_t TAGGED_FIELD_UUID = 1;
constexpr std::uint64_t TAGGED_FIELD_GNO = 2;
constexpr std::uint64_t TAGGED_FIELD_TAG = 3;
constexpr std::uint64_t TAGGED_FIELD_TRANSACTION_LENGTH = 8;
constexpr std::size_t TAG_MAX_LENGTH = 32;

// Positions the decoder on field `id` if it's next: present=true after
// consuming the id, present=false if the next id is later (field left
// out), false/error if the next id is earlier - ids only ascend.
bool EnterField(SerializationDecoder &decoder, std::uint64_t id, bool &present,
                std::string &error) {
  present = false;
  if (decoder.AtEnd()) return true;
  std::uint64_t next = 0;
  if (!decoder.PeekFieldId(next)) {
    error = "tagged GTID event: malformed field id at byte " +
            std::to_string(decoder.Position());
    return false;
  }
  if (next > id) return true;
  if (next < id) {
    error = "tagged GTID event: field " + std::to_string(next) +
            " out of order at byte " + std::to_string(decoder.Position());
    return false;
  }
  decoder.SkipFieldId();
  present = true;
  return true;
}

}  // namespace

bool GtidEventCodec::ParseTagged(std::span<const std::uint8_t> body,
                                 std::size_t checksumLength, GtidEvent &value,
                                 std::string &error) {
  if (body.size() < checksumLength) {
    error = "GTID event body shorter than the negotiated checksum";
    return false;
  }
  SerializationDecoder decoder(body.first(body.size() - checksumLength));
  std::uint64_t messageSize = 0;
  std::uint64_t lastNonIgnorableFieldId = 0;
  if (!decoder.ReadMessageHeader(messageSize, lastNonIgnorableFieldId)) {
    error = "tagged GTID event: malformed message header";
    return false;
  }
  // The size counts the whole message, its own header included
  // (serializer_default_impl.hpp, get_size_serializable) - a real event
  // has it equal to the body without the checksum.
  if (messageSize > body.size() - checksumLength) {
    error = "tagged GTID event: message size " + std::to_string(messageSize) +
            " exceeds the body";
    return false;
  }

  GtidEvent parsed;
  bool present = false;
  std::uint64_t unused = 0;
  if (!EnterField(decoder, TAGGED_FIELD_FLAGS, present, error)) return false;
  if (present && !decoder.ReadUnsigned(unused)) {
    error = "tagged GTID event: truncated flags";
    return false;
  }
  if (!EnterField(decoder, TAGGED_FIELD_UUID, present, error)) return false;
  if (!present || !decoder.ReadByteArray(parsed.uuid.bytes)) {
    error = "tagged GTID event: missing or truncated uuid";
    return false;
  }
  if (!EnterField(decoder, TAGGED_FIELD_GNO, present, error)) return false;
  if (!present || !decoder.ReadSigned(parsed.gno)) {
    error = "tagged GTID event: missing or truncated gno";
    return false;
  }
  if (!EnterField(decoder, TAGGED_FIELD_TAG, present, error)) return false;
  if (!present || !decoder.ReadString(TAG_MAX_LENGTH, parsed.tag)) {
    error = "tagged GTID event: missing, truncated or overlong tag";
    return false;
  }
  // Fields 4..7 are single varints this relay does not use; each is
  // read only to step over it, and an absent one (7 is optional) costs
  // nothing.
  for (std::uint64_t id = TAGGED_FIELD_TAG + 1;
       id < TAGGED_FIELD_TRANSACTION_LENGTH; ++id) {
    if (!EnterField(decoder, id, present, error)) return false;
    if (present && !decoder.ReadUnsigned(unused)) {
      error = "tagged GTID event: truncated field " + std::to_string(id);
      return false;
    }
  }
  if (!EnterField(decoder, TAGGED_FIELD_TRANSACTION_LENGTH, present, error))
    return false;
  if (present) {
    if (!decoder.ReadUnsigned(parsed.transactionLength)) {
      error = "tagged GTID event: truncated transaction_length";
      return false;
    }
    parsed.hasTransactionLength = true;
  }
  value = parsed;
  error.clear();
  return true;
}

bool GtidEventCodec::Parse(std::span<const std::uint8_t> body,
                           std::size_t checksumLength, GtidEvent &value,
                           std::string &error) {
  if (body.size() < checksumLength) {
    error = "GTID event body shorter than the negotiated checksum";
    return false;
  }
  const std::span<const std::uint8_t> content =
      body.first(body.size() - checksumLength);
  if (content.size() < FIXED_PREFIX_LENGTH) {
    error = "GTID event body shorter than its fixed prefix";
    return false;
  }

  GtidEvent parsed;
  std::size_t pos = 1;  // skip gtid_flags: not needed by GtidEvent
  for (std::size_t i = 0; i < parsed.uuid.bytes.size(); ++i)
    parsed.uuid.bytes[i] = content[pos + i];
  pos += parsed.uuid.bytes.size();
  parsed.gno = static_cast<std::int64_t>(
      ReadLittleEndian(content.subspan(pos), GNO_LENGTH));
  pos += GNO_LENGTH;
  const std::uint8_t logicalTimestampType = content[pos];
  pos += 1;

  // A source without the optional tail leaves transactionLength unset
  // rather than 0 - callers must not treat "unknown" as "zero-length".
  if (logicalTimestampType != LOGICAL_TIMESTAMP_TYPECODE) {
    value = parsed;
    error.clear();
    return true;
  }
  if (content.size() - pos < LOGICAL_CLOCK_LENGTH) {
    error =
        "GTID event declares a logical clock but its body ends before "
        "last_committed/sequence_number";
    return false;
  }
  pos += LOGICAL_CLOCK_LENGTH;

  if (content.size() - pos < COMMIT_TIMESTAMP_LENGTH) {
    // Older source, no commit-timestamp section - success, not an
    // error (percona-server dfc6d1f, control_events.cpp:565-587).
    value = parsed;
    error.clear();
    return true;
  }
  const std::uint64_t immediateCommitTimestamp =
      ReadLittleEndian(content.subspan(pos), COMMIT_TIMESTAMP_LENGTH);
  pos += COMMIT_TIMESTAMP_LENGTH;
  if ((immediateCommitTimestamp & ORIGINAL_TIMESTAMP_FOLLOWS_BIT) != 0) {
    if (content.size() - pos < COMMIT_TIMESTAMP_LENGTH) {
      error =
          "GTID event's immediate_commit_timestamp claims an "
          "original_commit_timestamp that is not there";
      return false;
    }
    pos += COMMIT_TIMESTAMP_LENGTH;
  }

  if (content.size() - pos < 1) {
    // Commit timestamps present, nothing after - genuinely absent, not
    // truncated.
    value = parsed;
    error.clear();
    return true;
  }
  std::uint64_t transactionLength = 0;
  bool isNull = false;
  const std::size_t consumed = LengthEncodedInteger::Decode(
      content.subspan(pos), transactionLength, isNull);
  if (consumed == 0 || isNull) {
    error = "GTID event's transaction_length is truncated";
    return false;
  }
  parsed.transactionLength = transactionLength;
  parsed.hasTransactionLength = true;
  value = parsed;
  error.clear();
  return true;
}

}  // namespace binlog_streamer
