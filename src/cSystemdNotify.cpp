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

#include "cSystemdNotify.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace binlog_streamer {

std::optional<std::string> SystemdNotify::Send(std::string_view message) {
  const char *path = std::getenv("NOTIFY_SOCKET");
  if (path == nullptr || *path == '\0') return std::nullopt;
  const std::string_view name(path);
  if (name.front() != '/' && name.front() != '@')
    return "NOTIFY_SOCKET " + std::string(name) + " is not a socket path";

  sockaddr_un address{};
  if (name.size() >= sizeof(address.sun_path))
    return "NOTIFY_SOCKET " + std::string(name) + " is too long";
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, name.data(), name.size());
  // '@' names a Linux abstract socket: its name starts with a zero byte.
  if (name.front() == '@') address.sun_path[0] = '\0';
  const auto length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size());

  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return std::string("socket: ") + std::strerror(errno);
  const ssize_t sent =
      sendto(fd, message.data(), message.size(), 0,
             reinterpret_cast<const sockaddr *>(&address), length);
  const int sendError = errno;
  close(fd);
  if (sent < 0)
    return "sending to " + std::string(name) + ": " + std::strerror(sendError);
  return std::nullopt;
}

}  // namespace binlog_streamer
