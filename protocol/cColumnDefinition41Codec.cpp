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

#include "protocol/cColumnDefinition41Codec.hpp"

#include "protocol/cLengthEncodedInteger.hpp"
#include "protocol/cLengthEncodedString.hpp"

namespace binlog_streamer {
namespace {

bool DecodeLenencStringField(std::span<const std::uint8_t> payload,
                             std::size_t &pos, std::string &out,
                             std::string &error, const char *fieldName) {
  const std::size_t consumed =
      LengthEncodedString::Decode(payload.subspan(pos), out);
  if (consumed == 0) {
    error = std::string("ColumnDefinition41: malformed ") + fieldName;
    return false;
  }
  pos += consumed;
  return true;
}

}  // namespace

bool ColumnDefinition41Codec::Parse(std::span<const std::uint8_t> payload,
                                    ColumnDefinition41 &value,
                                    std::string &error) {
  std::size_t pos = 0;
  ColumnDefinition41 parsed;
  if (!DecodeLenencStringField(payload, pos, parsed.catalog, error, "catalog"))
    return false;
  if (!DecodeLenencStringField(payload, pos, parsed.schema, error, "schema"))
    return false;
  if (!DecodeLenencStringField(payload, pos, parsed.table, error, "table"))
    return false;
  if (!DecodeLenencStringField(payload, pos, parsed.orgTable, error,
                               "org_table"))
    return false;
  if (!DecodeLenencStringField(payload, pos, parsed.name, error, "name"))
    return false;
  if (!DecodeLenencStringField(payload, pos, parsed.orgName, error, "org_name"))
    return false;

  std::uint64_t fixedFieldsLength = 0;
  bool isNull = false;
  const std::size_t fixedFieldsConsumed = LengthEncodedInteger::Decode(
      payload.subspan(pos), fixedFieldsLength, isNull);
  if (fixedFieldsConsumed == 0 || isNull) {
    error = "ColumnDefinition41: malformed length-of-fixed-fields marker";
    return false;
  }
  pos += fixedFieldsConsumed;

  if (payload.size() - pos < 10) {
    error = "ColumnDefinition41: too short for the fixed fields";
    return false;
  }
  parsed.characterSet = static_cast<std::uint16_t>(
      payload[pos] | (static_cast<std::uint32_t>(payload[pos + 1]) << 8));
  parsed.columnLength = static_cast<std::uint32_t>(payload[pos + 2]) |
                        (static_cast<std::uint32_t>(payload[pos + 3]) << 8) |
                        (static_cast<std::uint32_t>(payload[pos + 4]) << 16) |
                        (static_cast<std::uint32_t>(payload[pos + 5]) << 24);
  parsed.type = payload[pos + 6];
  parsed.flags = static_cast<std::uint16_t>(
      payload[pos + 7] | (static_cast<std::uint32_t>(payload[pos + 8]) << 8));
  parsed.decimals = payload[pos + 9];

  value = parsed;
  error.clear();
  return true;
}

// Layout as the server writes it (sql/protocol_classic.cc,
// Protocol_classic::send_field_metadata()): six strings, the 0x0C length of
// the fixed block, the fixed fields and two zero filler bytes.
void ColumnDefinition41Codec::Encode(const ColumnDefinition41 &value,
                                     std::vector<std::uint8_t> &out) {
  LengthEncodedString::Encode(value.catalog, out);
  LengthEncodedString::Encode(value.schema, out);
  LengthEncodedString::Encode(value.table, out);
  LengthEncodedString::Encode(value.orgTable, out);
  LengthEncodedString::Encode(value.name, out);
  LengthEncodedString::Encode(value.orgName, out);
  out.push_back(0x0C);
  out.push_back(static_cast<std::uint8_t>(value.characterSet));
  out.push_back(static_cast<std::uint8_t>(value.characterSet >> 8));
  out.push_back(static_cast<std::uint8_t>(value.columnLength));
  out.push_back(static_cast<std::uint8_t>(value.columnLength >> 8));
  out.push_back(static_cast<std::uint8_t>(value.columnLength >> 16));
  out.push_back(static_cast<std::uint8_t>(value.columnLength >> 24));
  out.push_back(value.type);
  out.push_back(static_cast<std::uint8_t>(value.flags));
  out.push_back(static_cast<std::uint8_t>(value.flags >> 8));
  out.push_back(value.decimals);
  out.insert(out.end(), 2, std::uint8_t{0});
}

}  // namespace binlog_streamer
