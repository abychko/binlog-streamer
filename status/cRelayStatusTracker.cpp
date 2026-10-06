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

#include "status/cRelayStatusTracker.hpp"

#include <algorithm>
#include <utility>

namespace binlog_streamer {
namespace {

std::uint64_t UnixSeconds(std::chrono::system_clock::time_point at) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch())
          .count());
}

std::uint64_t SecondsBehind(std::uint64_t clock, std::uint32_t timestamp) {
  return clock > timestamp ? clock - timestamp : 0;
}

}  // namespace

RelayStatusTracker::RelayStatusTracker(std::string name, std::string version,
                                       unsigned maxConnections,
                                       const StorageFacts *storage)
    : m_name(std::move(name)),
      m_version(std::move(version)),
      m_maxConnections(maxConnections),
      m_storage(storage),
      m_startedAt(std::chrono::system_clock::now()),
      m_startedAtSteady(std::chrono::steady_clock::now()) {}

void RelayStatusTracker::SetSourceAddress(std::string address) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_sourceAddress = std::move(address);
}

void RelayStatusTracker::SourceAttempt(unsigned attempt) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_attempt = attempt;
}

void RelayStatusTracker::SourceConnected(std::uint32_t serverId,
                                         std::string serverUuid,
                                         std::string version, bool tls,
                                         std::string compression,
                                         std::uint64_t clock) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_state = RelayState::Serving;
  m_connected = true;
  m_since = std::chrono::system_clock::now();
  m_attempt = 0;
  m_serverId = serverId;
  m_serverUuid = std::move(serverUuid);
  m_sourceVersion = std::move(version);
  m_sourceTls = tls;
  m_sourceCompression = std::move(compression);
  m_sourceClock = clock;
  m_sourceClockReadAt = std::chrono::steady_clock::now();
}

void RelayStatusTracker::SourceLost() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_state = RelayState::Reconnecting;
  m_connected = false;
  m_since = std::chrono::system_clock::now();
}

std::shared_ptr<ReplicaRegistration> RelayStatusTracker::RegisterReplica(
    ReplicaFacts facts) {
  auto registration = std::make_shared<ReplicaRegistration>(std::move(facts));
  std::lock_guard<std::mutex> lock(m_mutex);
  m_replicas.push_back(registration);
  return registration;
}

void RelayStatusTracker::UnregisterReplica(
    const std::shared_ptr<ReplicaRegistration> &registration) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_replicas.erase(
      std::remove(m_replicas.begin(), m_replicas.end(), registration),
      m_replicas.end());
}

std::optional<std::uint64_t> RelayStatusTracker::SourceClockNow(
    std::chrono::steady_clock::time_point now) const {
  if (!m_connected || m_sourceClock == 0) return std::nullopt;
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - m_sourceClockReadAt);
  return m_sourceClock + static_cast<std::uint64_t>(elapsed.count());
}

RelayStatus RelayStatusTracker::Snapshot() const {
  const auto now = std::chrono::system_clock::now();
  const auto nowSteady = std::chrono::steady_clock::now();
  RelayStatus status;
  status.name = m_name;
  status.version = m_version;
  status.startedAt = UnixSeconds(m_startedAt);
  status.now = UnixSeconds(now);
  status.uptimeSeconds = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(nowSteady -
                                                       m_startedAtSteady)
          .count());
  status.maxConnections = m_maxConnections;

  std::vector<std::shared_ptr<ReplicaRegistration>> replicas;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    status.state = m_state;
    SourceStatus &source = status.source;
    source.address = m_sourceAddress;
    source.connected = m_connected;
    if (m_since) source.since = UnixSeconds(*m_since);
    source.attempt = m_attempt;
    source.serverId = m_serverId;
    source.serverUuid = m_serverUuid;
    source.version = m_sourceVersion;
    source.tls = m_sourceTls;
    source.compression = m_sourceCompression;
    source.clock = SourceClockNow(nowSteady);
    replicas = m_replicas;
  }
  // Each position is read no earlier than the ones it runs ahead of, so a
  // position moving on between the reads only adds to a distance and never
  // turns it negative.
  status.replicas.reserve(replicas.size());
  for (const auto &registration : replicas) {
    ReplicaStatus replica;
    replica.facts = registration->Facts();
    replica.reportHost = registration->ReportHost();
    replica.since = UnixSeconds(registration->Since());
    if (registration->Dumping()) {
      replica.sent = registration->Progress().Read();
      replica.state = replica.sent.idle ? ReplicaState::Streaming
                                        : ReplicaState::CatchingUp;
      replica.readFromDisk = registration->Progress().FromDisk();
    }
    status.replicas.push_back(std::move(replica));
  }

  StorageStatus &storage = status.storage;
  if (m_storage != nullptr) {
    storage.files = m_storage->Files();
    storage.bytes = m_storage->Bytes();
    storage.maxBytes = m_storage->MaxBytes();
    status.memory = m_storage->Memory();
    m_storage->Published(storage.file, storage.position);
  }

  SourceStatus &source = status.source;
  source.seen = m_source.Read();
  if (source.connected && source.clock && source.seen.timestamp != 0) {
    source.behindSeconds =
        source.seen.idle ? 0
                         : SecondsBehind(*source.clock, source.seen.timestamp);
  }

  if (m_storage != nullptr && !source.seen.file.empty()) {
    // The catalog carries the open file at the size it had when it was added;
    // what has been received into it since is on disk too.
    const auto seenOffset =
        m_storage->Offset(source.seen.file, source.seen.position);
    if (seenOffset && *seenOffset > storage.bytes) storage.bytes = *seenOffset;
    storage.behindBytes = m_storage->Distance(
        storage.file, storage.position, source.seen.file, source.seen.position);
  }

  for (ReplicaStatus &replica : status.replicas) {
    if (replica.state == ReplicaState::Connected) continue;
    if (m_storage != nullptr)
      replica.behindBytes =
          m_storage->Distance(replica.sent.file, replica.sent.position,
                              storage.file, storage.position);
    if (replica.sent.idle) {
      replica.behindSeconds = 0;
    } else if (source.clock && replica.sent.timestamp != 0) {
      replica.behindSeconds =
          SecondsBehind(*source.clock, replica.sent.timestamp);
    }
  }
  return status;
}

}  // namespace binlog_streamer
