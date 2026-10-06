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

#include "net/iTransport.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer::test {

class FakeTransport : public Transport {
 public:
  std::vector<std::uint8_t> incoming;
  std::size_t readPosition = 0;
  std::vector<std::vector<std::uint8_t>> writes;
  unsigned connectFailuresRemaining = 0;
  bool connectAlwaysFails = false;
  unsigned connectCallCount = 0;

  std::deque<ReadOutcome> scriptedOutcomes;
  unsigned readCallCount = 0;

  std::vector<std::chrono::milliseconds> readTimeouts;

  std::size_t maxBytesPerRead = 0;

  std::vector<std::vector<std::uint8_t>> incomingByConnection;
  unsigned successfulConnectCount = 0;

  bool Connect(const std::string &, std::uint16_t, std::chrono::milliseconds,
               std::string &error) override {
    ++connectCallCount;
    if (connectAlwaysFails) {
      error = "connection refused";
      return false;
    }
    if (connectFailuresRemaining > 0) {
      --connectFailuresRemaining;
      error = "connection refused";
      return false;
    }
    if (!incomingByConnection.empty()) {
      const std::size_t index = std::min<std::size_t>(
          successfulConnectCount, incomingByConnection.size() - 1);
      incoming = incomingByConnection[index];
      readPosition = 0;
    }
    ++successfulConnectCount;
    error.clear();
    return true;
  }

  ReadOutcome Read(std::span<std::uint8_t> buffer, std::size_t &bytesRead,
                   std::chrono::milliseconds timeout,
                   std::string &error) override {
    ++readCallCount;
    readTimeouts.push_back(timeout);
    bytesRead = 0;
    if (!scriptedOutcomes.empty()) {
      const ReadOutcome outcome = scriptedOutcomes.front();
      scriptedOutcomes.pop_front();
      if (outcome != ReadOutcome::Data) return outcome;
    }
    if (readPosition >= incoming.size()) {
      error = "fake source script exhausted";
      return ReadOutcome::Failed;
    }
    std::size_t count = std::min(buffer.size(), incoming.size() - readPosition);
    if (maxBytesPerRead > 0) count = std::min(count, maxBytesPerRead);
    std::copy(
        incoming.begin() + static_cast<std::ptrdiff_t>(readPosition),
        incoming.begin() + static_cast<std::ptrdiff_t>(readPosition + count),
        buffer.begin());
    readPosition += count;
    bytesRead = count;
    return ReadOutcome::Data;
  }

  bool Write(std::span<const std::uint8_t> data, std::chrono::milliseconds,
             std::string &) override {
    writes.emplace_back(data.begin(), data.end());
    return true;
  }

  void Close() override {}
};

}  // namespace binlog_streamer::test
