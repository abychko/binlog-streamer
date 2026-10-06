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

#include "cReloadSignalThread.hpp"

#include <pthread.h>

#include <cerrno>
#include <csignal>
#include <utility>

namespace binlog_streamer {
namespace {

sigset_t ReloadSignalSet() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGHUP);
  return set;
}

}  // namespace

// A SIG_IGN inherited from the parent (nohup) makes macOS discard SIGHUP even
// while blocked, and an inherited blocked SIGTERM would hide a stop request:
// hence SIG_DFL and SETMASK.
void ReloadSignalThread::BlockReloadSignal() {
  signal(SIGHUP, SIG_DFL);
  const sigset_t set = ReloadSignalSet();
  pthread_sigmask(SIG_SETMASK, &set, nullptr);
}

ReloadSignalThread::ReloadSignalThread(Handler handler)
    : m_handler(std::move(handler)) {}

ReloadSignalThread::~ReloadSignalThread() { Stop(); }

void ReloadSignalThread::Start() {
  if (m_thread.joinable()) return;
  m_stopping.store(false);
  m_thread = std::thread([this] { Run(); });
}

void ReloadSignalThread::Stop() {
  if (!m_thread.joinable()) return;
  m_stopping.store(true);
  pthread_kill(m_thread.native_handle(), SIGHUP);
  m_thread.join();
}

void ReloadSignalThread::Run() {
  const sigset_t set = ReloadSignalSet();
  for (;;) {
    int signal = 0;
    int rc;
    while ((rc = sigwait(&set, &signal)) == EINTR) {
    }
    if (rc != 0 || m_stopping.load()) return;
    m_handler();
  }
}

}  // namespace binlog_streamer
