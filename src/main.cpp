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

#include "binlog/hEventLimits.hpp"
#include "cCacheSizeWarning.hpp"
#include "cCommandLine.hpp"
#include "cHostMemory.hpp"
#include "cReloadSignalThread.hpp"
#include "cStorageServerState.hpp"
#include "cStorageStatusFacts.hpp"
#include "cSystemdNotify.hpp"
#include "config/cConfigErrorPrinter.hpp"
#include "config/cConfigurationLoader.hpp"
#include "config/cConfigurationReload.hpp"
#include "config/cDiskProtectedFileReader.hpp"
#include "config/hConfigDefaults.hpp"
#include "eExitCode.hpp"
#include "http/cHtmlDirectory.hpp"
#include "http/cHttpListener.hpp"
#include "net/cPacketChannel.hpp"
#include "net/cTcpTransport.hpp"
#include "net/cTlsContext.hpp"
#include "net/cWakeupPipe.hpp"
#include "protocol/cComQuitCommand.hpp"
#include "protocol/eCompressionAlgorithm.hpp"
#include "receiver/cDumpProbe.hpp"
#include "receiver/cEventCounterSink.hpp"
#include "receiver/cEventStreamReader.hpp"
#include "receiver/cSessionUuid.hpp"
#include "receiver/cSourceClock.hpp"
#include "receiver/cStartupSequence.hpp"
#include "receiver/hSessionDefaults.hpp"
#include "server/cReplicaListener.hpp"
#include "server/cServerCertificateFiles.hpp"
#include "server/cServerUuidFile.hpp"
#include "server/hServerVersion.hpp"
#include "status/cRelayStatusTracker.hpp"
#include "status/cStatusJsonWriter.hpp"
#include "storage/cBinlogStorage.hpp"
#include "storage/cStorageEventSink.hpp"
#include "storage/cStorageReader.hpp"
#include "storage/cStorageRecovery.hpp"
#include "storage/cStorageWriter.hpp"
#include "storage/hStorageDefaults.hpp"
#include "storage/sStorageExpiry.hpp"

#include <poll.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#ifdef BINLOG_STREAMER_DEVELOPER_MODE
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace binlog_streamer {
namespace {

// Must be lock-free: it is written from a signal handler.
std::atomic<bool> g_stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

WakeupPipe g_wakeupPipe;

// Both calls are async-signal-safe, as a signal handler requires.
void HandleStopSignal(int) {
  g_stopRequested.store(true);
  g_wakeupPipe.Wake();
}

void InstallStopSignalHandlers() {
  struct sigaction action{};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags =
      0;  // deliberately no SA_RESTART, so a blocking read gets EINTR
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);
}

#ifdef BINLOG_STREAMER_DEVELOPER_MODE
std::string EffectiveUserName() {
  const auto *entry = getpwuid(geteuid());
  return entry != nullptr ? entry->pw_name : std::string();
}

std::string EffectiveGroupName() {
  const auto *entry = getgrgid(getegid());
  return entry != nullptr ? entry->gr_name : std::string();
}
#endif

// "registered" must appear verbatim: src/tests/source_integration_test.sh
// matches on it.
void PrintIdentity(const SourceIdentity &identity, const std::string &tls) {
  std::cerr << BINLOG_STREAMER_NAME ": registered with source (version "
            << identity.versionString << ", server_uuid " << identity.serverUuid
            << ", server_id " << identity.serverId << ", checksum "
            << identity.checksumAlgorithm << ", gtid_mode " << identity.gtidMode
            << ")" << (tls.empty() ? "" : " over " + tls) << '\n';
}

// A server_id mismatch only warns: operators may change it deliberately, and a
// real source swap is caught by history checks or error 1236.
std::optional<std::string> DescribeStorageIdentityChange(
    const StorageCatalog &catalog, const SourceIdentity &identity) {
  if (IsRelayServerVersion(identity.versionString, BINLOG_STREAMER_NAME))
    return std::nullopt;
  const std::optional<StoredFileRecord> last = catalog.Last();
  if (!last) return std::nullopt;
  if (last->serverId == identity.serverId) return std::nullopt;
  return "source server_id has changed from " + std::to_string(last->serverId) +
         " (recorded in " + last->name + ") to " +
         std::to_string(identity.serverId) +
         " - this should not happen unless the source's server_id was changed "
         "deliberately";
}

// milliseconds::rep can exceed poll()'s int timeout range: clamp.
int ClampToPollTimeout(std::chrono::milliseconds timeout) {
  const auto count = timeout.count();
  if (count < 0) return 0;
  if (count > std::numeric_limits<int>::max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(count);
}

// Wake() returns poll() at once; napping and re-checking could miss a signal
// handled between two checks.
void InterruptibleSleep(std::chrono::seconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (!g_stopRequested.load()) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) break;
    struct pollfd pfd{g_wakeupPipe.ReadFd(), POLLIN, 0};
    const int pollTimeout = ClampToPollTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining));
    const int pollResult = poll(&pfd, 1, pollTimeout);
    if (pollResult > 0 && (pfd.revents & POLLIN) != 0) {
      // Not drained: g_wakeupPipe is shared with every connection's
      // TcpTransport, and draining could delay another thread noticing the
      // stop.
      break;
    }
  }
}

// ClassifyStreamEnd() maps StoppedBySink to Success; main() overrides it to
// StorageError when that is why the stream stopped.
struct StreamOutcome {
  ExitCode exitCode = ExitCode::SourceError;
  std::string line;
};

StreamOutcome ClassifyStreamEnd(const StreamResult &result) {
  switch (result.reason) {
    case StreamEndReason::Stopped:
    case StreamEndReason::StoppedBySink:
      return {ExitCode::Success, "stopped: " + result.message};
    case StreamEndReason::SourceError:
      return {ExitCode::SourceError, "source error " +
                                         std::to_string(result.errorCode) +
                                         ": " + result.errorText};
    case StreamEndReason::HeartbeatFailure:
      return {ExitCode::SourceError, "heartbeat failure: " + result.message};
    case StreamEndReason::MalformedStream:
      return {ExitCode::SourceError, "malformed stream: " + result.message};
    case StreamEndReason::Timeout:
    case StreamEndReason::ConnectionClosed:
    case StreamEndReason::EndOfStream:
      return {ExitCode::SourceStreamLost, "stream lost: " + result.message};
  }
  return {ExitCode::SourceError, "unrecognized stream end reason"};
}

// The history the relay asks to continue from is gone from the source; asking
// again is refused the same way.
constexpr std::uint16_t ER_SOURCE_FATAL_ERROR_READING_BINLOG = 1236;

// Stream ends another stream can take over from, with storage and replica
// sessions untouched. The rest ends the run: storage failure, unparsable
// stream, heartbeat behind the position already read, and error 1236.
bool CanReconnect(const StreamResult &result) {
  switch (result.reason) {
    case StreamEndReason::Timeout:
    case StreamEndReason::ConnectionClosed:
    case StreamEndReason::EndOfStream:
      return true;
    case StreamEndReason::SourceError:
      return result.errorCode != ER_SOURCE_FATAL_ERROR_READING_BINLOG;
    case StreamEndReason::HeartbeatFailure:
    case StreamEndReason::MalformedStream:
    case StreamEndReason::Stopped:
    case StreamEndReason::StoppedBySink:
      return false;
  }
  return false;
}

// Built once per run from the first registration: the source's clock is the one
// stored files are aged by.
StorageExpiry MakeExpiry(const StorageSettings &settings,
                         const SourceIdentity &identity) {
  const SourceClock sourceClock = SourceClock::FromIdentity(identity);
  StorageExpiry expiry;
  if (sourceClock.Known()) {
    expiry.sourceNow = [sourceClock] { return sourceClock.Now(); };
    expiry.period = settings.retention.period;
  } else {
    std::cerr << BINLOG_STREAMER_NAME
        ": warning: the source did not report its clock (SELECT "
        "UNIX_TIMESTAMP()); files are not removed by storage.retention.period "
        "in this run\n";
  }

  expiry.onPurged = [](const PurgeResult &result) {
    std::string output;
    for (const auto &name : result.purged)
      output += BINLOG_STREAMER_NAME ": purged " + name + '\n';
    std::size_t begin = 0;
    while (begin < result.warning.size()) {
      const auto end = result.warning.find('\n', begin);
      output += BINLOG_STREAMER_NAME ": warning: could not remove " +
                result.warning.substr(begin, end - begin) + '\n';
      if (end == std::string::npos) break;
      begin = end + 1;
    }
    std::cerr << output;
  };
  const auto &disk = settings.disk;
  expiry.disk = StorageDiskLimits{
      disk.maxSize,
      disk.recoveryReserve,
      disk.purgeHighWatermark,
      disk.purgeLowWatermark,
      disk.minFreeSpace,
      [high = disk.purgeHighWatermark,
       minFree = disk.minFreeSpace](const PurgeResult &result) {
        std::string output;
        if (result.spaceShort) {
          output = BINLOG_STREAMER_NAME
                   ": warning: storage.disk limits are still exceeded after "
                   "purging: " +
                   std::to_string(result.usedBytes) +
                   " byte(s) stored (purge_high_watermark " +
                   std::to_string(high) + "), " +
                   std::to_string(result.availableBytes) +
                   " byte(s) free (min_free_space " + std::to_string(minFree) +
                   ")\n";
        } else {
          output = BINLOG_STREAMER_NAME
                   ": storage.disk limits are met again: " +
                   std::to_string(result.usedBytes) + " byte(s) stored, " +
                   std::to_string(result.availableBytes) + " byte(s) free\n";
        }
        std::cerr << output;
      }};
  return expiry;
}

void PrintStreamSummary(const StreamResult &result,
                        const std::string &reasonLine) {
  std::cerr << BINLOG_STREAMER_NAME ": " << reasonLine << '\n'
            << BINLOG_STREAMER_NAME ": first event "
            << result.firstPosition.fileName << ':'
            << result.firstPosition.position << '\n'
            << BINLOG_STREAMER_NAME ": last event "
            << result.lastPosition.fileName << ':'
            << result.lastPosition.position << '\n'
            << BINLOG_STREAMER_NAME ": events=" << result.events
            << " heartbeats=" << result.heartbeats
            << " artificial=" << result.artificial << " bytes=" << result.bytes
            << " largest_event_length=" << result.largestEventLength
            << " largest_event_sub_packets=" << result.largestEventSubPackets
            << '\n';
}

}  // namespace
}  // namespace binlog_streamer

int main(int argc, char *argv[]) {
  using namespace binlog_streamer;
  ReloadSignalThread::BlockReloadSignal();
  const auto command = CommandLine::Parse(argc, argv);
  if (!command.value) {
    ConfigErrorPrinter::Print(std::cerr, command.errors);
    return static_cast<int>(ExitCode::ConfigurationError);
  }
  if (command.value->showHelp) {
    std::cout << "Usage: " BINLOG_STREAMER_NAME
                 " [options]\n"
                 "\n"
                 "  --config PATH       settings file (default: "
              << DEFAULT_SETTINGS_PATH
              << ")\n"
                 "  --validate-config   check the settings and exit\n"
                 "  --help              show this help and exit\n"
                 "  --version           show the version and exit\n";
    return static_cast<int>(ExitCode::Success);
  }
  if (command.value->showVersion) {
    std::cout << BINLOG_STREAMER_NAME " " << BINLOG_STREAMER_VERSION;
#ifdef BINLOG_STREAMER_DEVELOPER_MODE
    std::cout << " (developer mode)";
#endif
    std::cout << '\n';
    return static_cast<int>(ExitCode::Success);
  }

#ifdef BINLOG_STREAMER_DEVELOPER_MODE
  std::cerr << BINLOG_STREAMER_NAME
      ": developer mode build - settings file owner/group check relaxed to the "
      "running user/group (see cmake/developer_mode.cmake)\n";
  const std::string expectedOwner = EffectiveUserName();
  const std::string expectedGroup = EffectiveGroupName();
#else
  const std::string expectedOwner = PROTECTED_FILE_OWNER;
  const std::string expectedGroup = PROTECTED_FILE_GROUP;
#endif
  DiskProtectedFileReader reader(expectedOwner, expectedGroup);
  const auto configuration = ConfigurationLoader(reader, expectedOwner)
                                 .Load(command.value->settingsPath);
  if (!configuration.value) {
    ConfigErrorPrinter::Print(std::cerr, configuration.errors);
    return static_cast<int>(ExitCode::ConfigurationError);
  }
  // Reservation is lazy, so warn instead of refusing to start. Compares host
  // RAM, ignoring cgroup limits.
  if (const auto text = CacheSizeWarning::Describe(
          configuration.value->settings.cache.maxSize,
          HostMemory::PhysicalBytes())) {
    std::cerr << BINLOG_STREAMER_NAME ": warning: " << *text << '\n';
  }
  if (command.value->validateOnly) return static_cast<int>(ExitCode::Success);

  // A missing or permission-denied data_dir is a configuration problem (exit
  // code 1), not a storage one (4).
  const auto &dataDir = configuration.value->settings.storage.dataDir;
  BinlogStorage storage;
  StorageOpenFailure storageOpenFailure = StorageOpenFailure::StorageProblem;
  std::string storageError;
  StorageStartState storageStartState;
  if (!storage.OpenResumed(dataDir, storageStartState, storageOpenFailure,
                           storageError)) {
    std::cerr << BINLOG_STREAMER_NAME ": storage: " << storageError << '\n';
    return static_cast<int>(storageOpenFailure ==
                                    StorageOpenFailure::AccessProblem
                                ? ExitCode::ConfigurationError
                                : ExitCode::StorageError);
  }
  if (!storage.ReserveCache(configuration.value->settings.cache.maxSize,
                            configuration.value->settings.cache.window,
                            g_stopRequested, storageOpenFailure,
                            storageError)) {
    std::cerr << BINLOG_STREAMER_NAME ": storage: " << storageError << '\n';
    return static_cast<int>(ExitCode::StorageError);
  }
  if (storageStartState.truncated) {
    std::cerr << BINLOG_STREAMER_NAME ": truncated "
              << storageStartState.lastFileName << " by "
              << storageStartState.truncatedBytes
              << " byte(s) past its last complete transaction (an earlier run "
                 "stopped mid-write)\n";
  }
  // pipe() failing is resource exhaustion, not a bad setting: classified
  // SourceError, the closest code.
  std::string wakeupPipeError;
  if (!g_wakeupPipe.Open(wakeupPipeError)) {
    std::cerr << BINLOG_STREAMER_NAME ": opening the stop-signal wakeup pipe: "
              << wakeupPipeError << '\n';
    return static_cast<int>(ExitCode::SourceError);
  }
  InstallStopSignalHandlers();

  static_assert(
      std::string_view(ServerUuidFile::FILE_NAME) == SERVER_UUID_FILE_NAME,
      "the storage has to recognise the file the relay keeps its server UUID "
      "in");
  std::string serverUuid;
  std::string serverUuidError;
  if (!ServerUuidFile::LoadOrCreate(dataDir, SessionUuid::Generate(),
                                    serverUuid, serverUuidError)) {
    std::cerr << BINLOG_STREAMER_NAME ": the relay's own server UUID: "
              << serverUuidError << '\n';
    return static_cast<int>(ExitCode::StorageError);
  }
  // Offered to every replica, required of none unless replica.yml says so;
  // without a configured certificate the relay generates one next to auto.cnf.
  static_assert(std::count(TLS_FILE_NAMES.begin(), TLS_FILE_NAMES.end(),
                           ServerCertificateFiles::CA_FILE_NAME) == 1 &&
                    std::count(TLS_FILE_NAMES.begin(), TLS_FILE_NAMES.end(),
                               ServerCertificateFiles::CA_KEY_FILE_NAME) == 1 &&
                    std::count(TLS_FILE_NAMES.begin(), TLS_FILE_NAMES.end(),
                               ServerCertificateFiles::CERT_FILE_NAME) == 1 &&
                    std::count(TLS_FILE_NAMES.begin(), TLS_FILE_NAMES.end(),
                               ServerCertificateFiles::KEY_FILE_NAME) == 1,
                "the storage has to recognise the files the relay keeps its "
                "certificate in");
  TlsContext serverTls;
  {
    const ReplicaSettings &replica = configuration.value->replica;
    TlsMaterial material = replica.tls;
    std::string tlsError;
    if (replica.sslCert.empty() &&
        !ServerCertificateFiles::LoadOrCreate(dataDir, BINLOG_STREAMER_NAME,
                                              material, tlsError)) {
      std::cerr << BINLOG_STREAMER_NAME ": the relay's own certificate: "
                << tlsError << '\n';
      return static_cast<int>(ExitCode::StorageError);
    }
    if (!serverTls.LoadServer(material, tlsError)) {
      std::cerr << BINLOG_STREAMER_NAME ": the relay's own certificate: "
                << tlsError << '\n';
      return static_cast<int>(ExitCode::ConfigurationError);
    }
  }
  const ServerIdentity serverIdentity{
      configuration.value->settings.server.serverId, serverUuid,
      BINLOG_STREAMER_NAME " " BINLOG_STREAMER_VERSION, BINLOG_STREAMER_NAME,
      BINLOG_STREAMER_VERSION};
  const StorageServerState serverState(storage.Catalog(), dataDir);
  StorageReader storageReader(dataDir, storage.Catalog(), storage.Published(),
                              storage.Cache());
  const StorageStatusFacts storageFacts(
      storage.Catalog(), storage.Published(), storage.Cache(),
      configuration.value->settings.storage.disk.maxSize);
  RelayStatusTracker statusTracker(
      BINLOG_STREAMER_NAME, BINLOG_STREAMER_VERSION,
      configuration.value->settings.server.maxConnections, &storageFacts);
  statusTracker.SetSourceAddress(
      configuration.value->source.host + ":" +
      std::to_string(configuration.value->source.port));

  // Started before the connect-retry loop so replicas can connect and wait
  // while the source is still unreachable.
  ReplicaListener replicaListener(
      configuration.value->replica,
      configuration.value->settings.server.maxConnections, &g_stopRequested,
      &g_wakeupPipe,
      [](const std::string &line) {
        std::cerr << BINLOG_STREAMER_NAME ": replica: " << line << '\n';
      },
      serverIdentity, &serverState, &storageReader, &serverTls, &statusTracker,
      configuration.value->settings.server.sendLinger);
  std::string replicaListenerError;
  if (!replicaListener.Start(replicaListenerError)) {
    std::cerr << BINLOG_STREAMER_NAME ": starting the replica listener: "
              << replicaListenerError << '\n';
    // ConfigurationError: a bind failure is usually a bad
    // listen_address/listen_port.
    return static_cast<int>(ExitCode::ConfigurationError);
  }
  Configuration running = *configuration.value;
  ReloadSignalThread reloadSignalThread([&] {
    std::cerr << BINLOG_STREAMER_NAME ": received SIGHUP, reloading\n";
    const auto loaded = ConfigurationLoader(reader, expectedOwner)
                            .Load(command.value->settingsPath);
    if (!loaded.value) {
      ConfigErrorPrinter::Print(std::cerr, loaded.errors);
      std::cerr << BINLOG_STREAMER_NAME
          ": reload failed, the running configuration is kept\n";
      return;
    }
    ReloadOutcome outcome =
        ConfigurationReload::Compare(running, *loaded.value);
    for (const std::string &ignored : outcome.ignored)
      std::cerr << BINLOG_STREAMER_NAME ": reload: " << ignored
                << " changed, takes effect after a restart\n";
    std::cerr << BINLOG_STREAMER_NAME ": reload: hosts updated for "
              << outcome.changedHosts << " of " << outcome.clients.size()
              << " users\n";
    running.replica.clients = outcome.clients;
    replicaListener.Clients().Replace(std::move(outcome.clients));
  });
  reloadSignalThread.Start();

  // /status.json is the live status; any other path is a file of html_dir.
  const HtmlDirectory htmlDirectory(
      configuration.value->settings.monitoring.http.htmlDir);
  HttpListener httpListener(
      configuration.value->settings.monitoring.http, &g_stopRequested,
      &g_wakeupPipe,
      [&statusTracker, &htmlDirectory](const HttpRequest &request) {
        if (request.path == "/status.json") {
          HttpResponse response;
          response.contentType = "application/json";
          response.body = StatusJsonWriter::Write(statusTracker.Snapshot());
          return response;
        }
        if (auto served = htmlDirectory.Serve(request.path)) return *served;
        HttpResponse response;
        response.status = 404;
        response.body = "no such path\n";
        return response;
      },
      [](const std::string &line) {
        std::cerr << BINLOG_STREAMER_NAME ": http: " << line << '\n';
      });
  std::string httpListenerError;
  if (!httpListener.Start(httpListenerError)) {
    std::cerr << BINLOG_STREAMER_NAME ": starting the http listener: "
              << httpListenerError << '\n';
    return static_cast<int>(ExitCode::ConfigurationError);
  }
  // Ready once both listeners are up, not once the source answers: that may
  // take the whole retry budget.
  if (const auto notifyError = SystemdNotify::Send("READY=1"))
    std::cerr << BINLOG_STREAMER_NAME
              << ": warning: telling systemd the relay is ready: "
              << *notifyError << '\n';

  TcpTransport transport(&g_stopRequested, &g_wakeupPipe);
  const std::string replicaUuid = SessionUuid::Generate();
  // Reuses the run's replicaUuid (probes never register). Same stop flag and
  // wakeup pipe as the main transport, so a stop is not missed mid-probe.
  DumpProbe probe(configuration.value->source,
                  configuration.value->settings.server, replicaUuid,
                  BINLOG_STREAMER_NAME, BINLOG_STREAMER_VERSION,
                  &g_stopRequested, &g_wakeupPipe);
  std::string attemptLabel = "start-up";
  RetryOptions options;
  options.sleep = InterruptibleSleep;
  options.onRetry = [&attemptLabel, &statusTracker](unsigned attempt,
                                                    unsigned maxAttempts,
                                                    const std::string &reason) {
    statusTracker.SourceAttempt(attempt);
    std::cerr << BINLOG_STREAMER_NAME ": " << attemptLabel << " attempt "
              << attempt << "/" << maxAttempts << " failed: " << reason << '\n';
  };

  // storageSink wraps counterSink, not the reverse: counting must not gate what
  // reaches disk, while storage's refusal must stop the stream.
  EventCounterSink counterSink;
  std::optional<StorageWriter> writer;
  std::optional<StorageEventSink> storageSink;
  std::size_t checksumLength = 0;

  std::optional<GtidSet> resumeSet =
      storageStartState.empty
          ? std::nullopt
          : std::optional<GtidSet>(storageStartState.startSet);
  std::string resumeFileName = storageStartState.lastFileName;
  StreamOutcome outcome{ExitCode::Success, std::string()};
  bool dumpStarted = false;

  for (;;) {
    StartupSequence startup(transport, configuration.value->source,
                            configuration.value->settings.server, replicaUuid,
                            BINLOG_STREAMER_NAME, BINLOG_STREAMER_VERSION,
                            probe, options, ReplicaSessionOptions{},
                            &g_stopRequested, resumeSet, resumeFileName);
    const StartupOutcome startupOutcome = startup.Run();
    const SessionResult &result = startupOutcome.result;

    if (result.outcome != SessionOutcome::Registered) {
      std::cerr << BINLOG_STREAMER_NAME ": " << result.message << '\n';
      if (result.outcome == SessionOutcome::Stopped) {
        outcome = {ExitCode::Success, std::string()};
      } else {
        // After a dump has run, an unreachable source is a lost stream: exit
        // code 3 makes systemd restart (code 2 would stop it), and the new run
        // resumes from the stored position.
        outcome = {
            dumpStarted ? ExitCode::SourceStreamLost : ExitCode::SourceError,
            std::string()};
      }
      break;
    }
    PrintIdentity(result.identity, startupOutcome.session->tlsDescription());
    statusTracker.SourceConnected(
        result.identity.serverId, result.identity.serverUuid,
        result.identity.versionString, startupOutcome.session->encrypted(),
        startupOutcome.session->compressed()
            ? std::string(CompressionAlgorithmName(
                  configuration.value->source.compression))
            : "none",
        result.identity.unixTimestamp);

    if (const auto identityWarning =
            DescribeStorageIdentityChange(storage.Catalog(), result.identity)) {
      // "warning: ", not "storage: ": that prefix means the run stopped (exit
      // code 4).
      std::cerr << BINLOG_STREAMER_NAME ": warning: " << *identityWarning
                << '\n';
    }

    if (!writer) {
      checksumLength =
          result.identity.checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
      writer.emplace(
          dataDir, *storage.Cache(), storage.Catalog(), StorageWriterHooks{},
          [] {
            std::cerr << BINLOG_STREAMER_NAME
                ": cache full: receiving paused until buffered events reach "
                "disk (reported once per run)\n";
          },
          MakeExpiry(configuration.value->settings.storage, result.identity));
      storageSink.emplace(
          dataDir, checksumLength, counterSink, storage.Catalog(),
          *storage.Cache(), *writer, &storage.Published(),
          [](const std::string &name, std::uint64_t size) {
            std::cerr << BINLOG_STREAMER_NAME ": closed " << name << " ("
                      << size
                      << " byte(s)), left \"in use\" by an earlier run the "
                         "source has since moved past\n";
          });
      writer->PostPurge();
      writer->Start();
    }

    ReplicaSession &session = *startupOutcome.session;
    const StartSetResolution &resolution = startupOutcome.resolution;

    if (resolution.usedStoredHistory) {
      std::cerr << BINLOG_STREAMER_NAME
          ": dump requested from stored history (file "
                << resolution.selectedFileName << ", set "
                << resolution.startSet.ToText() << ")\n";
    } else {
      // "(file X)" on its own: storage/tests/*.sh parse it.
      std::cerr << BINLOG_STREAMER_NAME
          ": dump requested from current position (file "
                << resolution.selectedFileName << "), set "
                << resolution.startSet.ToText() << '\n';
    }

    if (const auto dumpFailure = session.StartDump(resolution.startSet)) {
      std::cerr << BINLOG_STREAMER_NAME ": " << dumpFailure->message << '\n';
      outcome = {
          dumpStarted ? ExitCode::SourceStreamLost : ExitCode::SourceError,
          std::string()};
      break;
    }
    dumpStarted = true;

    StreamReaderOptions readerOptions;
    readerOptions.sequenceId = session.NextSequenceId();
    readerOptions.checksumLength = checksumLength;
    readerOptions.verifySequence = !session.compressed();
    readerOptions.progress = &statusTracker.Source();
    // session.transport(), not transport: a compressed connection carries the
    // dump in the same frames, on the same frame counter.
    EventStreamReader eventReader(
        session.transport(), *storageSink,
        StreamPosition{resolution.selectedFileName, 0}, readerOptions);
    const StreamResult streamResult = eventReader.Run();
    outcome = ClassifyStreamEnd(streamResult);
    // storageSink is the only sink here that can fail, so HasFailed() tells a
    // storage refusal from any other stop.
    bool storageFailed = false;
    if (streamResult.reason == StreamEndReason::StoppedBySink &&
        storageSink->HasFailed()) {
      outcome.exitCode = ExitCode::StorageError;
      outcome.line = "storage: " + storageSink->LastError().message;
      storageFailed = true;
    }
    if (writer->Failed()) {
      outcome.exitCode = ExitCode::StorageError;
      outcome.line = "storage: " + writer->LastError();
      storageFailed = true;
    }
    PrintStreamSummary(streamResult, outcome.line);
    if (storageFailed || !CanReconnect(streamResult)) break;

    // The stream is gone, what it filled is not: replicas reading storage are
    // not told anything ended.
    transport.Close();
    // Every byte must be on disk before the position is read back; the reader
    // above, the only producer, has stopped.
    if (!writer->DrainAndSync()) {
      std::cerr << BINLOG_STREAMER_NAME ": storage: " << writer->LastError()
                << '\n';
      outcome = {ExitCode::StorageError, std::string()};
      break;
    }
    StorageStartState resumed;
    std::string resumeError;
    if (!StorageRecovery::ResumePoint(dataDir, storage.Catalog(), resumed,
                                      resumeError)) {
      std::cerr << BINLOG_STREAMER_NAME ": storage: " << resumeError << '\n';
      outcome = {ExitCode::StorageError, std::string()};
      break;
    }
    resumeSet =
        resumed.empty ? std::nullopt : std::optional<GtidSet>(resumed.startSet);
    resumeFileName = resumed.lastFileName;
    storageSink->RestartStream();
    statusTracker.SourceLost();
    attemptLabel = "reconnect";
    std::cerr << BINLOG_STREAMER_NAME ": reconnecting to the source";
    if (!resumed.empty)
      std::cerr << " to go on with " << resumed.lastFileName << " at "
                << resumed.lastFileLength;
    std::cerr << '\n';
  }

  // Malformed input can follow accepted bytes of an unfinished group: those
  // bytes are kept, and recovery truncates the incomplete group on restart.
  if (writer && !writer->Failed() && !writer->DrainAndSync())
    std::cerr << BINLOG_STREAMER_NAME ": failed to flush storage on shutdown: "
              << writer->LastError() << '\n';

  const auto cacheCounters = storage.Cache()->Counters();
  std::cerr << "cache: appended=" << cacheCounters.appended
            << " evicted=" << cacheCounters.evictedForSpace
            << " evicted_window=" << cacheCounters.evictedForWindow
            << " max_unwritten=" << cacheCounters.maxUnwritten
            << " space_waits=" << cacheCounters.spaceWaits << " space_wait_ms="
            << cacheCounters.spaceWaitNanoseconds / 1'000'000
            << " max_pinned=" << cacheCounters.maxPinned << '\n';
  if (writer)
    std::cerr << "writer: bytes=" << writer->BytesWritten()
              << " writes=" << writer->WriteCalls()
              << " syncs=" << writer->SyncsPerformed()
              << " max_lag_ms=" << writer->MaxUnwrittenAgeMilliseconds()
              << '\n';

  if (outcome.exitCode == ExitCode::Success) {
    std::string quitError;
    PacketChannel quitChannel(transport, SOURCE_CHANNEL_OPTIONS);
    quitChannel.WritePacket(
        ComQuitCommand::Encode(),
        quitError);  // best-effort, matching a real client's mysql_close()
  }
  transport.Close();
  return static_cast<int>(outcome.exitCode);
}
