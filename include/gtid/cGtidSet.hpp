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

class GtidSet {
 public:
  bool AddInterval(const GtidSource &source, std::int64_t start,
                   std::int64_t end);

  // Whitespace is skipped only right after a comma, exactly as
  // Gtid_set::add_gtid_text does.
  bool AddFromText(std::string_view text, std::string &error);

  std::string ToText() const;

  // Byte-for-byte Gtid_set::encode(); skipTaggedGtids drops tagged sources
  // entirely.
  std::size_t GetEncodedLength(bool skipTaggedGtids) const;
  std::vector<std::uint8_t> Encode(bool skipTaggedGtids) const;

  // Strict: trailing bytes are an error. No rollback on failure, unlike
  // AddInterval.
  bool AddFromEncoding(std::span<const std::uint8_t> encoded,
                       std::string &error);

  bool IsEmpty() const;

  // Both sides must keep their intervals merged and ascending.
  bool IsSubsetOf(const GtidSet &other) const;

  bool Contains(const GtidSource &source, std::int64_t gno) const;

  std::vector<GtidInterval> GetIntervals(const GtidSource &source) const;

 private:
  std::map<GtidSource, std::vector<GtidInterval>> m_bySource;
};

}  // namespace binlog_streamer
