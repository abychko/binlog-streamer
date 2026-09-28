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
#include <string>
#include "config/sIpAddress.hpp"
#include "net/cWakeupPipe.hpp"
#include "net/eAcceptOutcome.hpp"

namespace binlog_streamer {

// Not part of net/'s Transport hierarchy, which models an already-established
// byte stream only.
class ListenSocket {
 public:
  // Same self-pipe technique as TcpTransport::Read(): Accept() polls the
  // same wakeup pipe every connection thread polls, so a stop signal
  // unblocks a listener in accept() too. Neither pointer is owned.
  ListenSocket(const std::atomic<bool> *stopRequested,
               const WakeupPipe *wakeupPipe);
  ~ListenSocket() { Close(); }
  ListenSocket(const ListenSocket &) = delete;
  ListenSocket &operator=(const ListenSocket &) = delete;

  // address's family selects the socket's family - listens on exactly the
  // one address named, not dual-stack. SO_REUSEADDR is always set, so a
  // restarted relay can rebind a port left in TIME_WAIT.
  bool Open(const IpAddress &address, std::uint16_t port, std::string &error);

  // On Accepted, acceptedSocket is still blocking and not yet protocol-ready
  // (TcpTransport::Accept() finishes that); peerAddress names the peer.
  AcceptOutcome Accept(int &acceptedSocket, IpAddress &peerAddress,
                       std::string &error);

  // The port bound, which a port of 0 leaves to the system; 0 when not
  // open.
  std::uint16_t Port() const;

  void Close();

 private:
  int m_socket = -1;
  const std::atomic<bool> *m_stopRequested;
  const WakeupPipe *m_wakeupPipe;
};

}  // namespace binlog_streamer
