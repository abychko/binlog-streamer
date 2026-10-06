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

#include "receiver/cStartSetResolver.hpp"

namespace binlog_streamer {
namespace {
// A malformed GTID text is a permanent failure, never retried.
SessionResult MakeFailure(std::string message) {
  SessionResult failure;
  failure.outcome = SessionOutcome::PermanentFailure;
  failure.message = std::move(message);
  return failure;
}
}  // namespace

std::optional<SessionResult> StartSetResolver::Resolve(
    const std::string &gtidExecutedText, const std::string &gtidPurgedText,
    StartSetResolution &resolution) {
  GtidSet executedSet;
  if (!gtidExecutedText.empty()) {
    std::string parseError;
    if (!executedSet.AddFromText(gtidExecutedText, parseError)) {
      return MakeFailure("parsing gtid_executed: " + parseError);
    }
  }
  const ProbeResult currentFile = m_probe.Probe(executedSet);
  if (!currentFile.ok) return currentFile.failure;

  const auto previousGtids = m_probe.PreviousGtidsText(currentFile.fileName);
  if (!previousGtids.ok) return previousGtids.failure;
  GtidSet startSet;
  if (!previousGtids.text.empty()) {
    std::string parseError;
    if (!startSet.AddFromText(previousGtids.text, parseError)) {
      return MakeFailure("parsing Previous_gtids for '" + currentFile.fileName +
                         "': " + parseError);
    }
  }
  // The source refuses a dump whose set misses any of gtid_purged; GTIDs purged
  // without ever reaching a binlog are not in Previous_gtids.
  if (!gtidPurgedText.empty()) {
    std::string parseError;
    if (!startSet.AddFromText(gtidPurgedText, parseError)) {
      return MakeFailure("parsing gtid_purged: " + parseError);
    }
  }
  resolution.startSet = startSet;
  resolution.selectedFileName = currentFile.fileName;
  return std::nullopt;
}

}  // namespace binlog_streamer
