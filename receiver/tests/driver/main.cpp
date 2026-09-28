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

// Test-only tool for StreamIntegrationTest's byte-exact comparison against
// the source's own binlog; not part of the product build.

#include "cFileEventSink.hpp"
#include "cStreamDriver.hpp"

#include "cSourceConfigLoader.hpp"  // config/'s own internal header - see this target's CMakeLists.txt
#include "config/cConfigErrorPrinter.hpp"
#include "config/cDiskProtectedFileReader.hpp"

#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> g_stopRequested{false};

// No-op: only needed to make an in-progress blocking read return EINTR
// (self-inflicted via alarm(), same pattern src/main.cpp uses for signals).
void HandleAlarm(int) { g_stopRequested.store(true); }

std::optional<std::string_view> FlagValue(std::string_view arg,
                                          std::string_view flag) {
  if (arg.size() <= flag.size() + 1 || arg.substr(0, flag.size()) != flag ||
      arg[flag.size()] != '=')
    return std::nullopt;
  return arg.substr(flag.size() + 1);
}

// Files this tool reads belong to whoever runs it - no packaged,
// root-owned deployment to check ownership against, unlike the built relay.
std::string EffectiveUserName() {
  const auto *entry = getpwuid(geteuid());
  return entry != nullptr ? entry->pw_name : std::string();
}

std::string EffectiveGroupName() {
  const auto *entry = getgrgid(getegid());
  return entry != nullptr ? entry->gr_name : std::string();
}

}  // namespace

int main(int argc, char *argv[]) {
  using namespace binlog_streamer;
  using namespace binlog_streamer::test;

  std::string sourceYmlPath;
  std::string sinkFilePath;
  DriverOptions options;
  std::uint64_t maxSeconds = 0;
  bool haveServerId = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (auto v = FlagValue(arg, "--source-yml")) {
      sourceYmlPath = *v;
    } else if (auto v = FlagValue(arg, "--server-id")) {
      options.server.serverId =
          static_cast<std::uint32_t>(std::stoul(std::string(*v)));
      haveServerId = true;
    } else if (auto v = FlagValue(arg, "--start-gtid-set")) {
      options.startGtidSetText = *v;
    } else if (auto v = FlagValue(arg, "--stop-after-gtid-events")) {
      options.stopAfterGtidEvents = std::stoull(std::string(*v));
    } else if (auto v = FlagValue(arg, "--wait-heartbeats")) {
      options.waitHeartbeats = std::stoull(std::string(*v));
    } else if (auto v = FlagValue(arg, "--heartbeat-period")) {
      options.heartbeatPeriod =
          std::chrono::seconds(std::stoll(std::string(*v)));
    } else if (auto v = FlagValue(arg, "--sink-file")) {
      sinkFilePath = *v;
    } else if (auto v = FlagValue(arg, "--max-seconds")) {
      maxSeconds = std::stoull(std::string(*v));
    } else {
      std::cerr << "unknown argument: " << arg << '\n';
      return 2;
    }
  }

  if (sourceYmlPath.empty() || sinkFilePath.empty() || !haveServerId) {
    std::cerr << "usage: bs-stream-driver --source-yml=PATH --server-id=N "
                 "--sink-file=PATH\n"
                 "    [--start-gtid-set=TEXT] [--stop-after-gtid-events=N] "
                 "[--wait-heartbeats=K]\n"
                 "    [--heartbeat-period=SECONDS] [--max-seconds=SECONDS]\n";
    return 2;
  }

  DiskProtectedFileReader reader(EffectiveUserName(), EffectiveGroupName());
  const auto sourceResult =
      SourceConfigLoader(reader, EffectiveUserName()).Load(sourceYmlPath);
  if (!sourceResult.value) {
    ConfigErrorPrinter::Print(std::cerr, sourceResult.errors);
    return 2;
  }
  options.source = *sourceResult.value;

  std::ofstream sinkStream(sinkFilePath, std::ios::binary | std::ios::trunc);
  if (!sinkStream) {
    std::cerr << "cannot open sink file: " << sinkFilePath << '\n';
    return 2;
  }

  if (maxSeconds > 0) {
    struct sigaction action{};
    action.sa_handler = HandleAlarm;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // deliberately no SA_RESTART - see cTcpTransport.cpp
    sigaction(SIGALRM, &action, nullptr);
    alarm(static_cast<unsigned>(maxSeconds));
  }

  StreamDriver driver(options, &g_stopRequested);
  const DriverResult result = driver.Run(sinkStream);
  sinkStream.close();

  std::cout << "file="
            << (result.hasWrittenAnyEvent ? result.firstWrittenFileName
                                          : std::string("-"))
            << " first_offset=" << result.firstWrittenOffset << " last_file="
            << (result.hasWrittenAnyEvent ? result.lastWrittenFileName
                                          : std::string("-"))
            << " last_offset=" << result.lastWrittenEndOffset
            << " gtid_events=" << result.gtidEventCount
            << " heartbeats=" << result.heartbeatCount
            << " events=" << result.events
            << " largest_event_length=" << result.largestEventLength
            << " largest_event_sub_packets=" << result.largestEventSubPackets
            << " reached_targets=" << (result.ok ? "yes" : "no")
            << " message=" << result.message << '\n';

  return result.ok ? 0 : 1;
}
