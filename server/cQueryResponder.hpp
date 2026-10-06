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

#include "cSessionVariables.hpp"
#include "sQueryResponse.hpp"
#include "server/iServerState.hpp"
#include "server/sServerIdentity.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace binlog_streamer {

// Recognises statements by shape, not by parsing SQL.
class QueryResponder {
 public:
  // state may be null: the history variables then read as empty and the
  // checksum as CRC32.
  QueryResponder(ServerIdentity identity, const ServerState *state);

  QueryResponse Respond(std::string_view sql, SessionVariables &session) const;

 private:
  QueryResponse Select(std::string_view expressions, std::string_view statement,
                       const SessionVariables &session) const;
  QueryResponse Set(std::string_view assignments, std::string_view statement,
                    SessionVariables &session) const;
  QueryResponse ShowPreviousGtids(std::string_view arguments,
                                  std::string_view statement) const;
  std::optional<std::string> SystemVariable(std::string_view name) const;
  // The same string the greeting showed, built the same way: a replica compares
  // the two.
  std::string ServerVersion() const;

  ServerIdentity m_identity;
  const ServerState *m_state;
};

}  // namespace binlog_streamer
