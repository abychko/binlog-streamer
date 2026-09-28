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

// main.cpp's only touchpoint with server/.
class ReplicaListener {
 public:
  using LogFunction = std::function<void(const std::string &line)>;

  // settings is not copied; the caller (Configuration, owned by main()
  // for the whole run) must keep it alive. Neither stopRequested nor
  // wakeupPipe is owned here. maxConnections is settings.yml's
  // server.max_connections: the connection past it gets ERR 1040. With no
  // state, or with one that knows no source version yet, every connection
  // gets ERR 3168 instead of a greeting. sendLinger is settings.yml's
  // server.send_linger, given to every dump.
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

  // False (error set) only if binding fails - an empty clients list is
  // not a failure, just every connection getting ERR 1130.
  bool Start(std::string &error);

  // Safe to call when Start() was never called, more than once, or not
  // at all - also runs from the destructor.
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
  // What a dump loop polls instead of *m_stopRequested: also set by
  // Stop() itself, so a dump idly waiting for events does not make
  // Stop() join it forever.
  std::atomic<bool> m_stopping{false};
  const WakeupPipe *m_wakeupPipe;
  LogFunction m_log;
  std::unique_ptr<QueryResponder> m_queryResponder;
  std::unique_ptr<DumpSessionRegistry> m_dumpSessions;
  BinlogStorageReader *m_storageReader;
  ServerIdentity m_identity;
  // Asked once per accepted connection for the source's version: the
  // greeting names it, and its absence refuses the connection.
  const ServerState *m_state;
  // The relay's certificate, offered to every connection; null offers
  // none.
  const TlsContext *m_tls;
  // Every logged-in replica is registered here; null registers none.
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
