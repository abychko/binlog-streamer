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
#include <string>
#include <string_view>
#include "config/sServerSettings.hpp"
#include "config/sSourceSettings.hpp"
#include "net/cWakeupPipe.hpp"
#include "receiver/iBinlogProbe.hpp"

namespace binlog_streamer {

class DumpProbe : public BinlogProbe {
 public:
  DumpProbe(const SourceSettings &source, const ServerSettings &server,
            std::string replicaUuid, std::string relayName,
            std::string relayVersion,
            const std::atomic<bool> *stopRequested = nullptr,
            const WakeupPipe *wakeupPipe = nullptr);

  ProbeResult Probe(const GtidSet &startSet) override;
  PreviousGtidsResult PreviousGtidsText(std::string_view fileName) override;

 private:
  const SourceSettings m_source;
  const ServerSettings m_server;
  std::string m_replicaUuid;
  std::string m_relayName;
  std::string m_relayVersion;
  const std::atomic<bool> *m_stopRequested;
  const WakeupPipe *m_wakeupPipe;
};

}  // namespace binlog_streamer
