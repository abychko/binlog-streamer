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

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "gtid/sGtidInterval.hpp"
#include "gtid/sGtidSource.hpp"

namespace binlog_streamer {

// Text and binary forms mirror Percona Server's Gtid_set
// (sql/rpl_gtid_set.cc).
class GtidSet {
 public:
  // Returns false and leaves the set unchanged if end <= start.
  bool AddInterval(const GtidSource &source, std::int64_t start,
                   std::int64_t end);

  // Whitespace is only skipped right after a comma, matching the server
  // exactly (Gtid_set::add_gtid_text) - not a bug.
  bool AddFromText(std::string_view text, std::string &error);

  // Same shape/separator as the server's default_string_format
  // (rpl_gtid_set.cc) and GTID_SUBTRACT() output.
  std::string ToText() const;

  // Matches Gtid_set::get_encoded_length()/encode() byte-for-byte.
  // skipTaggedGtids drops tagged sources entirely, not just their tag.
  std::size_t GetEncodedLength(bool skipTaggedGtids) const;
  std::vector<std::uint8_t> Encode(bool skipTaggedGtids) const;

  // Strict: trailing bytes are an error, matching the server's read of
  // its own files, not its lenient live-binlog scan. No rollback on
  // failure (unlike AddInterval).
  bool AddFromEncoding(std::span<const std::uint8_t> encoded,
                       std::string &error);

  bool IsEmpty() const;

  // Relies on both sides keeping intervals merged and ascending
  // (mirrors Gtid_set::is_subset()).
  bool IsSubsetOf(const GtidSet &other) const;

  // Whether the set holds this one transaction; nothing is copied, since a
  // dump asks it of every transaction it passes over.
  bool Contains(const GtidSource &source, std::int64_t gno) const;

  // Exposed for tests.
  std::vector<GtidInterval> GetIntervals(const GtidSource &source) const;

 private:
  std::map<GtidSource, std::vector<GtidInterval>> m_bySource;
};

}  // namespace binlog_streamer
