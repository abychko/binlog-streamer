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
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <variant>
#include <vector>
#include "cache/cEventCache.hpp"
#include "cache/hCacheDefaults.hpp"

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t S = binlog_streamer::CACHE_SEGMENT_SIZE;

double Percentile(std::vector<std::uint64_t> &samples, std::size_t percent) {
  std::sort(samples.begin(), samples.end());
  return static_cast<double>(samples[(samples.size() - 1) * percent / 100]) /
         1000.0;
}
}  // namespace

int main(int argc, char **argv) {
  const std::size_t readers = argc > 1 ? std::stoull(argv[1]) : 6;
  const std::size_t iterations = argc > 2 ? std::stoull(argv[2]) : 3000;
  if (readers == 0 || readers > 64 || iterations == 0) return 2;
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = binlog_streamer::EventCache::Reserve(
      64 * S, std::chrono::seconds(60), stop, error);
  if (!cache) {
    std::cerr << error << '\n';
    return 2;
  }
  cache->BeginFile("data", 0);
  const std::vector<std::uint8_t> bytes(S, 0x5a);
  for (std::size_t i = 0; i < 64; ++i) {
    if (cache->Append(bytes) != binlog_streamer::AppendOutcome::Appended)
      return 2;
    cache->MarkWritten("data", (i + 1) * S);
  }
  std::barrier phase(static_cast<std::ptrdiff_t>(readers + 1));
  std::atomic<std::uint64_t> failures{0};
  std::vector<std::vector<std::uint64_t>> readTimes(
      readers, std::vector<std::uint64_t>(iterations));
  std::vector<std::uint64_t> appendTimes(iterations);
  std::vector<std::thread> threads;
  for (std::size_t r = 0; r < readers; ++r) {
    threads.emplace_back([&, r] {
      std::vector<std::uint8_t> out(S);
      for (std::size_t i = 0; i < iterations; ++i) {
        phase.arrive_and_wait();
        const auto start = Clock::now();
        const auto result = cache->Read("data", (63 + i) * S, out);
        readTimes[r][i] = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                                 start)
                .count());
        const auto *copied =
            std::get_if<binlog_streamer::CacheReadResult::Copied>(
                &result.value);
        if (!copied || copied->n != S || out.front() != 0x5a ||
            out[S / 2] != 0x5a || out.back() != 0x5a)
          failures.fetch_add(1, std::memory_order_relaxed);
        phase.arrive_and_wait();
      }
    });
  }
  const auto started = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    phase.arrive_and_wait();
    const auto start = Clock::now();
    const auto outcome = cache->Append(bytes);
    appendTimes[i] = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             start)
            .count());
    if (outcome != binlog_streamer::AppendOutcome::Appended)
      failures.fetch_add(1, std::memory_order_relaxed);
    cache->MarkWritten("data", (65 + i) * S);
    phase.arrive_and_wait();
  }
  for (auto &thread : threads) thread.join();
  std::vector<std::uint64_t> reads;
  reads.reserve(readers * iterations);
  for (const auto &times : readTimes)
    reads.insert(reads.end(), times.begin(), times.end());
  std::cout << "readers=" << readers << " iterations=" << iterations
            << " bytes_per_operation=" << S
            << " append_p50_us=" << Percentile(appendTimes, 50)
            << " append_p99_us=" << Percentile(appendTimes, 99)
            << " read_p50_us=" << Percentile(reads, 50)
            << " read_p99_us=" << Percentile(reads, 99) << " elapsed_s="
            << std::chrono::duration<double>(Clock::now() - started).count()
            << " appended=" << cache->Counters().appended
            << " failures=" << failures.load() << '\n';
  return failures.load() == 0 ? 0 : 1;
}
