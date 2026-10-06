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

#include <optional>
#include <string>
#include "receiver/iBinlogProbe.hpp"
#include "receiver/sSessionResult.hpp"
#include "receiver/sStartSetResolution.hpp"

namespace binlog_streamer {

// Never gtid_executed: it can claim more than the source's current file covers.
class StartSetResolver {
 public:
  explicit StartSetResolver(BinlogProbe &probe) : m_probe(probe) {}

  // nullopt means success (resolution is filled in).
  std::optional<SessionResult> Resolve(const std::string &gtidExecutedText,
                                       const std::string &gtidPurgedText,
                                       StartSetResolution &resolution);

 private:
  BinlogProbe &m_probe;
};

}  // namespace binlog_streamer
