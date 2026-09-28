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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "cTempDirectoryFixture.hpp"
#include "cache/cEventCache.hpp"
#include "cache/hCacheDefaults.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageWriter.hpp"
int main(int argc, char **argv) {
  using namespace binlog_streamer;
  constexpr auto S = CACHE_SEGMENT_SIZE;
  const std::size_t chunk = argc > 1 ? std::stoull(argv[1]) : S;
  const std::uint64_t total = argc > 2 ? std::stoull(argv[2]) : 2ULL * 1024 * S;
  if (chunk == 0 || chunk > S || total == 0) return 2;
  test::TempDirectoryFixture directory;
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = EventCache::Reserve(64 * S, stop, error);
  if (!cache) {
    std::cerr << error << '\n';
    return 2;
  }
  StorageCatalog catalog;
  StoredFileRecord record;
  record.name = "binlog.000001";
  record.onDisk = false;
  record.headerLength = 32;
  record.size = 32;
  catalog.Add(record);
  StorageWriter writer(directory.Directory(), *cache, catalog);
  const std::array<std::uint8_t, 24> fde{};
  const std::array<std::uint8_t, 4> pge{};
  cache->BeginFile(record.name, 32);
  writer.PostCreate(record.name, fde, pge);
  writer.Start();
  if (!writer.DrainAndSync()) return 2;
  // Match the small-group workload: GTID, Query, Xid, one publication.
  const std::size_t eventsPerChunk = chunk == 744 ? 3 : 1;
  const std::vector<std::uint8_t> bytes(chunk, 0x51);
  bool success = true;
  const auto start = std::chrono::steady_clock::now();
  std::thread producer([&] {
    for (std::uint64_t sent = 0; sent < total;) {
      const auto take = static_cast<std::size_t>(
          std::min<std::uint64_t>(chunk, total - sent));
      if (cache->AppendOrWait(std::span(bytes).first(take)) !=
          AppendOutcome::Appended) {
        success = false;
        break;
      }
      for (std::size_t i = 0; i < eventsPerChunk; ++i)
        writer.NoteEventCompleted();
      writer.Wake();
      sent += take;
    }
  });
  producer.join();
  success = writer.DrainAndSync() && success;
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  writer.Stop();
  const auto counters = cache->Counters();
  std::cout << "chunk=" << chunk << " events_per_chunk=" << eventsPerChunk
            << " bytes=" << writer.BytesWritten() << " seconds=" << elapsed
            << " MB_per_second="
            << static_cast<double>(total) / elapsed / 1000000.0
            << " write_calls=" << writer.WriteCalls() << " writes_per_MiB="
            << static_cast<double>(writer.WriteCalls()) * S /
                   static_cast<double>(total)
            << " max_unwritten_bytes=" << counters.maxUnwritten
            << " max_unwritten_age_ms=" << writer.MaxUnwrittenAgeMilliseconds()
            << " space_waits=" << counters.spaceWaits
            << " wait_ns=" << counters.spaceWaitNanoseconds
            << " syncs=" << writer.SyncsPerformed() << " success=" << success
            << '\n';
  return success && writer.BytesWritten() == total ? 0 : 1;
}
