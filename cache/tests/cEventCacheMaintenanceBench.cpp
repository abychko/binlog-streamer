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

#include "cache/cEventCache.hpp"
#include "cache/hCacheDefaults.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr auto S = binlog_streamer::CACHE_SEGMENT_SIZE;

double Percentile(std::vector<double> &samples, std::size_t percent) {
  std::sort(samples.begin(), samples.end());
  return samples[(samples.size() - 1) * percent / 100];
}
}  // namespace

int main(int argc, char **argv) {
  const bool pageReturn = argc > 1 && std::string(argv[1]) == "return";
  const std::size_t samples = argc > 2 ? std::stoull(argv[2]) : 20000;
  if (samples == 0) return 2;
  std::atomic<bool> stop{false};
  std::string error;
  auto cache = binlog_streamer::EventCache::Reserve(2 * S, 60s, stop, error);
  if (!cache) {
    std::cerr << error << '\n';
    return 2;
  }
  std::vector<double> times(samples);
  if (!pageReturn) {
    cache->BeginFile("data", 0);
    const std::array<std::uint8_t, 1> bytes{42};
    if (cache->Append(bytes) != binlog_streamer::AppendOutcome::Appended)
      return 2;
    constexpr std::size_t batch = 100;
    for (std::size_t i = 0; i < samples; ++i) {
      const auto start = Clock::now();
      for (std::size_t j = 0; j < batch; ++j) cache->MarkWritten("data", 1);
      times[i] = std::chrono::duration<double, std::nano>(Clock::now() - start)
                     .count() /
                 batch;
    }
    std::cout << "mode=mark_written_no_eviction operations=" << samples * batch;
  } else {
    const std::vector<std::uint8_t> bytes(S, 0x57);
    for (std::size_t i = 0; i < samples; ++i) {
      const auto name = std::to_string(i);
      cache->BeginFile(name, 0);
      if (cache->Append(bytes) != binlog_streamer::AppendOutcome::Appended)
        return 2;
      cache->MarkWritten(name, S);
      cache->EndFile();
      const auto now = Clock::now() + 61s;
      const auto start = Clock::now();
      cache->EvictExpired(now, 60s);
      times[i] = std::chrono::duration<double, std::nano>(Clock::now() - start)
                     .count();
      if (cache->Counters().occupied != 0) return 1;
    }
    std::cout << "mode=return_one_segment operations=" << samples;
  }
  std::cout << " p50_ns=" << Percentile(times, 50)
            << " p99_ns=" << Percentile(times, 99) << '\n';
  return 0;
}
