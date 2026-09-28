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

#include "server/cReplicaListener.hpp"

#include "cDumpSessionRegistry.hpp"
#include "cQueryResponder.hpp"
#include "cReplicaConnection.hpp"
#include "eConnectionRefusal.hpp"
#include "hServerDefaults.hpp"
#include "net/cListenSocket.hpp"
#include "net/cTcpTransport.hpp"
#include "net/eAcceptOutcome.hpp"
#include "server/hServerVersion.hpp"
#include "server/iServerState.hpp"

#include <utility>

namespace binlog_streamer {

ReplicaListener::ReplicaListener(
    const ReplicaSettings &settings, unsigned maxConnections,
    const std::atomic<bool> *stopRequested, const WakeupPipe *wakeupPipe,
    LogFunction log, ServerIdentity identity, const ServerState *state,
    BinlogStorageReader *storageReader, const TlsContext *tls,
    RelayStatusTracker *status, std::chrono::microseconds sendLinger)
    : m_settings(settings),
      m_clients(settings.clients),
      m_maxConnections(maxConnections),
      m_stopRequested(stopRequested),
      m_wakeupPipe(wakeupPipe),
      m_log(std::move(log)),
      m_queryResponder(std::make_unique<QueryResponder>(identity, state)),
      m_dumpSessions(std::make_unique<DumpSessionRegistry>()),
      m_storageReader(storageReader),
      m_identity(std::move(identity)),
      m_state(state),
      m_tls(tls),
      m_status(status),
      m_sendLinger(sendLinger) {}

ReplicaListener::~ReplicaListener() { Stop(); }

bool ReplicaListener::Start(std::string &error) {
  m_listenSocket =
      std::make_unique<ListenSocket>(m_stopRequested, m_wakeupPipe);
  if (!m_listenSocket->Open(m_settings.listenAddress, m_settings.listenPort,
                            error)) {
    m_listenSocket.reset();
    return false;
  }
  m_started = true;
  m_acceptThread = std::thread([this] { AcceptLoop(); });
  error.clear();
  return true;
}

void ReplicaListener::Stop() {
  if (!m_started) return;
  m_started = false;
  // Unconditional: AcceptLoop() and every connection's Read() notice a stop
  // only by the wakeup pipe becoming readable, not by re-checking
  // *m_stopRequested; a second Wake() while one is pending is a harmless no-op.
  m_stopping.store(true);
  if (m_wakeupPipe != nullptr) m_wakeupPipe->Wake();

  if (m_acceptThread.joinable()) m_acceptThread.join();

  std::lock_guard<std::mutex> lock(m_connectionsMutex);
  for (auto &connection : m_connections)
    if (connection.thread.joinable()) connection.thread.join();
  m_connections.clear();
}

void ReplicaListener::AcceptLoop() {
  for (;;) {
    ReapFinishedConnections();

    int acceptedSocket = -1;
    IpAddress peerAddress{};
    std::string error;
    const AcceptOutcome outcome =
        m_listenSocket->Accept(acceptedSocket, peerAddress, error);
    if (outcome == AcceptOutcome::Interrupted) {
      m_stopping.store(true);
      break;
    }
    if (outcome == AcceptOutcome::Failed) {
      if (m_log) m_log("accept failed: " + error);
      continue;  // transient (e.g. a resource limit) - one failed accept does
                 // not end the listener
    }

    auto transport =
        std::make_unique<TcpTransport>(m_stopRequested, m_wakeupPipe);
    std::string acceptError;
    if (!transport->Accept(acceptedSocket, acceptError)) {
      if (m_log) m_log("accept failed: " + acceptError);
      continue;
    }

    // Counted synchronously in the accept loop, not inside the spawned
    // thread, so two connections arriving close together cannot both
    // read the same under-the-cap count before either increments it.
    const bool tooMany = m_activeConnections.load() >= m_maxConnections;
    if (!tooMany) m_activeConnections.fetch_add(1);

    auto finished = std::make_shared<std::atomic<bool>>(false);
    ReplicaClientList::Snapshot clients = m_clients.Current();
    std::atomic<unsigned> &activeConnections = m_activeConnections;
    // Read per connection, not once at start-up: the first file is
    // stored while replicas are already free to connect.
    std::string serverVersion = ServerVersionString(
        m_state != nullptr ? m_state->SourceVersion() : std::string(),
        m_identity.relayName, m_identity.relayVersion);
    ConnectionRefusal refusal = ConnectionRefusal::None;
    if (tooMany)
      refusal = ConnectionRefusal::TooManyConnections;
    else if (serverVersion.empty())
      refusal = ConnectionRefusal::NotReady;
    LogFunction log = m_log;
    // Everything named here outlives every connection: Stop() joins them first.
    ConnectionServices services{m_queryResponder.get(),
                                m_storageReader,
                                m_dumpSessions.get(),
                                m_identity.serverUuid,
                                m_identity.serverId,
                                &m_stopping,
                                m_settings.compression,
                                m_tls,
                                m_settings.requireSecureTransport,
                                m_status,
                                m_sendLinger};
    std::thread connectionThread(
        [transport = std::move(transport), peerAddress,
         clients = std::move(clients), serverVersion = std::move(serverVersion),
         log = std::move(log), tooMany, refusal, finished, &activeConnections,
         services = std::move(services)]() mutable {
          ReplicaConnection connection(*transport, peerAddress,
                                       std::move(clients),
                                       std::move(serverVersion), std::move(log),
                                       refusal, nullptr, std::move(services));
          connection.Run();
          if (!tooMany) activeConnections.fetch_sub(1);
          finished->store(true);
        });

    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    m_connections.push_back(
        ConnectionThread{std::move(connectionThread), std::move(finished)});
  }
  m_listenSocket->Close();
}

void ReplicaListener::ReapFinishedConnections() {
  std::lock_guard<std::mutex> lock(m_connectionsMutex);
  for (auto it = m_connections.begin(); it != m_connections.end();) {
    if (it->finished->load()) {
      it->thread.join();
      it = m_connections.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace binlog_streamer
