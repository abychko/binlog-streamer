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

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

TEST(ProbeStreamClassifierTest, ConnectionClosedIsTransient) {
  StreamResult result;
  result.reason = StreamEndReason::ConnectionClosed;
  result.message = "connection closed by source";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(classified.message.find(result.message), std::string::npos)
      << classified.message;
}

TEST(ProbeStreamClassifierTest, TimeoutIsTransient) {
  StreamResult result;
  result.reason = StreamEndReason::Timeout;
  result.message = "timed out waiting for a response from the source";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::TransientFailure);
  EXPECT_NE(classified.message.find(result.message), std::string::npos)
      << classified.message;
}

TEST(ProbeStreamClassifierTest, StoppedIsStopped) {
  StreamResult result;
  result.reason = StreamEndReason::Stopped;
  result.message = "read interrupted";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::Stopped);
  EXPECT_EQ(classified.message, result.message);
}

TEST(ProbeStreamClassifierTest,
     SourceErrorIsPermanentWithTheSourcesOwnMessage) {
  StreamResult result;
  result.reason = StreamEndReason::SourceError;
  result.errorCode = 1236;
  result.errorText =
      "could not find first log file name in binary log index file";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(classified.message.find(result.errorText), std::string::npos)
      << classified.message;
}

TEST(ProbeStreamClassifierTest, MalformedStreamIsPermanent) {
  StreamResult result;
  result.reason = StreamEndReason::MalformedStream;
  result.message = "event length larger than MAX_EVENT_LENGTH";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::PermanentFailure);
}

TEST(ProbeStreamClassifierTest, HeartbeatFailureIsPermanent) {
  StreamResult result;
  result.reason = StreamEndReason::HeartbeatFailure;
  result.message = "heartbeat named a position behind the current one";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::PermanentFailure);
}

TEST(ProbeStreamClassifierTest, EndOfStreamIsPermanent) {
  StreamResult result;
  result.reason = StreamEndReason::EndOfStream;
  result.message = "source ended the dump stream";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::PermanentFailure);
}

TEST(ProbeStreamClassifierTest,
     StoppedBySinkWithoutHavingSeenTheFormatDescriptionIsPermanent) {
  // The only StoppedBySink case DumpProbe passes here: the sink stopped before
  // FORMAT_DESCRIPTION_EVENT.
  StreamResult result;
  result.reason = StreamEndReason::StoppedBySink;
  result.message = "EventSink::OnEventBegin returned false";

  const SessionResult classified = ProbeStreamClassifier::Classify(result);

  EXPECT_EQ(classified.outcome, SessionOutcome::PermanentFailure);
}

}  // namespace
}  // namespace binlog_streamer
