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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "config/sHttpSettings.hpp"
#include "http/sHttpRequest.hpp"
#include "http/sHttpResponse.hpp"
#include "net/cWakeupPipe.hpp"

namespace binlog_streamer {

class ListenSocket;
class TcpTransport;

// Serves one request per connection on a thread of its own, the way the
// replica listener serves a replica: a client that stalls holds its own
// thread, not the listener. The handler runs on that thread and has to be
// safe to call from several at once.
class HttpListener {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest &request)>;
  using LogFunction = std::function<void(const std::string &line)>;

  // settings is not copied and has to outlive the listener. Neither
  // stopRequested nor wakeupPipe is owned.
  HttpListener(const HttpSettings &settings,
               const std::atomic<bool> *stopRequested,
               const WakeupPipe *wakeupPipe, Handler handler,
               LogFunction log = {});
  ~HttpListener();
  HttpListener(const HttpListener &) = delete;
  HttpListener &operator=(const HttpListener &) = delete;

  // False (error set) only when binding fails.
  bool Start(std::string &error);
  // The port bound, which listen_port 0 leaves to the system.
  std::uint16_t Port() const;
  void Stop();

 private:
  void AcceptLoop();
  void Serve(TcpTransport &transport, const std::string &peer);
  void ReapFinishedConnections();

  struct ConnectionThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  const HttpSettings &m_settings;
  const std::atomic<bool> *m_stopRequested;
  std::atomic<bool> m_stopping{false};
  const WakeupPipe *m_wakeupPipe;
  Handler m_handler;
  LogFunction m_log;

  // Once the accept thread runs, only it touches the socket; the port is
  // read beforehand.
  std::unique_ptr<ListenSocket> m_listenSocket;
  std::uint16_t m_port = 0;
  std::thread m_acceptThread;
  bool m_started = false;

  std::mutex m_connectionsMutex;
  std::vector<ConnectionThread> m_connections;
};

}  // namespace binlog_streamer
