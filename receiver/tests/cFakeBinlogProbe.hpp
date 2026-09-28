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

#include "receiver/iBinlogProbe.hpp"

#include <map>
#include <optional>
#include <string>

namespace binlog_streamer::test {

class FakeBinlogProbe : public BinlogProbe {
 public:
  std::map<std::string, ProbeResult> responsesByRequestedSetText;
  unsigned probeCallCount = 0;

  // nullopt scripts a failure defaulting to TransientFailure;
  // previousGtidsFailureByFileName overrides that for a specific outcome.
  std::map<std::string, std::optional<std::string>> previousGtidsTextByFileName;
  unsigned previousGtidsTextCallCount = 0;

  // Checked before previousGtidsTextByFileName's nullopt shorthand, so a
  // test can script a non-default SessionOutcome.
  std::map<std::string, SessionResult> previousGtidsFailureByFileName;

  ProbeResult Probe(const GtidSet &startSet) override {
    ++probeCallCount;
    const auto it = responsesByRequestedSetText.find(startSet.ToText());
    if (it == responsesByRequestedSetText.end()) {
      ProbeResult result;
      result.failure.message = "FakeBinlogProbe: no scripted response for '" +
                               startSet.ToText() + "'";
      return result;
    }
    return it->second;
  }

  PreviousGtidsResult PreviousGtidsText(std::string_view fileName) override {
    ++previousGtidsTextCallCount;
    const std::string key(fileName);
    PreviousGtidsResult result;
    const auto failureIt = previousGtidsFailureByFileName.find(key);
    if (failureIt != previousGtidsFailureByFileName.end()) {
      result.failure = failureIt->second;
      return result;
    }
    const auto it = previousGtidsTextByFileName.find(key);
    if (it == previousGtidsTextByFileName.end()) {
      result.failure.message =
          "FakeBinlogProbe: no scripted Previous_gtids response for '" + key +
          "'";
      return result;
    }
    if (!it->second.has_value()) {
      result.failure.message =
          "FakeBinlogProbe: scripted failure for '" + key + "'";
      return result;
    }
    result.ok = true;
    result.text = *it->second;
    return result;
  }
};

}  // namespace binlog_streamer::test
