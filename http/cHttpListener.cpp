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

#include "http/cHttpListener.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <utility>
#include "config/cIpAddressText.hpp"
#include "http/cHttpRequestParser.hpp"
#include "http/cHttpResponseWriter.hpp"
#include "http/hHttpDefaults.hpp"
#include "net/cListenSocket.hpp"
#include "net/cTcpTransport.hpp"
#include "net/eAcceptOutcome.hpp"

namespace binlog_streamer {

HttpListener::HttpListener(const HttpSettings &settings,
                           const std::atomic<bool> *stopRequested,
                           const WakeupPipe *wakeupPipe, Handler handler,
                           LogFunction log)
    : m_settings(settings),
      m_stopRequested(stopRequested),
      m_wakeupPipe(wakeupPipe),
      m_handler(std::move(handler)),
      m_log(std::move(log)) {}

HttpListener::~HttpListener() { Stop(); }

bool HttpListener::Start(std::string &error) {
  m_listenSocket = std::make_unique<ListenSocket>(&m_stopping, m_wakeupPipe);
  if (!m_listenSocket->Open(m_settings.listenAddress, m_settings.listenPort,
                            error)) {
    m_listenSocket.reset();
    return false;
  }
  m_port = m_listenSocket->Port();
  m_started = true;
  m_acceptThread = std::thread([this] { AcceptLoop(); });
  error.clear();
  return true;
}

std::uint16_t HttpListener::Port() const { return m_port; }

void HttpListener::Stop() {
  if (!m_started) return;
  m_started = false;
  m_stopping.store(true);
  if (m_wakeupPipe != nullptr) m_wakeupPipe->Wake();
  if (m_acceptThread.joinable()) m_acceptThread.join();
  std::lock_guard<std::mutex> lock(m_connectionsMutex);
  for (auto &connection : m_connections)
    if (connection.thread.joinable()) connection.thread.join();
  m_connections.clear();
}

void HttpListener::AcceptLoop() {
  for (;;) {
    ReapFinishedConnections();
    // The process's stop flag is polled here, not by the listen socket: the
    // socket watches m_stopping, which Stop() sets from either.
    if (m_stopRequested != nullptr && m_stopRequested->load())
      m_stopping.store(true);
    if (m_stopping.load()) break;

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
      continue;
    }
    auto transport = std::make_unique<TcpTransport>(&m_stopping, m_wakeupPipe);
    std::string acceptError;
    if (!transport->Accept(acceptedSocket, acceptError)) {
      if (m_log) m_log("accept failed: " + acceptError);
      continue;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread connectionThread([this, transport = std::move(transport),
                                  peer = IpAddressText::Format(peerAddress),
                                  finished]() mutable {
      Serve(*transport, peer);
      transport->Close();
      finished->store(true);
    });
    std::lock_guard<std::mutex> lock(m_connectionsMutex);
    m_connections.push_back(
        ConnectionThread{std::move(connectionThread), std::move(finished)});
  }
  m_listenSocket->Close();
}

void HttpListener::Serve(TcpTransport &transport, const std::string &peer) {
  std::string head;
  std::string error;
  std::uint8_t chunk[1024];
  HttpResponse response;
  bool headOnly = false;
  for (;;) {
    std::size_t bytesRead = 0;
    const ReadOutcome outcome =
        transport.Read(chunk, bytesRead, HTTP_READ_TIMEOUT, error);
    if (outcome == ReadOutcome::TimedOut) {
      response.status = 408;
      response.body = "request timeout\n";
      break;
    }
    if (outcome != ReadOutcome::Data) return;
    head.append(reinterpret_cast<const char *>(chunk), bytesRead);
    const auto end = head.find("\r\n\r\n");
    if (end == std::string::npos) {
      if (head.size() > MAX_HTTP_REQUEST_HEAD) {
        response.status = 431;
        response.body = "request head too large\n";
        break;
      }
      continue;
    }
    HttpRequest request;
    if (!HttpRequestParser::Parse(std::string_view(head).substr(0, end),
                                  request, error)) {
      response.status = 400;
      response.body = error + '\n';
      break;
    }
    headOnly = request.method == "HEAD";
    if (request.method != "GET" && !headOnly) {
      response.status = 405;
      response.body = "method not allowed\n";
      break;
    }
    response = m_handler(request);
    if (m_log) {
      m_log(peer + " " + request.method + " " + request.path + " " +
            std::to_string(response.status));
    }
    break;
  }
  const std::string bytes = HttpResponseWriter::Serialize(response, headOnly);
  transport.Write(
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()),
      HTTP_WRITE_TIMEOUT, error);
}

void HttpListener::ReapFinishedConnections() {
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
