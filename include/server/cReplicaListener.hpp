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
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "config/sReplicaSettings.hpp"
#include "net/cWakeupPipe.hpp"
#include "server/cReplicaClientList.hpp"
#include "server/iServerState.hpp"
#include "server/sServerIdentity.hpp"

namespace binlog_streamer {

class BinlogStorageReader;
class TlsContext;
class DumpSessionRegistry;
class QueryResponder;
class RelayStatusTracker;

class ListenSocket;

class ReplicaListener {
 public:
  using LogFunction = std::function<void(const std::string &line)>;

  // settings is not copied and must outlive the listener; stopRequested and
  // wakeupPipe are not owned. With no state, or none that knows a source
  // version yet, every connection gets ERR 3168 instead of a greeting.
  ReplicaListener(const ReplicaSettings &settings, unsigned maxConnections,
                  const std::atomic<bool> *stopRequested,
                  const WakeupPipe *wakeupPipe, LogFunction log = {},
                  ServerIdentity identity = {},
                  const ServerState *state = nullptr,
                  BinlogStorageReader *storageReader = nullptr,
                  const TlsContext *tls = nullptr,
                  RelayStatusTracker *status = nullptr,
                  std::chrono::microseconds sendLinger = {});
  ~ReplicaListener();
  ReplicaListener(const ReplicaListener &) = delete;
  ReplicaListener &operator=(const ReplicaListener &) = delete;

  bool Start(std::string &error);

  void Stop();

  ReplicaClientList &Clients() { return m_clients; }

 private:
  void AcceptLoop();
  void ReapFinishedConnections();

  struct ConnectionThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  const ReplicaSettings &m_settings;
  ReplicaClientList m_clients;
  unsigned m_maxConnections;
  const std::atomic<bool> *m_stopRequested;
  // Also set by Stop() itself, so an idle dump does not make Stop() join
  // forever.
  std::atomic<bool> m_stopping{false};
  const WakeupPipe *m_wakeupPipe;
  LogFunction m_log;
  std::unique_ptr<QueryResponder> m_queryResponder;
  std::unique_ptr<DumpSessionRegistry> m_dumpSessions;
  BinlogStorageReader *m_storageReader;
  ServerIdentity m_identity;
  const ServerState *m_state;
  const TlsContext *m_tls;
  RelayStatusTracker *m_status;
  std::chrono::microseconds m_sendLinger;

  std::unique_ptr<ListenSocket> m_listenSocket;
  std::thread m_acceptThread;
  bool m_started = false;

  std::mutex m_connectionsMutex;
  std::vector<ConnectionThread> m_connections;
  std::atomic<unsigned> m_activeConnections{0};
};

}  // namespace binlog_streamer
