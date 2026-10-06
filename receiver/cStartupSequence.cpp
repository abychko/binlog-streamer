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

#include "receiver/cStartupSequence.hpp"

#include "receiver/cStartSetResolver.hpp"

#include <utility>

namespace binlog_streamer {
namespace {

SessionResult MakeStopped(std::string message) {
  SessionResult result;
  result.outcome = SessionOutcome::Stopped;
  result.message = std::move(message);
  return result;
}

}  // namespace

StartupSequence::StartupSequence(
    Transport &transport, const SourceSettings &source,
    const ServerSettings &server, std::string replicaUuid,
    std::string relayName, std::string relayVersion, BinlogProbe &probe,
    RetryOptions options, ReplicaSessionOptions sessionOptions,
    const std::atomic<bool> *stopRequested,
    std::optional<GtidSet> storedStartSet, std::string storedFileName)
    : m_transport(transport),
      m_source(source),
      m_server(server),
      m_replicaUuid(std::move(replicaUuid)),
      m_relayName(std::move(relayName)),
      m_relayVersion(std::move(relayVersion)),
      m_probe(probe),
      m_options(std::move(options)),
      m_sessionOptions(sessionOptions),
      m_stopRequested(stopRequested),
      m_storedStartSet(std::move(storedStartSet)),
      m_storedFileName(std::move(storedFileName)) {}

StartupOutcome StartupSequence::Run() {
  StartSetResolver resolver(m_probe);
  SessionResult last;
  for (unsigned attempt = 1; attempt <= m_options.attempts; ++attempt) {
    auto session = std::make_unique<ReplicaSession>(
        m_transport, m_source, m_server, m_replicaUuid, m_relayName,
        m_relayVersion, m_sessionOptions);
    last = session->Run();
    StartSetResolution resolution;
    if (last.outcome == SessionOutcome::Registered) {
      if (m_storedStartSet.has_value()) {
        resolution.startSet = *m_storedStartSet;
        resolution.selectedFileName = m_storedFileName;
        resolution.usedStoredHistory = true;
        const auto gtidPurged =
            session->QueryText("SELECT @@GLOBAL.gtid_purged");
        if (!gtidPurged.ok) {
          last = gtidPurged.failure;
        } else {
          // While the source still has our last file, what gtid_purged adds was
          // never in a binlog; without the file the source decides (1236 on a
          // gap).
          const auto lastFile = m_probe.PreviousGtidsText(m_storedFileName);
          if (lastFile.ok && gtidPurged.value.has_value()) {
            std::string parseError;
            if (!resolution.startSet.AddFromText(*gtidPurged.value,
                                                 parseError)) {
              last.outcome = SessionOutcome::PermanentFailure;
              last.message = "parsing gtid_purged: " + parseError;
            } else {
              return StartupOutcome{last, std::move(session), resolution};
            }
          } else if (!lastFile.ok && lastFile.failure.outcome ==
                                         SessionOutcome::TransientFailure) {
            last = lastFile.failure;
          } else {
            return StartupOutcome{last, std::move(session), resolution};
          }
        }
      } else {
        const auto gtidExecuted =
            session->QueryText("SELECT @@GLOBAL.gtid_executed");
        const auto gtidPurged =
            gtidExecuted.ok ? session->QueryText("SELECT @@GLOBAL.gtid_purged")
                            : gtidExecuted;
        if (!gtidExecuted.ok) {
          last = gtidExecuted.failure;
        } else if (!gtidPurged.ok) {
          last = gtidPurged.failure;
        } else {
          const auto resolveFailure =
              resolver.Resolve(gtidExecuted.value.value_or(""),
                               gtidPurged.value.value_or(""), resolution);
          if (resolveFailure) {
            last = *resolveFailure;
          } else {
            return StartupOutcome{last, std::move(session), resolution};
          }
        }
      }
    }
    if (last.outcome == SessionOutcome::Stopped)
      return StartupOutcome{last, nullptr, {}};
    if (last.outcome != SessionOutcome::TransientFailure)
      return StartupOutcome{last, nullptr, {}};
    if (m_options.onRetry)
      m_options.onRetry(attempt, m_options.attempts, last.message);
    // Checked after onRetry() and again after sleep(): a stop arriving between
    // attempts must not wait out the remaining attempts.
    if (m_stopRequested != nullptr && m_stopRequested->load()) {
      return StartupOutcome{
          MakeStopped("stop requested between start-up attempts"), nullptr, {}};
    }
    if (attempt < m_options.attempts) m_options.sleep(m_options.interval);
    if (m_stopRequested != nullptr && m_stopRequested->load()) {
      return StartupOutcome{
          MakeStopped("stop requested between start-up attempts"), nullptr, {}};
    }
  }
  last.outcome = SessionOutcome::PermanentFailure;
  last.message = "start-up attempts exhausted after " +
                 std::to_string(m_options.attempts) +
                 " attempt(s): " + last.message;
  return StartupOutcome{last, nullptr, {}};
}

}  // namespace binlog_streamer
