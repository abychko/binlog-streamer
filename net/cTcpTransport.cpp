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

#include "net/cTcpTransport.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <limits>

namespace binlog_streamer {
namespace {

int ClampToPollTimeout(std::chrono::milliseconds timeout) {
  const auto count = timeout.count();
  if (count < 0) return 0;
  if (count > std::numeric_limits<int>::max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(count);
}

bool SetNonBlocking(int fd, std::string &error) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    error = std::strerror(errno);
    return false;
  }
  return true;
}

bool PrepareSocket(int fd, std::string &error) {
  if (!SetNonBlocking(fd, error)) return false;
#ifdef SO_NOSIGPIPE
  // Sending to a closed peer raises SIGPIPE; macOS has SO_NOSIGPIPE but not
  // MSG_NOSIGNAL, Linux the reverse, so both are applied.
  const int noSigPipe = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) !=
      0) {
    error = std::string("setting SO_NOSIGPIPE: ") + std::strerror(errno);
    return false;
  }
#endif
  return true;
}

class Deadline {
 public:
  explicit Deadline(std::chrono::milliseconds timeout)
      : m_deadline(std::chrono::steady_clock::now() + timeout) {}

  std::chrono::milliseconds Remaining() const {
    const auto remaining = m_deadline - std::chrono::steady_clock::now();
    return remaining > std::chrono::steady_clock::duration::zero()
               ? std::chrono::duration_cast<std::chrono::milliseconds>(
                     remaining)
               : std::chrono::milliseconds::zero();
  }

  bool Expired() const {
    return Remaining() == std::chrono::milliseconds::zero();
  }

 private:
  std::chrono::steady_clock::time_point m_deadline;
};

}  // namespace

TcpTransport::TcpTransport(const std::atomic<bool> *stopRequested,
                           const WakeupPipe *wakeupPipe)
    : m_stopRequested(stopRequested), m_wakeupPipe(wakeupPipe) {}

bool TcpTransport::Connect(const std::string &host, std::uint16_t port,
                           std::chrono::milliseconds timeout,
                           std::string &error) {
  Close();

  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *resolved = nullptr;
  const int gaiStatus = getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                                    &hints, &resolved);
  if (gaiStatus != 0) {
    error = std::string("resolving ") + host + ": " + gai_strerror(gaiStatus);
    return false;
  }

  bool connected = false;
  for (const struct addrinfo *candidate = resolved; candidate != nullptr;
       candidate = candidate->ai_next) {
    const int fd = socket(candidate->ai_family, candidate->ai_socktype,
                          candidate->ai_protocol);
    if (fd < 0) {
      error = std::strerror(errno);
      continue;
    }
    if (!PrepareSocket(fd, error)) {
      close(fd);
      continue;
    }
    const Deadline deadline(timeout);
    int rc = connect(fd, candidate->ai_addr, candidate->ai_addrlen);
    if (rc != 0 && errno == EINTR) {
      // After EINTR the connect continues in the background; calling connect()
      // again is undefined (POSIX).
      errno = EINPROGRESS;
    }
    if (rc == 0) {
      m_socket = fd;
      connected = true;
      break;
    }
    if (errno != EINPROGRESS) {
      error = std::strerror(errno);
      close(fd);
      continue;
    }
    bool candidateFailed = false;
    for (;;) {
      struct pollfd pfd{fd, POLLOUT, 0};
      const int pollResult =
          poll(&pfd, 1, ClampToPollTimeout(deadline.Remaining()));
      if (pollResult < 0) {
        if (errno == EINTR) {
          if (deadline.Expired()) {
            error = "connect timed out";
            candidateFailed = true;
            break;
          }
          continue;
        }
        error = std::strerror(errno);
        candidateFailed = true;
        break;
      }
      if (pollResult == 0) {
        error = "connect timed out";
        candidateFailed = true;
        break;
      }
      int socketError = 0;
      socklen_t socketErrorLength = sizeof(socketError);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError,
                     &socketErrorLength) < 0 ||
          socketError != 0) {
        error = socketError != 0 ? std::strerror(socketError)
                                 : std::strerror(errno);
        candidateFailed = true;
      }
      break;
    }
    if (candidateFailed) {
      close(fd);
      continue;
    }
    m_socket = fd;
    connected = true;
    break;
  }
  freeaddrinfo(resolved);
  if (!connected) {
    if (error.empty()) error = "no address resolved";
    return false;
  }
  error.clear();
  return true;
}

bool TcpTransport::Accept(int acceptedSocket, std::string &error) {
  Close();
  if (!PrepareSocket(acceptedSocket, error)) {
    close(acceptedSocket);
    return false;
  }
  m_socket = acceptedSocket;
  error.clear();
  return true;
}

ReadOutcome TcpTransport::Read(std::span<std::uint8_t> buffer,
                               std::size_t &bytesRead,
                               std::chrono::milliseconds timeout,
                               std::string &error) {
  bytesRead = 0;
  if (m_socket < 0) {
    error = "not connected";
    return ReadOutcome::Failed;
  }
  const Deadline deadline(timeout);
  for (;;) {
    if (m_stopRequested != nullptr && m_stopRequested->load())
      return ReadOutcome::Interrupted;

    struct pollfd pfds[2];
    pfds[0] = pollfd{m_socket, POLLIN, 0};
    nfds_t pollCount = 1;
    const bool wakeupPolled = m_wakeupPipe != nullptr && m_wakeupPipe->IsOpen();
    if (wakeupPolled) {
      pfds[1] = pollfd{m_wakeupPipe->ReadFd(), POLLIN, 0};
      pollCount = 2;
    }
    const int pollResult =
        poll(pfds, pollCount, ClampToPollTimeout(deadline.Remaining()));
    if (pollResult < 0) {
      if (errno == EINTR) {
        if (m_stopRequested != nullptr && m_stopRequested->load())
          return ReadOutcome::Interrupted;
        if (deadline.Expired()) return ReadOutcome::TimedOut;
        continue;
      }
      error = std::strerror(errno);
      return ReadOutcome::Failed;
    }
    if (pollResult == 0) return ReadOutcome::TimedOut;
    if (wakeupPolled && (pfds[1].revents & POLLIN) != 0) {
      // Self-pipe: closes the race a flag-and-EINTR check leaves open. Not
      // drained: the pipe is shared by every connection and Wake() is called
      // once, at process stop.
      return ReadOutcome::Interrupted;
    }
    if (wakeupPolled && pfds[0].revents == 0) continue;

    const ssize_t count = recv(m_socket, buffer.data(), buffer.size(), 0);
    if (count < 0) {
      if (errno == EINTR) {
        if (m_stopRequested != nullptr && m_stopRequested->load())
          return ReadOutcome::Interrupted;
        if (deadline.Expired()) return ReadOutcome::TimedOut;
        continue;
      }
      error = std::strerror(errno);
      return ReadOutcome::Failed;
    }
    if (count == 0) return ReadOutcome::Closed;
    bytesRead = static_cast<std::size_t>(count);
    return ReadOutcome::Data;
  }
}

bool TcpTransport::Write(std::span<const std::uint8_t> data,
                         std::chrono::milliseconds timeout,
                         std::string &error) {
  if (m_socket < 0) {
    error = "not connected";
    return false;
  }
  const Deadline deadline(timeout);
  std::size_t offset = 0;
  bool wait = false;
  while (offset < data.size()) {
    if (wait) {
      struct pollfd pfd{m_socket, POLLOUT, 0};
      const int pollResult =
          poll(&pfd, 1, ClampToPollTimeout(deadline.Remaining()));
      if (pollResult < 0) {
        if (errno == EINTR) {
          if (deadline.Expired()) {
            error = "write timed out";
            return false;
          }
          continue;
        }
        error = std::strerror(errno);
        return false;
      }
      if (pollResult == 0) {
        error = "write timed out";
        return false;
      }
      wait = false;
    }
#ifdef MSG_NOSIGNAL
    const int sendFlags = MSG_NOSIGNAL;
#else
    const int sendFlags = 0;
#endif
    const ssize_t count =
        send(m_socket, data.data() + offset, data.size() - offset, sendFlags);
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        wait = true;
        continue;
      }
      if (errno == EINTR) {
        if (deadline.Expired()) {
          error = "write timed out";
          return false;
        }
        continue;
      }
      error = std::strerror(errno);
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

void TcpTransport::Close() {
  if (m_socket >= 0) {
    close(m_socket);
    m_socket = -1;
  }
}

}  // namespace binlog_streamer
