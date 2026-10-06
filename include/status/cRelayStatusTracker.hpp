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

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "status/cReplicaRegistration.hpp"
#include "status/cStreamProgress.hpp"
#include "status/iStorageFacts.hpp"
#include "status/sRelayStatus.hpp"
#include "status/sReplicaFacts.hpp"

namespace binlog_streamer {

// Fed from the source loop and every replica's thread, read by the
// status page. Source() and each registration's Progress() take no lock
// per event; everything else is rare and does.
class RelayStatusTracker {
 public:
  RelayStatusTracker(std::string name, std::string version,
                     unsigned maxConnections, const StorageFacts *storage);

  void SetSourceAddress(std::string address);
  void SourceAttempt(unsigned attempt);
  void SourceConnected(std::uint32_t serverId, std::string serverUuid,
                       std::string version, bool tls, std::string compression,
                       std::uint64_t clock);
  void SourceLost();
  StreamProgress &Source() { return m_source; }

  std::shared_ptr<ReplicaRegistration> RegisterReplica(ReplicaFacts facts);
  void UnregisterReplica(
      const std::shared_ptr<ReplicaRegistration> &registration);

  RelayStatus Snapshot() const;

 private:
  std::optional<std::uint64_t> SourceClockNow(
      std::chrono::steady_clock::time_point now) const;

  std::string m_name;
  std::string m_version;
  unsigned m_maxConnections;
  const StorageFacts *m_storage;
  std::chrono::system_clock::time_point m_startedAt;
  std::chrono::steady_clock::time_point m_startedAtSteady;

  mutable std::mutex m_mutex;
  RelayState m_state = RelayState::Starting;
  std::string m_sourceAddress;
  bool m_connected = false;
  std::optional<std::chrono::system_clock::time_point> m_since;
  unsigned m_attempt = 0;
  std::uint32_t m_serverId = 0;
  std::string m_serverUuid;
  std::string m_sourceVersion;
  bool m_sourceTls = false;
  std::string m_sourceCompression;
  std::uint64_t m_sourceClock = 0;
  std::chrono::steady_clock::time_point m_sourceClockReadAt{};
  std::vector<std::shared_ptr<ReplicaRegistration>> m_replicas;

  StreamProgress m_source;
};

}  // namespace binlog_streamer
