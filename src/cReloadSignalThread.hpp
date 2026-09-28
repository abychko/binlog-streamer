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
#include <functional>
#include <thread>

namespace binlog_streamer {

// SIGHUP is taken by sigwait() in one thread, as mysqld does, instead of a
// handler: the reload runs as ordinary code, not in signal context.
class ReloadSignalThread {
 public:
  using Handler = std::function<void()>;

  // Call before any other thread starts, so every thread inherits the mask
  // and a SIGHUP arriving before Start() waits instead of ending the process.
  // Also undoes signal state inherited from the parent: SIG_IGN for SIGHUP
  // and any other blocked signal.
  static void BlockReloadSignal();

  explicit ReloadSignalThread(Handler handler);
  ~ReloadSignalThread();
  ReloadSignalThread(const ReloadSignalThread &) = delete;
  ReloadSignalThread &operator=(const ReloadSignalThread &) = delete;

  void Start();
  void Stop();

 private:
  void Run();

  Handler m_handler;
  std::atomic<bool> m_stopping{false};
  std::thread m_thread;
};

}  // namespace binlog_streamer
