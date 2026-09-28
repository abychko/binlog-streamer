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

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "config/sHttpSettings.hpp"
#include "http/cHttpListener.hpp"
#include "net/cWakeupPipe.hpp"

namespace binlog_streamer {
namespace {

// A client of the listener under test: connects to the loopback port,
// sends what it is given and reads until the relay closes.
class LoopbackClient {
 public:
  explicit LoopbackClient(std::uint16_t port) {
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    m_connected = connect(m_socket, reinterpret_cast<sockaddr *>(&address),
                          sizeof(address)) == 0;
  }
  ~LoopbackClient() { close(m_socket); }
  bool Connected() const { return m_connected; }
  void Send(const std::string &bytes) {
    ASSERT_EQ(send(m_socket, bytes.data(), bytes.size(), 0),
              static_cast<ssize_t>(bytes.size()));
  }
  // Everything until the peer closes; empty when it does not within the
  // budget, which no test expects.
  std::string ReadAll(
      std::chrono::milliseconds budget = std::chrono::milliseconds(10'000)) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) return out;
      struct pollfd fd{m_socket, POLLIN, 0};
      if (poll(&fd, 1, static_cast<int>(left.count())) <= 0) return out;
      char chunk[4096];
      const ssize_t got = recv(m_socket, chunk, sizeof chunk, 0);
      if (got <= 0) return out;
      out.append(chunk, static_cast<std::size_t>(got));
    }
  }

 private:
  int m_socket = -1;
  bool m_connected = false;
};

class HttpListenerLoopbackTest : public ::testing::Test {
 protected:
  HttpSettings settings;
  std::atomic<bool> stopRequested{false};
  WakeupPipe wakeupPipe;
  std::mutex logMutex;
  std::vector<std::string> log;

  void SetUp() override {
    std::string error;
    ASSERT_TRUE(wakeupPipe.Open(error)) << error;
    settings.listenAddress.family = AddressFamily::Ipv4;
    settings.listenAddress.bytes = {127, 0, 0, 1};
    settings.listenPort = 0;
  }

  HttpListener::Handler Handler() {
    return [](const HttpRequest &request) {
      HttpResponse response;
      if (request.path == "/status.json") {
        response.contentType = "application/json";
        response.body = "{\"query\":\"" + request.query + "\"}\n";
        return response;
      }
      response.status = 404;
      response.body = "no such path\n";
      return response;
    };
  }

  HttpListener::LogFunction Log() {
    return [this](const std::string &line) {
      std::lock_guard<std::mutex> lock(logMutex);
      log.push_back(line);
    };
  }
};

TEST_F(HttpListenerLoopbackTest, ServesOneRequestPerConnectionAndCloses) {
  HttpListener listener(settings, &stopRequested, &wakeupPipe, Handler(),
                        Log());
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;
  ASSERT_NE(listener.Port(), 0);

  LoopbackClient client(listener.Port());
  ASSERT_TRUE(client.Connected());
  client.Send("GET /status.json?x=1 HTTP/1.1\r\nHost: relay\r\n\r\n");
  const std::string reply = client.ReadAll();
  EXPECT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n")) << reply;
  EXPECT_NE(reply.find("\r\nContent-Type: application/json\r\n"),
            std::string::npos)
      << reply;
  EXPECT_TRUE(reply.ends_with("\r\n\r\n{\"query\":\"x=1\"}\n")) << reply;

  LoopbackClient second(listener.Port());
  ASSERT_TRUE(second.Connected());
  second.Send("GET /nowhere HTTP/1.1\r\n\r\n");
  EXPECT_TRUE(second.ReadAll().starts_with("HTTP/1.1 404 Not Found\r\n"));

  listener.Stop();
  std::lock_guard<std::mutex> lock(logMutex);
  ASSERT_EQ(log.size(), 2u);
  EXPECT_TRUE(log[0].ends_with(" GET /status.json 200")) << log[0];
  EXPECT_TRUE(log[0].starts_with("127.0.0.1")) << log[0];
  EXPECT_TRUE(log[1].ends_with(" GET /nowhere 404")) << log[1];
}

TEST_F(HttpListenerLoopbackTest, HeadGetsHeadersOnlyAndOthersGet405) {
  HttpListener listener(settings, &stopRequested, &wakeupPipe, Handler());
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  LoopbackClient head(listener.Port());
  head.Send("HEAD /status.json HTTP/1.1\r\n\r\n");
  const std::string reply = head.ReadAll();
  EXPECT_TRUE(reply.starts_with("HTTP/1.1 200 OK\r\n")) << reply;
  EXPECT_NE(reply.find("\r\nContent-Length: 13\r\n"), std::string::npos)
      << reply;
  EXPECT_TRUE(reply.ends_with("\r\n\r\n")) << reply;

  LoopbackClient post(listener.Port());
  post.Send("POST /status.json HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
  EXPECT_TRUE(
      post.ReadAll().starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
}

TEST_F(HttpListenerLoopbackTest, RefusesGarbageAndOversizedHeads) {
  HttpListener listener(settings, &stopRequested, &wakeupPipe, Handler());
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;

  LoopbackClient garbage(listener.Port());
  // A MySQL client's greeting, not HTTP: what lands here by a wrong port.
  garbage.Send(std::string("\x05\x00\x01\x00 nonsense\r\n\r\n", 18));
  EXPECT_TRUE(garbage.ReadAll().starts_with("HTTP/1.1 400 Bad Request\r\n"));

  LoopbackClient oversized(listener.Port());
  oversized.Send("GET / HTTP/1.1\r\nX-Pad: " + std::string(9000, 'a'));
  EXPECT_TRUE(oversized.ReadAll().starts_with(
      "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST_F(HttpListenerLoopbackTest, StopUnblocksAcceptAndAnIdleConnection) {
  HttpListener listener(settings, &stopRequested, &wakeupPipe, Handler());
  std::string error;
  ASSERT_TRUE(listener.Start(error)) << error;
  LoopbackClient idle(listener.Port());
  ASSERT_TRUE(idle.Connected());
  // A connected client that sends nothing holds a thread until Stop.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto before = std::chrono::steady_clock::now();
  listener.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(2));
  EXPECT_TRUE(idle.ReadAll(std::chrono::milliseconds(500)).empty());
}

TEST_F(HttpListenerLoopbackTest, ReportsAPortItCannotBind) {
  HttpListener first(settings, &stopRequested, &wakeupPipe, Handler());
  std::string error;
  ASSERT_TRUE(first.Start(error)) << error;
  HttpSettings taken = settings;
  taken.listenPort = first.Port();
  HttpListener second(taken, &stopRequested, &wakeupPipe, Handler());
  EXPECT_FALSE(second.Start(error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace binlog_streamer
