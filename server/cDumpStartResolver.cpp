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

#include "cDumpStartResolver.hpp"

#include <algorithm>
#include <cctype>

namespace binlog_streamer {

namespace {

constexpr char PURGED_REQUIRED_GTIDS[] =
    "Cannot replicate because the source purged required binary logs. "
    "Replicate the missing "
    "transactions from elsewhere, or provision a new replica from backup. "
    "Consider increasing "
    "the source's binary log expiration period.";
constexpr char REPLICA_HAS_MORE_GTIDS[] =
    "Replica has more GTIDs than the source has, using the source's "
    "SERVER_UUID. This may "
    "indicate that the the last binary log file was truncated or lost, e.g., "
    "after a power "
    "failure when sync_binlog != 1. The source may have rolled back "
    "transactions that were "
    "already replicated to the replica. Replicate any transactions that source "
    "has rolled "
    "back from replica to source, and/or commit empty transactions on source "
    "to account for "
    "transactions that have been committed on source but are not included in "
    "GTID_EXECUTED.";

// A file can be removed between being chosen and being opened; the choice is
// then made again.
constexpr int OPEN_ATTEMPTS = 3;

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

}  // namespace

DumpStart DumpStartResolver::Resolve(const BinlogDumpGtidCommand &command,
                                     const std::string &serverUuid,
                                     BinlogStorageReader &reader) {
  DumpStart result;
  std::string error;
  if (!result.replicaSet.AddFromEncoding(command.gtidSetEncoded, error)) {
    result.kind = DumpStartKind::BadRequest;
    result.message = "malformed GTID set in the dump request: " + error;
    return result;
  }

  // The relay issues no GTIDs of its own, so any the replica holds under the
  // relay's UUID (tagged or not) are ones it cannot have.
  if (!serverUuid.empty() &&
      Lower(result.replicaSet.ToText()).find(Lower(serverUuid)) !=
          std::string::npos) {
    result.kind = DumpStartKind::ReplicaHasOwnGtids;
    result.message = REPLICA_HAS_MORE_GTIDS;
    return result;
  }

  if (reader.Published().fileName.empty()) {
    result.kind = DumpStartKind::NoHistoryYet;
    result.message = "the relay has not stored its first binary log file yet";
    return result;
  }

  for (int attempt = 0; attempt < OPEN_ATTEMPTS; ++attempt) {
    const auto fileName = reader.FindStartFile(result.replicaSet);
    if (!fileName) {
      result.kind = DumpStartKind::PurgedRequiredGtids;
      result.message = PURGED_REQUIRED_GTIDS;
      return result;
    }
    result.cursor = reader.Open(*fileName, error);
    if (result.cursor) {
      result.kind = DumpStartKind::Found;
      return result;
    }
  }
  result.kind = DumpStartKind::StorageError;
  result.message = "could not open the start file: " + error;
  return result;
}

}  // namespace binlog_streamer
