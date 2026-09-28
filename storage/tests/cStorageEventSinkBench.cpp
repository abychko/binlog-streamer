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

#include "cache/hCacheDefaults.hpp"

#include "binlog/cBinlogFileName.hpp"
#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "gtid/cGtidSet.hpp"
#include "receiver/cEventCounterSink.hpp"
#include "receiver/iEventSink.hpp"
#include "receiver/sStreamPosition.hpp"
#include "storage/cFileCursor.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageEventSink.hpp"
#include "storage/cStorageReader.hpp"
#include "storage/cStorageWriter.hpp"
#include "storage/eNextFileOutcome.hpp"
#include "storage/eWaitOutcome.hpp"
#include "storage/sPublishedPosition.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#else
#include <sys/resource.h>
#endif

namespace binlog_streamer {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t BENCH_CHUNK_SIZE = 1024 * 1024;
// 256 MiB spreads a multi-gigabyte dataset over many files without
// catalog bookkeeping dominating the result.
constexpr std::uint64_t ROTATE_THRESHOLD_BYTES = 256ULL * 1024 * 1024;
// Matches packaging/settings.yml's own cache.window default: replicas
// lagging by less than this are treated as almost in sync.
constexpr double DEFAULT_WINDOW_SECONDS = 60.0;
// Spread across the whole dataset, not all starting at the same point.
constexpr std::array<double, 4> HISTORIAN_START_FRACTIONS{0.0, 0.25, 0.50,
                                                          0.90};

// ---------------------------------------------------------------------------
// Byte-size / rate parsing
// ---------------------------------------------------------------------------

// Independent copy of config/cByteSizeParser.hpp's convention, kept
// private rather than shared across modules.
bool ParseByteSize(std::string_view text, std::uint64_t &value) {
  if (text.size() < 2) return false;
  const auto suffixPos = std::string_view("kKmMgGtT").find(text.back());
  if (suffixPos == std::string_view::npos) return false;
  const auto numberText = text.substr(0, text.size() - 1);
  if (numberText.empty() ||
      numberText.find_first_not_of("0123456789") != std::string_view::npos)
    return false;
  std::uint64_t parsed = 0;
  for (const char digit : numberText) {
    const std::uint64_t next =
        parsed * 10 + static_cast<std::uint64_t>(digit - '0');
    if (next < parsed) return false;
    parsed = next;
  }
  const std::uint64_t multiplier = std::uint64_t{1}
                                   << (10 * (suffixPos / 2 + 1));
  if (parsed > UINT64_MAX / multiplier) return false;
  value = parsed * multiplier;
  return true;
}

// --rate is decimal MB/s (10^6 bytes/s, the usual network-throughput unit),
// unlike --dataset/--large-event's binary units above - deliberate, not
// accidental.
bool ParseDouble(std::string_view text, double &value) {
  if (text.empty()) return false;
  std::size_t consumed = 0;
  try {
    value = std::stod(std::string(text), &consumed);
  } catch (...) {
    return false;
  }
  return consumed == text.size();
}

bool ParseUint64(std::string_view text, std::uint64_t &value) {
  if (text.empty()) return false;
  std::size_t consumed = 0;
  try {
    value = std::stoull(std::string(text), &consumed);
  } catch (...) {
    return false;
  }
  return consumed == text.size();
}

// ---------------------------------------------------------------------------
// Wire-format helpers
// ---------------------------------------------------------------------------

void AppendLE(std::vector<std::uint8_t> &out, std::uint64_t value,
              std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::vector<std::uint8_t> Lenenc(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  if (value < 251) {
    out.push_back(static_cast<std::uint8_t>(value));
  } else if (value < 65536) {
    out.push_back(0xFC);
    AppendLE(out, value, 2);
  } else {
    out.push_back(0xFE);
    AppendLE(out, value, 8);
  }
  return out;
}

std::vector<std::uint8_t> GtidBody(std::int64_t gno,
                                   std::uint64_t transactionLength) {
  std::vector<std::uint8_t> body;
  body.push_back(0x01);
  body.insert(body.end(), 16, std::uint8_t{0});
  AppendLE(body, static_cast<std::uint64_t>(gno), 8);
  body.push_back(0x02);
  AppendLE(body, 1, 8);
  AppendLE(body, 2, 8);
  AppendLE(body, 0x0001'2345'6789ULL, 7);
  const auto encoded = Lenenc(transactionLength);
  body.insert(body.end(), encoded.begin(), encoded.end());
  return body;
}

std::vector<std::uint8_t> SampleFde() {
  std::vector<std::uint8_t> body(57, 0x00);
  body[0] = 4;
  const std::string version = "8.4.11";
  for (std::size_t i = 0; i < version.size(); ++i)
    body[2 + i] = static_cast<std::uint8_t>(version[i]);
  body[56] = 19;
  body.push_back(0x01);
  body.insert(body.end(), 4, 0x00);
  return body;
}

std::vector<std::uint8_t> SamplePreviousGtids() {
  auto body = GtidSet().Encode(/*skipTaggedGtids=*/false);
  body.insert(body.end(), 4, 0x00);
  return body;
}

std::vector<std::uint8_t> RotateBody(std::uint64_t position,
                                     const std::string &fileName) {
  std::vector<std::uint8_t> body;
  AppendLE(body, position, 8);
  body.insert(body.end(), fileName.begin(), fileName.end());
  return body;
}

struct WireEvent {
  EventHeader header;
  std::vector<std::uint8_t> bytes;
};

WireEvent MakeEvent(std::uint8_t type, std::span<const std::uint8_t> body,
                    std::uint32_t nextPosition, std::uint16_t flags = 0) {
  WireEvent event;
  event.header.timestamp = (flags & EVENT_FLAG_ARTIFICIAL) ? 0 : 1700000000;
  event.header.type = type;
  event.header.serverId = 1;
  event.header.eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + body.size());
  event.header.nextPosition = nextPosition;
  event.header.flags = flags;
  event.bytes.reserve(event.header.eventLength);
  AppendLE(event.bytes, event.header.timestamp, 4);
  event.bytes.push_back(event.header.type);
  AppendLE(event.bytes, event.header.serverId, 4);
  AppendLE(event.bytes, event.header.eventLength, 4);
  AppendLE(event.bytes, event.header.nextPosition, 4);
  AppendLE(event.bytes, event.header.flags, 2);
  event.bytes.insert(event.bytes.end(), body.begin(), body.end());
  return event;
}

bool Drive(EventSink &sink, const WireEvent &event,
           const StreamPosition &position) {
  if (!sink.OnEventBegin(event.header, position)) return false;
  if (!sink.OnEventBytes(event.bytes)) return false;
  return sink.OnEventEnd();
}

// One buffer, filled once, sliced per chunk, so --large-event never holds
// the whole event body in memory at once.
const std::vector<std::uint8_t> &ChunkFillTemplate() {
  static const std::vector<std::uint8_t> chunk(BENCH_CHUNK_SIZE, 0xAB);
  return chunk;
}

bool DriveChunked(EventSink &sink, std::uint8_t type, std::uint64_t bodySize,
                  std::uint32_t nextPosition, const StreamPosition &position) {
  EventHeader header;
  header.timestamp = 1700000000;
  header.type = type;
  header.serverId = 1;
  header.eventLength =
      static_cast<std::uint32_t>(EVENT_HEADER_LENGTH + bodySize);
  header.nextPosition = nextPosition;
  header.flags = 0;
  if (!sink.OnEventBegin(header, position)) return false;

  std::vector<std::uint8_t> headerBytes;
  headerBytes.reserve(EVENT_HEADER_LENGTH);
  AppendLE(headerBytes, header.timestamp, 4);
  headerBytes.push_back(header.type);
  AppendLE(headerBytes, header.serverId, 4);
  AppendLE(headerBytes, header.eventLength, 4);
  AppendLE(headerBytes, header.nextPosition, 4);
  AppendLE(headerBytes, header.flags, 2);

  std::uint64_t bodySent = 0;
  {
    const std::uint64_t firstBodyChunk = std::min<std::uint64_t>(
        bodySize, BENCH_CHUNK_SIZE - headerBytes.size());
    std::vector<std::uint8_t> firstChunk = headerBytes;
    firstChunk.insert(
        firstChunk.end(), ChunkFillTemplate().begin(),
        ChunkFillTemplate().begin() + static_cast<long>(firstBodyChunk));
    if (!sink.OnEventBytes(firstChunk)) return false;
    bodySent += firstBodyChunk;
  }
  while (bodySent < bodySize) {
    const std::uint64_t thisChunk =
        std::min<std::uint64_t>(bodySize - bodySent, BENCH_CHUNK_SIZE);
    if (!sink.OnEventBytes(std::span<const std::uint8_t>(
            ChunkFillTemplate().data(), thisChunk)))
      return false;
    bodySent += thisChunk;
  }
  return sink.OnEventEnd();
}

struct FreshFileHeader {
  std::uint64_t afterPge = 0;
};

bool OpenFreshFile(EventSink &sink, const std::string &fileName,
                   FreshFileHeader &out, std::string &error) {
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate),
                RotateBody(4, fileName), 0, EVENT_FLAG_ARTIFICIAL);
  if (!Drive(sink, rotate, StreamPosition{fileName, 0})) {
    error = "artificial ROTATE refused";
    return false;
  }

  const auto fde = SampleFde();
  const std::uint64_t afterFde = 4 + EVENT_HEADER_LENGTH + fde.size();
  const auto fdeEvent =
      MakeEvent(static_cast<std::uint8_t>(EventType::FormatDescription), fde,
                static_cast<std::uint32_t>(afterFde));
  if (!Drive(sink, fdeEvent, StreamPosition{fileName, 4})) {
    error = "Format_description_event refused";
    return false;
  }

  const auto pge = SamplePreviousGtids();
  const std::uint64_t afterPge = afterFde + EVENT_HEADER_LENGTH + pge.size();
  const auto pgeEvent =
      MakeEvent(static_cast<std::uint8_t>(EventType::PreviousGtids), pge,
                static_cast<std::uint32_t>(afterPge));
  if (!Drive(sink, pgeEvent, StreamPosition{fileName, afterFde})) {
    error = "Previous_gtids_event refused";
    return false;
  }

  out.afterPge = afterPge;
  return true;
}

bool CloseFileWithRotate(EventSink &sink, const std::string &currentFile,
                         std::uint64_t offset, const std::string &nextFile) {
  const auto body = RotateBody(4, nextFile);
  const auto rotateEventLength = EVENT_HEADER_LENGTH + body.size();
  // This generator claims correct positions throughout, unlike unit tests
  // that use an arbitrary ROTATE placeholder.
  const auto rotate =
      MakeEvent(static_cast<std::uint8_t>(EventType::Rotate), body,
                static_cast<std::uint32_t>(offset + rotateEventLength));
  return Drive(sink, rotate, StreamPosition{currentFile, offset});
}

struct DrivenGroup {
  WireEvent gtid;
  WireEvent query;
  WireEvent xid;
};

std::uint64_t GroupWireLength(const DrivenGroup &group) {
  return group.gtid.bytes.size() + group.query.bytes.size() +
         group.xid.bytes.size();
}

// Sized so an OLTP group's on-wire length is exactly 744 bytes, a plausible
// small UPDATE/INSERT; 744 stays in the same lenenc width class regardless
// of the placeholder used to size the GTID event.
constexpr std::size_t OLTP_QUERY_BODY_SIZE = 627;

const std::vector<std::uint8_t> &OltpQueryBodyTemplate() {
  static const std::vector<std::uint8_t> body(OLTP_QUERY_BODY_SIZE, 0xAB);
  return body;
}

DrivenGroup MakeOltpGroup(std::uint64_t start, std::int64_t gno) {
  const std::size_t xidEventLength = EVENT_HEADER_LENGTH + 8;
  const std::size_t queryEventLength =
      EVENT_HEADER_LENGTH + OLTP_QUERY_BODY_SIZE;
  const std::size_t gtidEventLength =
      EVENT_HEADER_LENGTH +
      GtidBody(gno, /*placeholder, same lenenc width*/ 1000).size();
  const std::uint64_t transactionLength =
      gtidEventLength + queryEventLength + xidEventLength;

  DrivenGroup group;
  group.gtid = MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                         GtidBody(gno, transactionLength),
                         static_cast<std::uint32_t>(start + gtidEventLength));
  group.query = MakeEvent(
      2 /* a Query event - no EventType enumerator, eEventType.hpp */,
      OltpQueryBodyTemplate(),
      static_cast<std::uint32_t>(start + gtidEventLength + queryEventLength));
  group.xid =
      MakeEvent(16 /* Xid event */, std::vector<std::uint8_t>(8, 0xCD),
                static_cast<std::uint32_t>(start + gtidEventLength +
                                           queryEventLength + xidEventLength));
  return group;
}

bool DriveGroup(EventSink &sink, const DrivenGroup &group,
                const std::string &fileName, std::uint64_t start) {
  std::uint64_t offset = start;
  if (!Drive(sink, group.gtid, StreamPosition{fileName, offset})) return false;
  offset += group.gtid.bytes.size();
  if (!Drive(sink, group.query, StreamPosition{fileName, offset})) return false;
  offset += group.query.bytes.size();
  return Drive(sink, group.xid, StreamPosition{fileName, offset});
}

// ---------------------------------------------------------------------------
// /proc + cgroup v2 readings (Linux only; see MacPeakRssBytes() for macOS)
// ---------------------------------------------------------------------------

struct MemorySample {
  bool available = false;
  // false: callers must not read pgstealStart/End or the refault fields.
  bool keysOk = true;
  std::string error;
  std::uint64_t maxFile = 0;
  std::uint64_t maxAnon = 0;
  // "pgsteal", not "pgsteal_file": this kernel's memory.stat has no such
  // split. Under MemorySwapMax=0, anon is never a reclaim target, so
  // cgroup-scoped "pgsteal" here is page-cache reclaim.
  std::uint64_t pgstealStart = 0;
  std::uint64_t pgstealEnd = 0;
  std::uint64_t workingsetRefaultFileStart = 0;
  std::uint64_t workingsetRefaultFileEnd = 0;
};

#if defined(__linux__)

// false+error on a real read failure, not a silent 0: callers must tell
// "nothing read from disk" apart from a broken /proc read.
bool ReadProcIoReadBytes(const std::string &path, std::uint64_t &value,
                         std::string &error) {
  std::ifstream file(path);
  if (!file) {
    error = "could not open " + path;
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("read_bytes:", 0) == 0) {
      value = std::strtoull(line.c_str() + 11, nullptr, 10);
      return true;
    }
  }
  error = path + " has no read_bytes: line";
  return false;
}

bool ThreadReadBytes(std::uint64_t &value, std::string &error) {
  const auto tid = static_cast<long>(::syscall(SYS_gettid));
  return ReadProcIoReadBytes("/proc/self/task/" + std::to_string(tid) + "/io",
                             value, error);
}

bool ProcessReadBytes(std::uint64_t &value, std::string &error) {
  return ReadProcIoReadBytes("/proc/self/io", value, error);
}

std::uint64_t ReadRssAnonBytes() {
  std::ifstream file("/proc/self/status");
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("RssAnon:", 0) == 0)
      return std::strtoull(line.c_str() + 8, nullptr, 10) * 1024;
  }
  return 0;
}

bool ReadMemoryStat(const std::string &path,
                    std::map<std::string, std::uint64_t> &fields) {
  std::ifstream file(path);
  if (!file) return false;
  std::string key;
  std::uint64_t value = 0;
  while (file >> key >> value) fields[key] = value;
  return true;
}

// Refuses to fall back to 0 like std::map::operator[] would: a missing key
// must surface as a loud fault, not blend into "nothing happened".
bool RequireMemoryStatField(const std::map<std::string, std::uint64_t> &fields,
                            const char *key, std::uint64_t &out) {
  const auto it = fields.find(key);
  if (it == fields.end()) return false;
  out = it->second;
  return true;
}

// The "0::" line: unified (v2) hierarchy only, not v1's comma-separated
// controller list.
std::optional<std::string> CgroupMemoryStatPath() {
  std::ifstream file("/proc/self/cgroup");
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("0::", 0) == 0)
      return "/sys/fs/cgroup" + line.substr(3) + "/memory.stat";
  }
  return std::nullopt;
}

class MemorySampler {
 public:
  explicit MemorySampler(std::string statPath)
      : m_statPath(std::move(statPath)) {}
  void Start() {
    m_thread = std::thread([this] { Run(); });
  }
  void Stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
  }
  MemorySample Result() const {
    return m_sample;
  }  // safe: only read after Stop() has joined m_thread

 private:
  void Run() {
    bool first = true;
    while (!m_stop.load(std::memory_order_relaxed)) {
      std::map<std::string, std::uint64_t> fields;
      if (!ReadMemoryStat(m_statPath, fields)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;  // transient: not the same as a missing key, below
      }
      m_sample.available = true;
      std::uint64_t file = 0, anon = 0, pgsteal = 0, refault = 0;
      if (!RequireMemoryStatField(fields, "file", file) ||
          !RequireMemoryStatField(fields, "anon", anon) ||
          !RequireMemoryStatField(fields, "pgsteal", pgsteal) ||
          !RequireMemoryStatField(fields, "workingset_refault_file", refault)) {
        m_sample.keysOk = false;
        m_sample.error = m_statPath +
                         " is missing an expected key (kernel/cgroup memory "
                         "controller mismatch)";
        return;  // systemic: the key stays missing on every later read too
      }
      if (first) {
        m_sample.pgstealStart = pgsteal;
        m_sample.workingsetRefaultFileStart = refault;
        first = false;
      }
      m_sample.maxFile = std::max(m_sample.maxFile, file);
      m_sample.maxAnon = std::max(m_sample.maxAnon, anon);
      m_sample.pgstealEnd = pgsteal;
      m_sample.workingsetRefaultFileEnd = refault;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  std::string m_statPath;
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
  MemorySample m_sample;
};

#else  // !__linux__

bool ThreadReadBytes(std::uint64_t &value, std::string &) {
  value = 0;  // no per-thread I/O accounting on macOS; not a read failure
  return true;
}
bool ProcessReadBytes(std::uint64_t &value, std::string &) {
  value = 0;
  return true;
}
// Shared calling code (below) calls this on every platform.
std::optional<std::string> CgroupMemoryStatPath() { return std::nullopt; }

class MemorySampler {
 public:
  explicit MemorySampler(std::string) {}
  void Start() {}
  void Stop() {}
  MemorySample Result() const { return {}; }
};

// ru_maxrss is bytes on macOS (unlike Linux's KB) and a lifetime peak, not
// a delta - printed once, not compared like the Linux snapshots.
std::uint64_t MacPeakRssBytes() {
  struct rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0;
  return static_cast<std::uint64_t>(usage.ru_maxrss);
}

#endif

// ---------------------------------------------------------------------------
// Timing percentiles
// ---------------------------------------------------------------------------

struct Percentiles {
  double p50Us = 0;
  double p99Us = 0;
  double maxUs = 0;
};

Percentiles ComputePercentiles(std::vector<double> samplesUs) {
  if (samplesUs.empty()) return {};
  std::sort(samplesUs.begin(), samplesUs.end());
  const auto at = [&](double fraction) {
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(samplesUs.size() - 1));
    return samplesUs[index];
  };
  Percentiles result;
  result.p50Us = at(0.50);
  result.p99Us = at(0.99);
  result.maxUs = samplesUs.back();
  return result;
}

// ---------------------------------------------------------------------------
// Rate limiting
// ---------------------------------------------------------------------------

class TokenBucket {
 public:
  explicit TokenBucket(double bytesPerSecond)
      : m_rate(bytesPerSecond), m_last(Clock::now()) {}

  void Consume(std::uint64_t bytes) {
    if (m_rate <= 0.0) return;
    m_tokens -= static_cast<double>(bytes);
    while (m_tokens < 0.0) {
      const auto now = Clock::now();
      m_tokens += std::chrono::duration<double>(now - m_last).count() * m_rate;
      m_last = now;
      if (m_tokens < 0.0)
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

 private:
  double m_rate;
  double m_tokens = 0.0;
  Clock::time_point m_last;
};

// ---------------------------------------------------------------------------
// Ingest: dataset prep (unthrottled) and live phase (rate limited, timed)
// ---------------------------------------------------------------------------

struct FileBoundary {
  std::string name;
  std::uint64_t cumulativeStart = 0;
};

struct IngestState {
  std::string currentFile;
  std::uint64_t currentFileOffset = 0;
  std::uint64_t fileNumber = 0;
  std::uint64_t cumulativeBytes = 0;
  std::int64_t nextGno = 1;
};

// Shared by dataset-prep and the live phase, so --mixed historians walk
// through multi-file history, not one giant file.
bool RotateIfNeeded(StorageEventSink &sink, IngestState &state,
                    std::vector<FileBoundary> *boundaries, std::string &error) {
  if (!state.currentFile.empty() &&
      state.currentFileOffset < ROTATE_THRESHOLD_BYTES)
    return true;
  const std::string nextFile =
      BinlogFileName::Format("binlog", ++state.fileNumber);
  if (!state.currentFile.empty() &&
      !CloseFileWithRotate(sink, state.currentFile, state.currentFileOffset,
                           nextFile)) {
    error = "genuine ROTATE refused";
    return false;
  }
  FreshFileHeader header;
  if (!OpenFreshFile(sink, nextFile, header, error)) return false;
  if (boundaries) boundaries->push_back({nextFile, state.cumulativeBytes});
  state.currentFile = nextFile;
  state.currentFileOffset = header.afterPge;
  return true;
}

// boundaries feeds LocateFraction() below.
bool WriteDataset(StorageEventSink &sink, std::uint64_t targetBytes,
                  IngestState &state, std::vector<FileBoundary> &boundaries,
                  std::string &error) {
  while (state.cumulativeBytes < targetBytes) {
    if (!RotateIfNeeded(sink, state, &boundaries, error)) return false;
    const auto group = MakeOltpGroup(state.currentFileOffset, state.nextGno++);
    if (!DriveGroup(sink, group, state.currentFile, state.currentFileOffset)) {
      error = "dataset group refused";
      return false;
    }
    const auto length = GroupWireLength(group);
    state.currentFileOffset += length;
    state.cumulativeBytes += length;
  }
  return true;
}

struct LiveIngestResult {
  bool ok = true;
  std::string error;
  std::uint64_t groupsEmitted = 0;
  std::uint64_t bytesEmitted = 0;
  std::vector<double> groupDurationsUs;
};

// The measured phase behind the groups/s, MB/s and p50/p99/max numbers.
LiveIngestResult EmitLiveGroups(StorageEventSink &sink, IngestState &state,
                                std::uint64_t groupCount, TokenBucket *rate) {
  LiveIngestResult result;
  result.groupDurationsUs.reserve(groupCount);
  for (std::uint64_t i = 0; i < groupCount; ++i) {
    std::string rotateError;
    if (!RotateIfNeeded(sink, state, nullptr, rotateError)) {
      result.ok = false;
      result.error = rotateError;
      return result;
    }
    const auto group = MakeOltpGroup(state.currentFileOffset, state.nextGno++);
    const auto start = Clock::now();
    const bool ok =
        DriveGroup(sink, group, state.currentFile, state.currentFileOffset);
    const auto elapsedUs =
        std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    if (!ok) {
      result.ok = false;
      result.error = "live group refused";
      return result;
    }
    result.groupDurationsUs.push_back(elapsedUs);
    const auto length = GroupWireLength(group);
    state.currentFileOffset += length;
    state.cumulativeBytes += length;
    result.groupsEmitted++;
    result.bytesEmitted += length;
    if (rate) rate->Consume(length);
  }
  return result;
}

// One oversized group, its Query event driven in BENCH_CHUNK_SIZE portions
// so memory stays bounded. Bypasses MAX_EVENT_LENGTH and the 4 GiB
// wire-position wrap since this generator drives the sink directly.
bool RunLargeEvent(StorageEventSink &sink, std::uint64_t largeEventBytes,
                   std::string &error) {
  FreshFileHeader header;
  if (!OpenFreshFile(sink, "binlog.000001", header, error)) return false;
  const std::uint64_t start = header.afterPge;

  const std::size_t xidEventLength = EVENT_HEADER_LENGTH + 8;
  const std::size_t queryEventLength = EVENT_HEADER_LENGTH + largeEventBytes;
  // largeEventBytes already exceeds Lenenc()'s 65536 threshold, so the
  // GTID event's lenenc width is fixed regardless of the placeholder used here.
  const std::size_t gtidEventLength =
      EVENT_HEADER_LENGTH + GtidBody(1, 1'000'000'000).size();
  const std::uint64_t transactionLength =
      gtidEventLength + queryEventLength + xidEventLength;

  const auto gtidEvent =
      MakeEvent(static_cast<std::uint8_t>(EventType::Gtid),
                GtidBody(1, transactionLength),
                static_cast<std::uint32_t>(start + gtidEventLength));
  if (!Drive(sink, gtidEvent, StreamPosition{"binlog.000001", start})) {
    error = "GTID event refused";
    return false;
  }

  if (!DriveChunked(sink, 2 /* Query */, largeEventBytes,
                    static_cast<std::uint32_t>(start + gtidEventLength +
                                               queryEventLength),
                    StreamPosition{"binlog.000001", start + gtidEventLength})) {
    error = "large Query event refused";
    return false;
  }

  const auto xidEvent =
      MakeEvent(16 /* Xid */, std::vector<std::uint8_t>(8, 0xCD),
                static_cast<std::uint32_t>(start + gtidEventLength +
                                           queryEventLength + xidEventLength));
  if (!Drive(sink, xidEvent,
             StreamPosition{"binlog.000001",
                            start + gtidEventLength + queryEventLength})) {
    error = "Xid event refused";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

enum class CaughtUpBehavior { Wait, Loop, Stop };

struct ReaderStats {
  bool ok = true;
  std::string error;
  ReaderCounters cacheCounters;
  std::uint64_t bytesRead = 0;
  std::uint64_t threadIoStart = 0;
  std::uint64_t threadIoEnd = 0;
  std::uint64_t processIoStart = 0;
  std::uint64_t processIoEnd = 0;
  // false means an I/O accounting call failed; the *IoStart/*IoEnd fields
  // are then not trustworthy deltas and must not feed a witness silently.
  bool ioAccountingOk = true;
  std::string ioAccountingError;
};

// Destructor, not an explicit call at the end: must run on every early
// return too, not just the ordinary exit.
struct EndIoCapture {
  ReaderStats &stats;
  StorageReader &reader;
  ~EndIoCapture() {
    stats.cacheCounters = reader.Counters();
    std::string ioError;
    if (!ThreadReadBytes(stats.threadIoEnd, ioError)) {
      stats.ioAccountingOk = false;
      stats.ioAccountingError = ioError;
    }
    if (!ProcessReadBytes(stats.processIoEnd, ioError)) {
      stats.ioAccountingOk = false;
      stats.ioAccountingError = ioError;
    }
  }
};

void RunReaderLoop(StorageReader &reader, const std::string &startFile,
                   std::uint64_t startOffset,
                   const std::atomic<bool> &stopRequested,
                   CaughtUpBehavior onCaughtUp,
                   const std::function<bool(std::uint64_t)> &pace,
                   std::atomic<std::uint64_t> *aggregateBytesRead,
                   ReaderStats &stats) {
  {
    std::string ioError;
    if (!ThreadReadBytes(stats.threadIoStart, ioError)) {
      stats.ioAccountingOk = false;
      stats.ioAccountingError = ioError;
    }
    if (!ProcessReadBytes(stats.processIoStart, ioError)) {
      stats.ioAccountingOk = false;
      stats.ioAccountingError = ioError;
    }
  }
  const EndIoCapture endIoCapture{stats, reader};

  std::string openError;
  auto cursor = reader.Open(startFile, openError);
  if (!cursor) {
    stats.ok = false;
    stats.error = openError;
    return;
  }
  std::uint64_t offset = startOffset;
  std::vector<std::uint8_t> buffer(BENCH_CHUNK_SIZE);

  while (!stopRequested.load(std::memory_order_relaxed)) {
    if (pace && !pace(stats.bytesRead)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    std::string readError;
    const std::size_t bytesRead =
        reader.Read(*cursor, offset, buffer, readError);
    if (!readError.empty()) {
      stats.ok = false;
      stats.error = readError;
      return;
    }
    if (bytesRead > 0) {
      offset += bytesRead;
      stats.bytesRead += bytesRead;
      if (aggregateBytesRead)
        aggregateBytesRead->fetch_add(bytesRead, std::memory_order_relaxed);
      continue;
    }
    if (reader.Published().fileName == cursor->FileName()) {
      if (onCaughtUp == CaughtUpBehavior::Loop) {
        auto restarted = reader.Open(startFile, openError);
        if (!restarted) {
          stats.ok = false;
          stats.error = openError;
          return;
        }
        cursor = std::move(restarted);
        offset = startOffset;
        continue;
      }
      if (onCaughtUp == CaughtUpBehavior::Stop) return;
      reader.WaitForNewEvents(PublishedPosition{cursor->FileName(), offset},
                              std::chrono::milliseconds(20),
                              WaitStyle::PollFirst);
      continue;
    }
    std::unique_ptr<FileCursor> next;
    std::string nextError;
    const NextFileOutcome outcome =
        reader.Next(*cursor, offset, next, nextError);
    if (outcome == NextFileOutcome::Found) {
      cursor = std::move(next);
      offset = 0;
      continue;
    }
    if (outcome == NextFileOutcome::NotYetAvailable) {
      reader.WaitForNewEvents(PublishedPosition{cursor->FileName(), offset},
                              std::chrono::milliseconds(20),
                              WaitStyle::PollFirst);
      continue;
    }
    stats.ok = false;
    stats.error = nextError;
    return;
  }
}

std::pair<std::string, std::uint64_t> LocateFraction(
    const std::vector<FileBoundary> &boundaries, std::uint64_t datasetBytes,
    double fraction) {
  const auto target =
      static_cast<std::uint64_t>(fraction * static_cast<double>(datasetBytes));
  std::size_t chosen = 0;
  for (std::size_t i = 0; i < boundaries.size(); ++i) {
    if (boundaries[i].cumulativeStart <= target)
      chosen = i;
    else
      break;
  }
  return {boundaries[chosen].name, target - boundaries[chosen].cumulativeStart};
}

// ---------------------------------------------------------------------------
// Free space check
// ---------------------------------------------------------------------------

bool HasEnoughFreeSpace(const std::filesystem::path &dataDir,
                        std::uint64_t datasetBytes, std::string &error) {
  std::error_code ec;
  const auto info = std::filesystem::space(dataDir, ec);
  if (ec) {
    error = "statvfs failed: " + ec.message();
    return false;
  }
  const auto required = datasetBytes + datasetBytes / 2;
  if (info.available < required) {
    std::ostringstream out;
    out << "not enough free space: available " << info.available
        << " byte(s), need " << required << " (dataset × 1.5)";
    error = out.str();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct BenchOptions {
  enum class Mode {
    IngestOrReaders,
    Mixed,
    LargeEvent,
    FsyncControl
  } mode = Mode::IngestOrReaders;
  std::optional<std::uint64_t> groups;
  std::uint64_t readers = 0;
  double readersLagSeconds = 0.0;
  std::uint64_t mixedNearSync = 0;
  std::uint64_t mixedHistorians = 0;
  bool mixedNoLoop = false;
  std::chrono::seconds::rep windowSeconds =
      static_cast<std::chrono::seconds::rep>(DEFAULT_WINDOW_SECONDS);
  std::optional<std::uint64_t> datasetBytes;
  // Matches the shipped settings template; explicit --cache-size overrides it.
  std::uint64_t cacheSize = 2ULL * 1024 * 1024 * 1024;
  double rateMegabytesPerSecond = 0.0;  // 0 = unlimited
  // Per historian, 0 = as fast as memory copies; unpaced, several of them
  // would measure memory bandwidth instead of a realistic catch-up rate.
  double historianRateMegabytesPerSecond = 0.0;
  std::optional<std::uint64_t> largeEventBytes;
  // process-io|thread-io: which read_bytes attribution feeds the "official"
  // summary; both are always computed and printed regardless of this choice.
  enum class Metric { ThreadIo, ProcessIo } metric = Metric::ThreadIo;
  std::filesystem::path dataDir;
};

bool NextArg(int argc, char **argv, int &index, std::string_view flag,
             std::string &out) {
  if (index + 1 >= argc) {
    std::cerr << "option requires a value: " << flag << "\n";
    return false;
  }
  out = argv[++index];
  return true;
}

// checksumLength is always 0 in this bench: StorageEventSink never verifies
// checksum bytes, and no metric this bench reports is affected by their
// presence.
bool ParseArgs(int argc, char **argv, BenchOptions &options,
               std::string &error) {
  bool sawLargeEvent = false, sawFsync = false, sawMixed = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    std::string value;
    if (arg == "--groups") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --groups";
        return false;
      }
      if (!ParseUint64(value, options.groups.emplace())) {
        error = "bad --groups value";
        return false;
      }
    } else if (arg == "--readers") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --readers";
        return false;
      }
      if (!ParseUint64(value, options.readers)) {
        error = "bad --readers value";
        return false;
      }
    } else if (arg == "--readers-lag") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --readers-lag";
        return false;
      }
      if (!ParseDouble(value, options.readersLagSeconds)) {
        error = "bad --readers-lag value";
        return false;
      }
    } else if (arg == "--mixed") {
      sawMixed = true;
      options.mode = BenchOptions::Mode::Mixed;
      if (i + 2 >= argc) {
        error = "--mixed requires two values: N M";
        return false;
      }
      if (!ParseUint64(argv[i + 1], options.mixedNearSync) ||
          !ParseUint64(argv[i + 2], options.mixedHistorians)) {
        error = "bad --mixed values: N M must both be non-negative integers";
        return false;
      }
      i += 2;
    } else if (arg == "--mixed-no-loop") {
      options.mixedNoLoop = true;
    } else if (arg == "--window") {
      std::uint64_t seconds = 0;
      if (!NextArg(argc, argv, i, arg, value) ||
          value.find_first_not_of("0123456789") != std::string::npos ||
          !ParseUint64(value, seconds) || seconds == 0 ||
          seconds >
              static_cast<std::uint64_t>(std::chrono::seconds::max().count())) {
        error = "bad --window value";
        return false;
      }
      options.windowSeconds = static_cast<std::chrono::seconds::rep>(seconds);
    } else if (arg == "--dataset") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --dataset";
        return false;
      }
      std::uint64_t parsed = 0;
      if (!ParseByteSize(value, parsed)) {
        error = "bad --dataset value";
        return false;
      }
      options.datasetBytes = parsed;
    } else if (arg == "--cache-size") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --cache-size";
        return false;
      }
      std::uint64_t parsed = 0;
      if (!ParseByteSize(value, parsed) || parsed < 2 * CACHE_SEGMENT_SIZE) {
        error = "--cache-size must be at least 2M";
        return false;
      }
      options.cacheSize = parsed;
    } else if (arg == "--rate") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --rate";
        return false;
      }
      if (!ParseDouble(value, options.rateMegabytesPerSecond)) {
        error = "bad --rate value";
        return false;
      }
    } else if (arg == "--historian-rate") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --historian-rate";
        return false;
      }
      if (!ParseDouble(value, options.historianRateMegabytesPerSecond)) {
        error = "bad --historian-rate value";
        return false;
      }
    } else if (arg == "--large-event") {
      sawLargeEvent = true;
      options.mode = BenchOptions::Mode::LargeEvent;
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --large-event";
        return false;
      }
      std::uint64_t parsed = 0;
      if (!ParseByteSize(value, parsed)) {
        error = "bad --large-event value";
        return false;
      }
      options.largeEventBytes = parsed;
    } else if (arg == "--fsync-every-group") {
      sawFsync = true;
      options.mode = BenchOptions::Mode::FsyncControl;
    } else if (arg == "--metric") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --metric";
        return false;
      }
      if (value == "thread-io")
        options.metric = BenchOptions::Metric::ThreadIo;
      else if (value == "process-io")
        options.metric = BenchOptions::Metric::ProcessIo;
      else {
        error = "--metric must be thread-io or process-io";
        return false;
      }
    } else if (arg == "--data-dir") {
      if (!NextArg(argc, argv, i, arg, value)) {
        error = "bad --data-dir";
        return false;
      }
      options.dataDir = value;
    } else {
      error = "unknown option: " + std::string(arg);
      return false;
    }
  }
  if (static_cast<int>(sawLargeEvent) + static_cast<int>(sawFsync) +
          static_cast<int>(sawMixed) >
      1) {
    error =
        "--large-event, --fsync-every-group and --mixed are mutually exclusive";
    return false;
  }
  if (options.mode == BenchOptions::Mode::LargeEvent)
    return true;  // --groups not needed
  if (!options.groups) {
    error = "--groups is required";
    return false;
  }
  if (options.mode == BenchOptions::Mode::Mixed) {
    if (!options.datasetBytes) {
      error = "--mixed requires --dataset";
      return false;
    }
    if (options.rateMegabytesPerSecond <= 0.0) {
      error = "--mixed requires --rate > 0 (lag pacing needs a fixed rate)";
      return false;
    }
  }
  if (options.readersLagSeconds > 0.0 &&
      options.rateMegabytesPerSecond <= 0.0) {
    error =
        "--readers-lag > 0 requires --rate > 0 (lag pacing needs a fixed rate)";
    return false;
  }
  return true;
}

std::filesystem::path MakeDataDir(const std::filesystem::path &requested) {
  if (!requested.empty()) {
    std::filesystem::create_directories(requested);
    return requested;
  }
  auto path =
      (std::filesystem::temp_directory_path() / "storage_sink_bench_XXXXXX")
          .string();
  if (mkdtemp(path.data()) == nullptr)
    throw std::runtime_error(std::string("mkdtemp failed: ") +
                             std::strerror(errno));
  return path;
}

// Removes the data directory this run created for itself, RAII rather than
// a cleanup call before every one of the several early returns below. An
// operator-supplied --data-dir is left untouched for manual inspection.
class AutoCleanDataDir {
 public:
  AutoCleanDataDir(std::filesystem::path path, bool autoGenerated)
      : m_path(std::move(path)), m_autoGenerated(autoGenerated) {}
  ~AutoCleanDataDir() {
    if (m_autoGenerated) {
      std::error_code ec;
      std::filesystem::remove_all(m_path, ec);
    }
  }
  AutoCleanDataDir(const AutoCleanDataDir &) = delete;
  AutoCleanDataDir &operator=(const AutoCleanDataDir &) = delete;
  const std::filesystem::path &Path() const { return m_path; }

 private:
  std::filesystem::path m_path;
  bool m_autoGenerated;
};

void PrintIngestSummary(const std::string &label, std::uint64_t groups,
                        std::uint64_t bytes, double elapsedSeconds,
                        const Percentiles &p) {
  const double mbPerSecond =
      elapsedSeconds > 0
          ? static_cast<double>(bytes) / 1'000'000.0 / elapsedSeconds
          : 0.0;
  const double groupsPerSecond =
      elapsedSeconds > 0 ? static_cast<double>(groups) / elapsedSeconds : 0.0;
  std::cout << label << " groups=" << groups << " bytes=" << bytes
            << " elapsed_s=" << elapsedSeconds
            << " groups_per_s=" << groupsPerSecond
            << " MB_per_s=" << mbPerSecond << " p50_us=" << p.p50Us
            << " p99_us=" << p.p99Us << " max_us=" << p.maxUs << "\n";
}

// Never fails: a run under 100,000 groups (e.g. the ctest smoke invocation)
// is a deliberately smaller request, not a failure.
void PrintGroupsWitness(std::uint64_t requested) {
  if (requested >= 100000)
    std::cout << "WITNESS groups>=100000: requested=" << requested
              << " pass=1\n";
  else
    std::cout << "WITNESS groups>=100000: skipped (explicit --groups "
              << requested << ")\n";
}

void PrintCacheSummary(std::chrono::seconds::rep windowSeconds,
                       const CacheCounters &counters) {
  std::cout << "CACHE window_s=" << windowSeconds
            << " evicted_window=" << counters.evictedForWindow
            << " evicted_space=" << counters.evictedForSpace << "\n";
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

int RunIngestOrReaders(const BenchOptions &options) {
  const AutoCleanDataDir dataDirGuard(MakeDataDir(options.dataDir),
                                      options.dataDir.empty());
  const auto &dataDir = dataDirGuard.Path();
  std::cout << "data_dir=" << dataDir.string() << "\n";

  EventCounterSink counterSink;
  StorageCatalog catalog;
  PublishedPositionTracker published;
  std::atomic<bool> stopRequested{false};
  std::optional<EventCache> cache;
  if (options.cacheSize) {
    std::string cacheError;
    cache = EventCache::Reserve(options.cacheSize,
                                std::chrono::seconds{options.windowSeconds},
                                stopRequested, cacheError);
    if (!cache) {
      std::cerr << "cache: " << cacheError << "\n";
      return 4;
    }
  }
  EventCache *cachePointer = &*cache;
  StorageWriter writer(dataDir, *cache, catalog);
  StorageEventSink sink(dataDir, /*checksumLength=*/0, counterSink, catalog,
                        *cache, writer, &published);
  writer.Start();

  IngestState state;
  TokenBucket rate(options.rateMegabytesPerSecond * 1'000'000.0);
  TokenBucket *rateOrNull =
      options.rateMegabytesPerSecond > 0.0 ? &rate : nullptr;

  // Readers, if any, start at the very beginning: this mode has no
  // dataset-prep phase (unlike --mixed), so "the tail" and "the start"
  // are the same point when readers spawn.
  std::string rotateError;
  if (!RotateIfNeeded(sink, state, nullptr, rotateError)) {
    std::cerr << "storage: " << rotateError << "\n";
    return 4;
  }
  const std::string startFile = state.currentFile;
  const std::uint64_t startOffset = state.currentFileOffset;

  const auto benchStart = Clock::now();
  std::vector<std::thread> readerThreads;
  std::vector<ReaderStats> readerStats(options.readers);
  for (std::uint64_t i = 0; i < options.readers; ++i) {
    const double lag = options.readersLagSeconds;
    const double rateBytesPerSecond =
        options.rateMegabytesPerSecond * 1'000'000.0;
    std::function<bool(std::uint64_t)> pace;
    if (lag > 0.0) {
      pace = [benchStart, lag, rateBytesPerSecond](std::uint64_t bytesSoFar) {
        const double elapsed =
            std::chrono::duration<double>(Clock::now() - benchStart).count();
        const double allowedSeconds = elapsed - lag;
        if (allowedSeconds <= 0) return false;
        return static_cast<double>(bytesSoFar) <
               allowedSeconds * rateBytesPerSecond;
      };
    }
    readerThreads.emplace_back([&dataDir, &catalog, &published, cachePointer,
                                startFile, startOffset, &stopRequested, pace,
                                &stats = readerStats[i]] {
      StorageReader reader(dataDir, catalog, published, cachePointer);
      RunReaderLoop(reader, startFile, startOffset, stopRequested,
                    CaughtUpBehavior::Wait, pace, nullptr, stats);
    });
  }

  const LiveIngestResult live =
      EmitLiveGroups(sink, state, *options.groups, rateOrNull);
  std::string flushError;
  const bool flushed = writer.DrainAndSync();
  flushError = writer.LastError();
  std::cout << "WRITER bytes=" << writer.BytesWritten()
            << " max_lag_ms=" << writer.MaxUnwrittenAgeMilliseconds() << "\n";
  stopRequested = true;
  for (auto &t : readerThreads) t.join();

  if (!live.ok) {
    std::cerr << "storage: " << live.error << "\n";
    return 4;
  }
  if (!flushed) {
    std::cerr << "storage: failed to flush on shutdown: " << flushError << "\n";
    return 4;
  }
  const double elapsedSeconds =
      std::chrono::duration<double>(Clock::now() - benchStart).count();
  PrintIngestSummary("INGEST", live.groupsEmitted, live.bytesEmitted,
                     elapsedSeconds, ComputePercentiles(live.groupDurationsUs));
  PrintGroupsWitness(*options.groups);

  const auto cacheCounters = cache->Counters();
  PrintCacheSummary(options.windowSeconds, cacheCounters);
  std::uint64_t minFromDisk = std::numeric_limits<std::uint64_t>::max();
  int exitCode = 0;
  for (std::uint64_t i = 0; i < options.readers; ++i) {
    const auto &stats = readerStats[i];
    std::cout << "READER index=" << i << " lag_s=" << options.readersLagSeconds
              << " ok=" << stats.ok << " bytes_read=" << stats.bytesRead
              << " from_cache=" << stats.cacheCounters.readFromCache
              << " from_disk=" << stats.cacheCounters.readFromDisk
              << " from_disk_header=" << stats.cacheCounters.readFromDiskHeader
              << " from_disk_body="
              << (stats.cacheCounters.readFromDisk -
                  stats.cacheCounters.readFromDiskHeader)
              << " seam_crossings=" << stats.cacheCounters.seamCrossings
              << " io_accounting_ok=" << stats.ioAccountingOk
              << " thread_io_delta="
              << (stats.threadIoEnd - stats.threadIoStart)
              << " process_io_delta="
              << (stats.processIoEnd - stats.processIoStart) << "\n";
    minFromDisk = std::min(minFromDisk, stats.cacheCounters.readFromDisk);
    if (!stats.ok) {
      std::cerr << "reader " << i << " failed: " << stats.error << "\n";
      exitCode = 1;
    }
    if (!stats.ioAccountingOk)
      std::cerr << "reader " << i
                << " I/O accounting failed: " << stats.ioAccountingError
                << "\n";
  }
  if (options.readers > 0) {
    if (options.readersLagSeconds > options.windowSeconds) {
      const bool pass = cacheCounters.evictedForWindow > 0 && minFromDisk > 0;
      std::cout << "WITNESS window-eviction: pass=" << pass
                << " evicted_window=" << cacheCounters.evictedForWindow
                << " min_from_disk=" << minFromDisk << "\n";
      if (!pass) exitCode = 1;
    } else {
      std::cout << "WITNESS window-eviction: skipped (lag <= window)\n";
    }
  }
  if (live.groupsEmitted != *options.groups) {
    std::cerr << "WITNESS groups: incomplete run\n";
    exitCode = 1;
  }
  return exitCode;
}

int RunLargeEventMode(const BenchOptions &options) {
  const AutoCleanDataDir dataDirGuard(MakeDataDir(options.dataDir),
                                      options.dataDir.empty());
  const auto &dataDir = dataDirGuard.Path();
  std::cout << "data_dir=" << dataDir.string() << "\n";
  EventCounterSink counterSink;
  StorageCatalog catalog;
  PublishedPositionTracker published;
  std::atomic<bool> stopRequested{false};
  std::optional<EventCache> cache;
  if (options.cacheSize) {
    std::string cacheError;
    cache = EventCache::Reserve(options.cacheSize,
                                std::chrono::seconds{options.windowSeconds},
                                stopRequested, cacheError);
    if (!cache) {
      std::cerr << "cache: " << cacheError << "\n";
      return 4;
    }
  }
  StorageWriter writer(dataDir, *cache, catalog);
  StorageEventSink sink(dataDir, /*checksumLength=*/0, counterSink, catalog,
                        *cache, writer, &published);
  writer.Start();

#if defined(__linux__)
  const std::uint64_t rssAnonBefore = ReadRssAnonBytes();
#endif
  std::string error;
  const bool ok = RunLargeEvent(sink, *options.largeEventBytes, error);
  std::string flushError;
  const bool flushed = writer.DrainAndSync();
  flushError = writer.LastError();
  std::cout << "WRITER bytes=" << writer.BytesWritten()
            << " max_lag_ms=" << writer.MaxUnwrittenAgeMilliseconds() << "\n";
  PrintCacheSummary(options.windowSeconds, cache->Counters());
  if (!ok) {
    std::cerr << "storage: " << error << "\n";
    return 4;
  }
  if (!flushed) {
    std::cerr << "storage: failed to flush on shutdown: " << flushError << "\n";
    return 4;
  }
  // File length, not counterSink.bytes: the counter only echoes back the
  // eventLength this generator wrote, so it can never disagree with what
  // was requested even if the body never reached disk.
  std::error_code sizeError;
  const auto fileSize =
      std::filesystem::file_size(dataDir / "binlog.000001", sizeError);
  if (sizeError || fileSize < *options.largeEventBytes) {
    std::cerr << "WITNESS large-event: file size " << (sizeError ? 0 : fileSize)
              << " below requested " << *options.largeEventBytes
              << (sizeError ? (" (" + sizeError.message() + ")") : "") << "\n";
    return 1;
  }
#if defined(__linux__)
  const std::uint64_t rssAnonAfter = ReadRssAnonBytes();
  std::cout << "LARGE_EVENT bytes=" << *options.largeEventBytes
            << " RssAnon_before=" << rssAnonBefore
            << " RssAnon_after=" << rssAnonAfter
            << " RssAnon_growth=" << (rssAnonAfter - rssAnonBefore) << "\n";
#else
  std::cout << "LARGE_EVENT bytes=" << *options.largeEventBytes
            << " macOS_peak_rss_debug_only=" << MacPeakRssBytes() << "\n";
#endif
  return 0;
}

// Internal A/B: the same group count written twice - once at the sink's
// default fsync cadence, once with an extra ::fsync() forced externally.
// Comparing p99 between the two phases is a positive control.
int RunFsyncControl(const BenchOptions &options) {
  const AutoCleanDataDir dataDirGuard(MakeDataDir(options.dataDir),
                                      options.dataDir.empty());
  const auto &dataDir = dataDirGuard.Path();
  std::cout << "data_dir=" << dataDir.string() << "\n";

  CacheCounters phaseCounters;
  const auto runPhase = [&](const std::filesystem::path &subDir,
                            bool forceFsync, Percentiles &out) -> bool {
    std::filesystem::create_directories(subDir);
    EventCounterSink counterSink;
    StorageCatalog catalog;
    PublishedPositionTracker published;
    std::atomic<bool> stopRequested{false};
    std::optional<EventCache> cache;
    if (options.cacheSize) {
      std::string cacheError;
      cache = EventCache::Reserve(options.cacheSize,
                                  std::chrono::seconds{options.windowSeconds},
                                  stopRequested, cacheError);
      if (!cache) {
        std::cerr << "cache: " << cacheError << "\n";
        return false;
      }
    }
    StorageWriter writer(subDir, *cache, catalog);
    StorageEventSink sink(subDir, /*checksumLength=*/0, counterSink, catalog,
                          *cache, writer, &published);
    writer.Start();
    IngestState state;
    std::string rotateError;
    if (!RotateIfNeeded(sink, state, nullptr, rotateError)) {
      std::cerr << "storage: " << rotateError << "\n";
      return false;
    }

    int extraFd = -1;
    if (forceFsync) {
      if (!writer.DrainAndSync()) return false;
      extraFd = ::open((subDir / state.currentFile).c_str(), O_RDONLY);
      if (extraFd < 0) {
        std::cerr << "fsync control: could not open a duplicate descriptor\n";
        return false;
      }
    }
    std::vector<double> durationsUs;
    durationsUs.reserve(*options.groups);
    for (std::uint64_t i = 0; i < *options.groups; ++i) {
      std::string innerRotateError;
      if (!RotateIfNeeded(sink, state, nullptr, innerRotateError)) {
        std::cerr << "storage: " << innerRotateError << "\n";
        return false;
      }
      const auto group =
          MakeOltpGroup(state.currentFileOffset, state.nextGno++);
      const auto start = Clock::now();
      const bool ok =
          DriveGroup(sink, group, state.currentFile, state.currentFileOffset);
      if (extraFd >= 0) {
        if (!writer.DrainAndSync()) {
          ::close(extraFd);
          return false;
        }
        ::fsync(extraFd);
      }
      durationsUs.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - start)
              .count());
      if (!ok) {
        std::cerr << "storage: fsync-control group refused\n";
        return false;
      }
      const auto length = GroupWireLength(group);
      state.currentFileOffset += length;
    }
    if (extraFd >= 0) ::close(extraFd);
    std::string flushError;
    if (!writer.DrainAndSync()) {
      flushError = writer.LastError();
      std::cerr << "storage: failed to flush on shutdown: " << flushError
                << "\n";
      return false;
    }
    const auto counters = cache->Counters();
    phaseCounters.evictedForWindow += counters.evictedForWindow;
    phaseCounters.evictedForSpace += counters.evictedForSpace;
    out = ComputePercentiles(durationsUs);
    return true;
  };

  Percentiles baseline, forced;
  if (!runPhase(dataDir / "baseline", false, baseline)) return 4;
  if (!runPhase(dataDir / "forced", true, forced)) return 4;
  PrintCacheSummary(options.windowSeconds, phaseCounters);
  PrintGroupsWitness(*options.groups);

  const double ratio = baseline.p99Us > 0 ? forced.p99Us / baseline.p99Us : 0.0;
  const bool pass = ratio >= 10.0;
  std::cout << "CONTROL fsync-every-group baseline_p99_us=" << baseline.p99Us
            << " forced_p99_us=" << forced.p99Us << " ratio=" << ratio
            << " pass=" << pass << "\n";
  return pass ? 0 : 1;
}

int RunMixed(const BenchOptions &options) {
  const std::uint64_t datasetBytes = *options.datasetBytes;
  const AutoCleanDataDir dataDirGuard(MakeDataDir(options.dataDir),
                                      options.dataDir.empty());
  const auto &dataDir = dataDirGuard.Path();
  std::cout << "data_dir=" << dataDir.string() << "\n";

  std::string spaceError;
  if (!HasEnoughFreeSpace(dataDir, datasetBytes, spaceError)) {
    std::cerr << spaceError << "\n";
    return 3;
  }

  EventCounterSink counterSink;
  StorageCatalog catalog;
  PublishedPositionTracker published;
  std::atomic<bool> stopRequested{false};
  std::optional<EventCache> cache;
  if (options.cacheSize) {
    std::string cacheError;
    cache = EventCache::Reserve(options.cacheSize,
                                std::chrono::seconds{options.windowSeconds},
                                stopRequested, cacheError);
    if (!cache) {
      std::cerr << "cache: " << cacheError << "\n";
      return 4;
    }
  }
  EventCache *cachePointer = &*cache;
  StorageWriter writer(dataDir, *cache, catalog);
  StorageEventSink sink(dataDir, /*checksumLength=*/0, counterSink, catalog,
                        *cache, writer, &published);
  writer.Start();

  IngestState state;
  std::vector<FileBoundary> boundaries;
  std::string prepError;
  std::cout << "preparing dataset (" << datasetBytes << " byte(s)) ...\n";
  if (!WriteDataset(sink, datasetBytes, state, boundaries, prepError)) {
    std::cerr << "storage: " << prepError << "\n";
    return 4;
  }
  const std::string tailFile = state.currentFile;
  const std::uint64_t tailOffset = state.currentFileOffset;

  auto cgroupPath = CgroupMemoryStatPath();
  std::unique_ptr<MemorySampler> sampler;
  if (cgroupPath) {
    sampler = std::make_unique<MemorySampler>(*cgroupPath);
    sampler->Start();
  }

  const double rateBytesPerSecond =
      options.rateMegabytesPerSecond * 1'000'000.0;
  const auto benchStart = Clock::now();
  std::atomic<std::uint64_t> historianAggregateBytes{0};

  // Whole-process read_bytes around every reader thread's lifetime: the
  // upper bound the per-thread counters of all readers must fit under.
  std::uint64_t processIoBeforeReaders = 0, processIoAfterReaders = 0;
  std::string processIoError;
  bool processIoWindowOk =
      ProcessReadBytes(processIoBeforeReaders, processIoError);

  std::vector<std::thread> nearSyncThreads;
  std::vector<ReaderStats> nearSyncStats(options.mixedNearSync);
  const std::array<double, 3> lagBuckets{0.0, options.windowSeconds / 4.0,
                                         options.windowSeconds / 2.0};
  for (std::uint64_t i = 0; i < options.mixedNearSync; ++i) {
    const double lag = lagBuckets[i % 3];
    std::function<bool(std::uint64_t)> pace =
        [benchStart, lag, rateBytesPerSecond](std::uint64_t bytesSoFar) {
          const double elapsed =
              std::chrono::duration<double>(Clock::now() - benchStart).count();
          const double allowedSeconds = elapsed - lag;
          if (allowedSeconds <= 0) return false;
          return static_cast<double>(bytesSoFar) <
                 allowedSeconds * rateBytesPerSecond;
        };
    nearSyncThreads.emplace_back([&dataDir, &catalog, &published, cachePointer,
                                  tailFile, tailOffset, &stopRequested, pace,
                                  &stats = nearSyncStats[i]] {
      StorageReader reader(dataDir, catalog, published, cachePointer);
      RunReaderLoop(reader, tailFile, tailOffset, stopRequested,
                    CaughtUpBehavior::Wait, pace, nullptr, stats);
    });
  }

  std::vector<std::thread> historianThreads;
  std::vector<ReaderStats> historianStats(options.mixedHistorians);
  // --mixed-no-loop: a historian that catches up keeps following the live
  // tail (like a near-sync reader) instead of wrapping back to its start.
  const auto onCaughtUp =
      options.mixedNoLoop ? CaughtUpBehavior::Wait : CaughtUpBehavior::Loop;
  const double historianBytesPerSecond =
      options.historianRateMegabytesPerSecond * 1'000'000.0;
  std::function<bool(std::uint64_t)> historianPace;
  if (historianBytesPerSecond > 0.0) {
    historianPace = [benchStart,
                     historianBytesPerSecond](std::uint64_t bytesSoFar) {
      const double elapsed =
          std::chrono::duration<double>(Clock::now() - benchStart).count();
      return static_cast<double>(bytesSoFar) <
             elapsed * historianBytesPerSecond;
    };
  }
  for (std::uint64_t j = 0; j < options.mixedHistorians; ++j) {
    const auto [file, offset] = LocateFraction(
        boundaries, datasetBytes,
        HISTORIAN_START_FRACTIONS[j % HISTORIAN_START_FRACTIONS.size()]);
    historianThreads.emplace_back([&dataDir, &catalog, &published, cachePointer,
                                   file, offset, &stopRequested, onCaughtUp,
                                   &historianAggregateBytes, historianPace,
                                   &stats = historianStats[j]] {
      StorageReader reader(dataDir, catalog, published, cachePointer);
      RunReaderLoop(reader, file, offset, stopRequested, onCaughtUp,
                    historianPace, &historianAggregateBytes, stats);
    });
  }

  TokenBucket rate(rateBytesPerSecond);
  const LiveIngestResult live =
      EmitLiveGroups(sink, state, *options.groups, &rate);
  std::string flushError;
  const bool flushed = writer.DrainAndSync();
  flushError = writer.LastError();
  std::cout << "WRITER bytes=" << writer.BytesWritten()
            << " max_lag_ms=" << writer.MaxUnwrittenAgeMilliseconds() << "\n";

  // Let historians circle until they collectively read at least the
  // dataset size or a safety cap elapses; too small a run fails the
  // WITNESS check below rather than hanging.
  const auto historianDeadline = Clock::now() + std::chrono::minutes(10);
  while (historianAggregateBytes.load(std::memory_order_relaxed) <
             datasetBytes &&
         Clock::now() < historianDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  stopRequested = true;
  for (auto &t : nearSyncThreads) t.join();
  for (auto &t : historianThreads) t.join();
  if (processIoWindowOk)
    processIoWindowOk = ProcessReadBytes(processIoAfterReaders, processIoError);
  if (!processIoWindowOk)
    std::cerr << "process I/O accounting around the readers failed: "
              << processIoError << "\n";
  if (sampler) sampler->Stop();
  PrintCacheSummary(options.windowSeconds, cache->Counters());

  const double elapsedSeconds =
      std::chrono::duration<double>(Clock::now() - benchStart).count();

  if (!live.ok) {
    std::cerr << "storage: " << live.error << "\n";
    return 4;
  }
  if (!flushed) {
    std::cerr << "storage: failed to flush on shutdown: " << flushError << "\n";
    return 4;
  }
  PrintIngestSummary("INGEST", live.groupsEmitted, live.bytesEmitted,
                     elapsedSeconds, ComputePercentiles(live.groupDurationsUs));

  bool witnessesOk = true;
  if (live.groupsEmitted != *options.groups) {
    std::cerr << "WITNESS groups-completed: FAIL\n";
    witnessesOk = false;
  }
  PrintGroupsWitness(*options.groups);

  // Computed before the near-sync block needs it: the CONTROL check adds
  // this to the near-sync readers' thread-io and compares against process-io.
  std::uint64_t historianThreadIoSum = 0;
  bool anyHistorianIoAccountingBad = false;
  for (const auto &stats : historianStats) {
    historianThreadIoSum += stats.threadIoEnd - stats.threadIoStart;
    if (!stats.ioAccountingOk) anyHistorianIoAccountingBad = true;
  }

  // applicationBytesRead is a liveness check (bytes actually received);
  // officialDiskBytes is the disk-pressure number, which can legitimately
  // be 0 under page-cache hits and must never gate the exit code.
  std::uint64_t nearSyncApplicationTotal = 0;
  std::uint64_t nearSyncOfficialDiskTotal = 0;
  bool anyNearSyncIoAccountingBad = false;
  for (std::uint64_t i = 0; i < options.mixedNearSync; ++i) {
    const auto &stats = nearSyncStats[i];
    const std::uint64_t officialDiskBytes =
        options.metric == BenchOptions::Metric::ThreadIo
            ? stats.threadIoEnd - stats.threadIoStart
            : stats.processIoEnd - stats.processIoStart;
    nearSyncApplicationTotal += stats.bytesRead;
    nearSyncOfficialDiskTotal += officialDiskBytes;
    if (!stats.ioAccountingOk) anyNearSyncIoAccountingBad = true;
    std::cout << "NEAR_SYNC index=" << i << " lag_s=" << lagBuckets[i % 3]
              << " ok=" << stats.ok << " bytes_read=" << stats.bytesRead
              << " from_cache=" << stats.cacheCounters.readFromCache
              << " from_disk=" << stats.cacheCounters.readFromDisk
              << " from_disk_header=" << stats.cacheCounters.readFromDiskHeader
              << " from_disk_body="
              << (stats.cacheCounters.readFromDisk -
                  stats.cacheCounters.readFromDiskHeader)
              << " seam_crossings=" << stats.cacheCounters.seamCrossings
              << " io_accounting_ok=" << stats.ioAccountingOk
              << " thread_io_delta="
              << (stats.threadIoEnd - stats.threadIoStart)
              << " process_io_delta="
              << (stats.processIoEnd - stats.processIoStart)
              << " official_disk_read_bytes=" << officialDiskBytes << "\n";
    if (options.cacheSize) {
      const bool cachePass = stats.cacheCounters.readFromCache > 0 &&
                             stats.cacheCounters.readFromDisk ==
                                 stats.cacheCounters.readFromDiskHeader;
      std::cout << "WITNESS near-sync-cache index=" << i
                << " pass=" << cachePass << "\n";
      if (!cachePass) witnessesOk = false;
    }
    if (!stats.ok) {
      std::cerr << "near-sync reader " << i << " failed: " << stats.error
                << "\n";
      witnessesOk = false;
    }
    if (!stats.ioAccountingOk)
      std::cerr << "near-sync reader " << i
                << " I/O accounting failed: " << stats.ioAccountingError
                << "\n";
  }
  std::cout << "WITNESS near-sync-received-bytes: total="
            << nearSyncApplicationTotal
            << " pass=" << (nearSyncApplicationTotal > 0) << "\n";
  if (nearSyncApplicationTotal == 0) witnessesOk = false;
  // A failed accounting call leaves its field at 0, indistinguishable from
  // a legitimate "everything served from page cache" result - printed as
  // unreliable rather than silently trusted.
  if (anyNearSyncIoAccountingBad) {
    std::cout << "NEAR_SYNC_DISK near-sync-disk-read-bytes: unreliable (I/O "
                 "accounting failed for at least one reader, see stderr)\n";
  } else {
    std::cout << "NEAR_SYNC_DISK near-sync-disk-read-bytes: total="
              << nearSyncOfficialDiskTotal
              << " target_is_zero pass=" << (nearSyncOfficialDiskTotal == 0)
              << " (informational, not a witness)\n";
  }

  // The per-thread counter must really be per thread: summed across all
  // readers it must not exceed what the whole process read over the same
  // span, or the "thread" counter is really process-wide, counted once per
  // reader.
  if (anyNearSyncIoAccountingBad || anyHistorianIoAccountingBad ||
      !processIoWindowOk) {
    std::cout << "CONTROL metric-isolation: pass=0 (I/O accounting failed, see "
                 "stderr)\n";
    witnessesOk = false;
  } else {
    std::uint64_t minProcessIo = UINT64_MAX, maxProcessIo = 0,
                  allThreadIoSum = historianThreadIoSum;
    for (const auto &stats : nearSyncStats) {
      const auto processDelta = stats.processIoEnd - stats.processIoStart;
      minProcessIo = std::min(minProcessIo, processDelta);
      maxProcessIo = std::max(maxProcessIo, processDelta);
      allThreadIoSum += stats.threadIoEnd - stats.threadIoStart;
    }
    if (nearSyncStats.empty()) minProcessIo = 0;
    const auto processIoWindow = processIoAfterReaders - processIoBeforeReaders;
    const bool controlPass =
        minProcessIo > 0 && allThreadIoSum <= processIoWindow;
    std::cout << "CONTROL metric-isolation process_io_min=" << minProcessIo
              << " process_io_max=" << maxProcessIo
              << " all_readers_thread_io_sum=" << allThreadIoSum
              << " process_io_window=" << processIoWindow
              << " pass=" << controlPass << "\n";
    if (!controlPass) witnessesOk = false;
  }

  const auto historianTotal = historianAggregateBytes.load();
  std::cout << "WITNESS historians-read-dataset: total=" << historianTotal
            << " dataset=" << datasetBytes
            << " pass=" << (historianTotal >= datasetBytes) << "\n";
  if (historianTotal < datasetBytes) witnessesOk = false;
  for (std::uint64_t j = 0; j < options.mixedHistorians; ++j) {
    const auto &stats = historianStats[j];
    std::cout << "HISTORIAN index=" << j << " start_fraction="
              << HISTORIAN_START_FRACTIONS[j % HISTORIAN_START_FRACTIONS.size()]
              << " ok=" << stats.ok << " bytes_read=" << stats.bytesRead
              << " from_cache=" << stats.cacheCounters.readFromCache
              << " from_disk=" << stats.cacheCounters.readFromDisk
              << " from_disk_header=" << stats.cacheCounters.readFromDiskHeader
              << " from_disk_body="
              << (stats.cacheCounters.readFromDisk -
                  stats.cacheCounters.readFromDiskHeader)
              << " seam_crossings=" << stats.cacheCounters.seamCrossings
              << " io_accounting_ok=" << stats.ioAccountingOk
              << " thread_io_delta="
              << (stats.threadIoEnd - stats.threadIoStart)
              << " process_io_delta="
              << (stats.processIoEnd - stats.processIoStart) << "\n";
    if (!stats.ok) {
      std::cerr << "historian " << j << " failed: " << stats.error << "\n";
      witnessesOk = false;
    }
    if (!stats.ioAccountingOk)
      std::cerr << "historian " << j
                << " I/O accounting failed: " << stats.ioAccountingError
                << "\n";
  }
  std::cout << "HISTORIAN_DISK thread-io-sum=" << historianThreadIoSum
            << (anyHistorianIoAccountingBad
                    ? " unreliable (I/O accounting failed for at least one "
                      "historian, see stderr)"
                    : "")
            << "\n";

  if (sampler) {
    const auto sample = sampler->Result();
    if (!sample.available) {
      // Reaching this branch means a cgroup v2 hierarchy was found but
      // memory.stat itself is unreadable - an instrument fault, gated
      // the same way a missing key is.
      std::cerr << "MEMORY instrument error: memory.stat could not be read "
                   "after a cgroup v2 hierarchy was found\n";
      std::cout << "WITNESS memory-pressure: pass=0 (instrument unavailable, "
                   "see stderr)\n";
      witnessesOk = false;
    } else if (!sample.keysOk) {
      // A renamed or missing key is an instrument fault, not "nothing
      // reclaimed" - reported loudly and gated the same way a real
      // witness failure is, not folded into a silent 0.
      std::cerr << "MEMORY instrument error: " << sample.error << "\n";
      std::cout
          << "WITNESS memory-pressure: pass=0 (instrument error, see stderr)\n";
      witnessesOk = false;
    } else {
      const auto pgstealDelta = sample.pgstealEnd - sample.pgstealStart;
      const auto refaultDelta =
          sample.workingsetRefaultFileEnd - sample.workingsetRefaultFileStart;
      std::cout << "MEMORY file_max=" << sample.maxFile
                << " anon_max=" << sample.maxAnon
                << " pgsteal_delta=" << pgstealDelta
                << " workingset_refault_file_delta=" << refaultDelta << "\n";
      // Both counters, not either: pgsteal alone can be nonzero without
      // a matching refault, since sampling starts only after the
      // dataset is already written - requiring both is a stronger claim.
      const bool pressureWitnessed = pgstealDelta > 0 && refaultDelta > 0;
      std::cout << "WITNESS memory-pressure: pass=" << pressureWitnessed
                << "\n";
      if (!pressureWitnessed) witnessesOk = false;
    }
  } else {
#if defined(__linux__)
    // Linux, but no cgroup v2 hierarchy found (v1 host, or
    // /proc/self/cgroup unreadable): absent is not "no pressure", it is nothing
    // measured.
    std::cerr << "MEMORY instrument error: no cgroup v2 hierarchy found in "
                 "/proc/self/cgroup\n";
    std::cout << "WITNESS memory-pressure: pass=0 (instrument unavailable, see "
                 "stderr)\n";
    witnessesOk = false;
#else
    std::cout << "MEMORY not measured (non-Linux build)\n";
#endif
  }

  return witnessesOk ? 0 : 1;
}

}  // namespace
}  // namespace binlog_streamer

int main(int argc, char **argv) {
  using namespace binlog_streamer;
  BenchOptions options;
  std::string error;
  if (!ParseArgs(argc, argv, options, error)) {
    std::cerr << "storage_sink_bench: " << error << "\n";
    return 2;
  }
  switch (options.mode) {
    case BenchOptions::Mode::LargeEvent:
      return RunLargeEventMode(options);
    case BenchOptions::Mode::FsyncControl:
      return RunFsyncControl(options);
    case BenchOptions::Mode::Mixed:
      return RunMixed(options);
    case BenchOptions::Mode::IngestOrReaders:
      return RunIngestOrReaders(options);
  }
  return 2;
}
