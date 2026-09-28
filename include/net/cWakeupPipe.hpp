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

#include <string>

namespace binlog_streamer {

// Self-pipe trick: a byte on the write end makes poll() on the read end
// return immediately. Wake() is safe from a signal handler; every other
// method must run on the normal thread.
class WakeupPipe {
 public:
  WakeupPipe() = default;
  ~WakeupPipe() { Close(); }
  WakeupPipe(const WakeupPipe &) = delete;
  WakeupPipe &operator=(const WakeupPipe &) = delete;

  // Both ends are non-blocking (Wake() must never block, even from a
  // signal handler) and close-on-exec.
  bool Open(std::string &error);

  // Async-signal-safe: only write() on an fd already open and
  // non-blocking. Return value not surfaced - EAGAIN (pipe full) just
  // means a wakeup is already pending.
  void Wake() const;

  // Reads and discards every queued byte so an already-handled wakeup
  // doesn't make a later poll() return on stale data.
  void Drain() const;

  int ReadFd() const { return m_readFd; }
  bool IsOpen() const { return m_readFd >= 0; }

  void Close();

 private:
  int m_readFd = -1;
  int m_writeFd = -1;
};

}  // namespace binlog_streamer
