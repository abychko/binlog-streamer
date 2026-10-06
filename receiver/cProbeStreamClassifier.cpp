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

#include "receiver/cProbeStreamClassifier.hpp"

namespace binlog_streamer {
namespace {

SessionResult MakeFailure(std::string message) {
  SessionResult failure;
  failure.outcome = SessionOutcome::PermanentFailure;
  failure.message = std::move(message);
  return failure;
}

SessionResult MakeTransient(std::string message) {
  SessionResult failure;
  failure.outcome = SessionOutcome::TransientFailure;
  failure.message = std::move(message);
  return failure;
}

SessionResult MakeStopped(std::string message) {
  SessionResult failure;
  failure.outcome = SessionOutcome::Stopped;
  failure.message = std::move(message);
  return failure;
}

}  // namespace

SessionResult ProbeStreamClassifier::Classify(
    const StreamResult &streamResult) {
  // Network-level failures are retried like a connect failure; protocol-level
  // ones (dump rejected, unrecognized reply) are not.
  switch (streamResult.reason) {
    case StreamEndReason::ConnectionClosed:
    case StreamEndReason::Timeout:
      return MakeTransient("probe's event stream ended: " +
                           streamResult.message);
    case StreamEndReason::Stopped:
      return MakeStopped(streamResult.message);
    case StreamEndReason::SourceError:
      return MakeFailure("source rejected dump: " + streamResult.errorText);
    case StreamEndReason::MalformedStream:
    case StreamEndReason::HeartbeatFailure:
    case StreamEndReason::EndOfStream:
    case StreamEndReason::StoppedBySink:
      return MakeFailure(
          "probe did not observe ROTATE + FORMAT_DESCRIPTION_EVENT: " +
          streamResult.message);
  }
  return MakeFailure("probe: unrecognized stream end reason");
}

}  // namespace binlog_streamer
