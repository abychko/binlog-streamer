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
#include "net/cWakeupPipe.hpp"
#include "net/iTransport.hpp"

namespace binlog_streamer {

class TcpTransport : public Transport {
 public:
  // Setting the flag while not blocked in a syscall wakes nothing: the caller
  // must also call Wake(). Pointers are not owned and may be null.
  explicit TcpTransport(const std::atomic<bool> *stopRequested = nullptr,
                        const WakeupPipe *wakeupPipe = nullptr);
  ~TcpTransport() override { Close(); }
  TcpTransport(const TcpTransport &) = delete;
  TcpTransport &operator=(const TcpTransport &) = delete;

  bool Connect(const std::string &host, std::uint16_t port,
               std::chrono::milliseconds timeout, std::string &error) override;
  bool Accept(int acceptedSocket, std::string &error);
  ReadOutcome Read(std::span<std::uint8_t> buffer, std::size_t &bytesRead,
                   std::chrono::milliseconds timeout,
                   std::string &error) override;
  bool Write(std::span<const std::uint8_t> data,
             std::chrono::milliseconds timeout, std::string &error) override;
  void Close() override;

 private:
  int m_socket = -1;
  const std::atomic<bool> *m_stopRequested;
  const WakeupPipe *m_wakeupPipe;
};

}  // namespace binlog_streamer
