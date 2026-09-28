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

#include <atomic>
#include <optional>
#include <string>
#include "config/sServerSettings.hpp"
#include "config/sSourceSettings.hpp"
#include "gtid/cGtidSet.hpp"
#include "net/iTransport.hpp"
#include "receiver/iBinlogProbe.hpp"
#include "receiver/sReplicaSessionOptions.hpp"
#include "receiver/sRetryOptions.hpp"
#include "receiver/sStartupOutcome.hpp"

namespace binlog_streamer {

// Every attempt is a fresh ReplicaSession: a transient failure partway
// through a previous attempt leaves its channel/transport unsafe to
// resume.
class StartupSequence {
 public:
  // When storedStartSet/storedFileName are set, a successful
  // registration builds StartSetResolution from them instead of
  // running StartSetResolver's probes.
  StartupSequence(Transport &transport, const SourceSettings &source,
                  const ServerSettings &server, std::string replicaUuid,
                  std::string relayName, std::string relayVersion,
                  BinlogProbe &probe, RetryOptions options = {},
                  ReplicaSessionOptions sessionOptions = {},
                  const std::atomic<bool> *stopRequested = nullptr,
                  std::optional<GtidSet> storedStartSet = std::nullopt,
                  std::string storedFileName = {});

  // Registered is the only outcome with both session and resolution
  // meaningful. TransientFailure never escapes this method - it
  // surfaces as PermanentFailure once attempts are exhausted.
  StartupOutcome Run();

 private:
  Transport &m_transport;
  // Own the settings so their lifetime does not depend on the caller.
  const SourceSettings m_source;
  const ServerSettings m_server;
  std::string m_replicaUuid;
  std::string m_relayName;
  std::string m_relayVersion;
  BinlogProbe &m_probe;
  RetryOptions m_options;
  ReplicaSessionOptions m_sessionOptions;
  const std::atomic<bool> *m_stopRequested;
  std::optional<GtidSet> m_storedStartSet;
  std::string m_storedFileName;
};

}  // namespace binlog_streamer
