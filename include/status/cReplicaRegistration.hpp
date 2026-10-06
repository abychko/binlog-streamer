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
#include <chrono>
#include <mutex>
#include <string>
#include "status/cStreamProgress.hpp"
#include "status/sReplicaFacts.hpp"

namespace binlog_streamer {

class ReplicaRegistration {
 public:
  explicit ReplicaRegistration(ReplicaFacts facts);

  const ReplicaFacts &Facts() const { return m_facts; }
  std::chrono::system_clock::time_point Since() const { return m_since; }
  StreamProgress &Progress() { return m_progress; }
  const StreamProgress &Progress() const { return m_progress; }
  void SetDumping(bool dumping) { m_dumping.store(dumping); }
  bool Dumping() const { return m_dumping.load(); }
  void SetReportHost(std::string host);
  std::string ReportHost() const;

 private:
  ReplicaFacts m_facts;
  std::chrono::system_clock::time_point m_since;
  StreamProgress m_progress;
  std::atomic<bool> m_dumping{false};
  mutable std::mutex m_reportHostMutex;
  std::string m_reportHost;
};

}  // namespace binlog_streamer
