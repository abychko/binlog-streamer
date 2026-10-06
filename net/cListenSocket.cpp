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

#include "net/cListenSocket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace binlog_streamer {
namespace {

bool SetNonBlocking(int fd, std::string &error) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    error = std::strerror(errno);
    return false;
  }
  return true;
}

constexpr int NO_WAKEUP_PIPE_POLL_INTERVAL_MS = 1000;

}  // namespace

ListenSocket::ListenSocket(const std::atomic<bool> *stopRequested,
                           const WakeupPipe *wakeupPipe)
    : m_stopRequested(stopRequested), m_wakeupPipe(wakeupPipe) {}

bool ListenSocket::Open(const IpAddress &address, std::uint16_t port,
                        std::string &error) {
  Close();
  const int family = address.family == AddressFamily::Ipv6 ? AF_INET6 : AF_INET;
  const int fd = socket(family, SOCK_STREAM, 0);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  const int reuse = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    error = std::string("setting SO_REUSEADDR: ") + std::strerror(errno);
    close(fd);
    return false;
  }
  if (family == AF_INET6) {
    // Set explicitly: the platform default differs (Linux 0, macOS/BSD 1).
    const int v6Only = 1;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only)) !=
        0) {
      error = std::string("setting IPV6_V6ONLY: ") + std::strerror(errno);
      close(fd);
      return false;
    }
  }
  if (!SetNonBlocking(fd, error)) {
    close(fd);
    return false;
  }

  if (family == AF_INET) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr, address.bytes.data(), sizeof(addr.sin_addr));
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
        0) {
      error = std::strerror(errno);
      close(fd);
      return false;
    }
  } else {
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    std::memcpy(&addr.sin6_addr, address.bytes.data(), sizeof(addr.sin6_addr));
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) !=
        0) {
      error = std::strerror(errno);
      close(fd);
      return false;
    }
  }

  if (listen(fd, SOMAXCONN) != 0) {
    error = std::strerror(errno);
    close(fd);
    return false;
  }
  m_socket = fd;
  error.clear();
  return true;
}

std::uint16_t ListenSocket::Port() const {
  if (m_socket < 0) return 0;
  struct sockaddr_storage bound{};
  socklen_t length = sizeof bound;
  if (getsockname(m_socket, reinterpret_cast<struct sockaddr *>(&bound),
                  &length) != 0)
    return 0;
  if (bound.ss_family == AF_INET6)
    return ntohs(reinterpret_cast<const sockaddr_in6 &>(bound).sin6_port);
  return ntohs(reinterpret_cast<const sockaddr_in &>(bound).sin_port);
}

AcceptOutcome ListenSocket::Accept(int &acceptedSocket, IpAddress &peerAddress,
                                   std::string &error) {
  if (m_socket < 0) {
    error = "not listening";
    return AcceptOutcome::Failed;
  }
  for (;;) {
    if (m_stopRequested != nullptr && m_stopRequested->load())
      return AcceptOutcome::Interrupted;

    struct pollfd pfds[2];
    pfds[0] = pollfd{m_socket, POLLIN, 0};
    nfds_t pollCount = 1;
    const bool wakeupPolled = m_wakeupPipe != nullptr && m_wakeupPipe->IsOpen();
    if (wakeupPolled) {
      pfds[1] = pollfd{m_wakeupPipe->ReadFd(), POLLIN, 0};
      pollCount = 2;
    }
    const int pollTimeout = wakeupPolled ? -1 : NO_WAKEUP_PIPE_POLL_INTERVAL_MS;
    const int pollResult = poll(pfds, pollCount, pollTimeout);
    if (pollResult < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      return AcceptOutcome::Failed;
    }
    if (pollResult == 0) continue;
    if (wakeupPolled && (pfds[1].revents & POLLIN) != 0) {
      // Not drained: the pipe is shared by every connection thread and a stop
      // request is final.
      return AcceptOutcome::Interrupted;
    }
    if (pfds[0].revents == 0) continue;

    struct sockaddr_storage peer{};
    socklen_t peerLength = sizeof(peer);
    const int fd = accept(m_socket, reinterpret_cast<struct sockaddr *>(&peer),
                          &peerLength);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
          errno == ECONNABORTED)
        continue;
      error = std::strerror(errno);
      return AcceptOutcome::Failed;
    }

    IpAddress address{};
    if (peer.ss_family == AF_INET6) {
      const auto *addr6 = reinterpret_cast<const struct sockaddr_in6 *>(&peer);
      address.family = AddressFamily::Ipv6;
      std::memcpy(address.bytes.data(), &addr6->sin6_addr,
                  sizeof(addr6->sin6_addr));
    } else {
      const auto *addr4 = reinterpret_cast<const struct sockaddr_in *>(&peer);
      address.family = AddressFamily::Ipv4;
      std::memcpy(address.bytes.data(), &addr4->sin_addr,
                  sizeof(addr4->sin_addr));
    }
    acceptedSocket = fd;
    peerAddress = address;
    error.clear();
    return AcceptOutcome::Accepted;
  }
}

void ListenSocket::Close() {
  if (m_socket >= 0) {
    close(m_socket);
    m_socket = -1;
  }
}

}  // namespace binlog_streamer
