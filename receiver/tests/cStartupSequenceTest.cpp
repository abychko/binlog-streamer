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

#include "receiver/cStartupSequence.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include "cFakeBinlogProbe.hpp"
#include "cFakeTransport.hpp"
#include "cScriptedSourceBuilder.hpp"
#include "config/sServerSettings.hpp"
#include "config/sSourceSettings.hpp"

namespace binlog_streamer {
namespace {

const std::string EXECUTED_TEXT = "11111111-1111-1111-1111-111111111111:1-100";

SourceSettings MakeSource() {
  SourceSettings source;
  source.host = "127.0.0.1";
  source.port = 3306;
  source.user = "repl";
  source.password = "secret";
  return source;
}

test::ScriptedSourceBuilder OneAttemptScript() {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@@GLOBAL.gtid_executed", EXECUTED_TEXT);
  script.AppendSingleColumnRow("@@GLOBAL.gtid_purged", "");
  return script;
}

void ScriptCurrentFile(test::FakeBinlogProbe &probe) {
  GtidSet executedSet;
  std::string parseError;
  ASSERT_TRUE(executedSet.AddFromText(EXECUTED_TEXT, parseError));
  ProbeResult currentFile;
  currentFile.ok = true;
  currentFile.fileName = "binlog.000050";
  probe.responsesByRequestedSetText[executedSet.ToText()] = currentFile;
  probe.previousGtidsTextByFileName["binlog.000050"] = EXECUTED_TEXT;
}

class FlakyProbe : public BinlogProbe {
 public:
  FlakyProbe(BinlogProbe &delegate, unsigned failCount)
      : m_delegate(delegate), m_failCount(failCount) {}

  ProbeResult Probe(const GtidSet &startSet) override {
    ++m_totalCalls;
    if (m_failedCalls < m_failCount) {
      ++m_failedCalls;
      ProbeResult result;
      result.failure.outcome = SessionOutcome::TransientFailure;
      result.failure.message = "simulated network error probing the source";
      return result;
    }
    return m_delegate.Probe(startSet);
  }

  PreviousGtidsResult PreviousGtidsText(std::string_view fileName) override {
    return m_delegate.PreviousGtidsText(fileName);
  }

  unsigned calls() const { return m_totalCalls; }

 private:
  BinlogProbe &m_delegate;
  unsigned m_failCount;
  unsigned m_failedCalls = 0;
  unsigned m_totalCalls = 0;
};

const std::string STORED_TEXT = "11111111-1111-1111-1111-111111111111:1-100";
const std::string PURGED_TEXT = "22222222-2222-2222-2222-222222222222:1-5";

test::ScriptedSourceBuilder ResumeAttemptScript() {
  test::ScriptedSourceBuilder script =
      test::ScriptedSourceBuilder::ThroughReplicaUuid("999");
  script.AppendCommandOk();
  script.AppendSingleColumnRow("@@GLOBAL.gtid_purged", PURGED_TEXT);
  return script;
}

StartupOutcome RunResume(test::FakeTransport &transport, BinlogProbe &probe,
                         unsigned attempts) {
  GtidSet stored;
  std::string parseError;
  EXPECT_TRUE(stored.AddFromText(STORED_TEXT, parseError));
  ServerSettings server;
  server.serverId = 42;
  RetryOptions options;
  options.attempts = attempts;
  options.sleep = [](std::chrono::seconds) {};
  StartupSequence sequence(
      transport, MakeSource(), server, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
      "name", "ver", probe, options, {}, nullptr, stored, "binlog.000050");
  return sequence.Run();
}

TEST(StartupSequenceTest, ResumeAddsGtidPurgedWhileTheSourceHasOurLastFile) {
  test::FakeTransport transport;
  transport.incomingByConnection = {ResumeAttemptScript().Bytes()};
  test::FakeBinlogProbe probe;
  probe.previousGtidsTextByFileName["binlog.000050"] = STORED_TEXT;

  const StartupOutcome outcome = RunResume(transport, probe, 1);

  ASSERT_EQ(outcome.result.outcome, SessionOutcome::Registered)
      << outcome.result.message;
  EXPECT_TRUE(outcome.resolution.usedStoredHistory);
  EXPECT_EQ(outcome.resolution.selectedFileName, "binlog.000050");
  EXPECT_EQ(outcome.resolution.startSet.ToText(),
            STORED_TEXT + ",\n" + PURGED_TEXT);
}

TEST(StartupSequenceTest, ResumeSendsTheStoredSetAloneWhenOurLastFileIsGone) {
  test::FakeTransport transport;
  transport.incomingByConnection = {ResumeAttemptScript().Bytes()};
  test::FakeBinlogProbe probe;
  SessionResult missing;
  missing.outcome = SessionOutcome::PermanentFailure;
  missing.message = "Could not find target log";
  probe.previousGtidsFailureByFileName["binlog.000050"] = missing;

  const StartupOutcome outcome = RunResume(transport, probe, 1);

  ASSERT_EQ(outcome.result.outcome, SessionOutcome::Registered)
      << outcome.result.message;
  EXPECT_EQ(outcome.resolution.startSet.ToText(), STORED_TEXT);
}

TEST(StartupSequenceTest,
     ResumeRetriesWhenCheckingOurLastFileFailsTransiently) {
  test::FakeTransport transport;
  const auto script = ResumeAttemptScript().Bytes();
  transport.incomingByConnection = {script, script};
  test::FakeBinlogProbe probe;
  const StartupOutcome outcome = RunResume(transport, probe, 2);

  EXPECT_EQ(outcome.result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_EQ(transport.connectCallCount, 2u);
  EXPECT_EQ(probe.previousGtidsTextCallCount, 2u);
}

TEST(StartupSequenceTest, RetriesAfterATransientProbeFailureThenSucceeds) {
  test::FakeTransport transport;
  const auto script = OneAttemptScript().Bytes();
  transport.incomingByConnection = {script, script};

  test::FakeBinlogProbe delegateProbe;
  ScriptCurrentFile(delegateProbe);
  FlakyProbe probe(delegateProbe, /*failCount=*/1);

  ServerSettings server;
  server.serverId = 42;
  RetryOptions options;
  options.attempts = 3;
  options.sleep = [](std::chrono::seconds) {};
  unsigned retryCallbacks = 0;
  options.onRetry = [&](unsigned, unsigned, const std::string &) {
    ++retryCallbacks;
  };

  StartupSequence sequence(transport, MakeSource(), server,
                           "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                           "ver", probe, options);
  const StartupOutcome outcome = sequence.Run();

  EXPECT_EQ(outcome.result.outcome, SessionOutcome::Registered);
  ASSERT_NE(outcome.session, nullptr);
  EXPECT_EQ(outcome.resolution.selectedFileName, "binlog.000050");
  EXPECT_EQ(transport.connectCallCount, 2u);
  EXPECT_EQ(retryCallbacks, 1u);
  EXPECT_EQ(probe.calls(), 2u);
}

TEST(StartupSequenceTest,
     ExhaustingAttemptsOnStartSetResolutionReportsPermanentFailure) {
  test::FakeTransport transport;
  const auto script = OneAttemptScript().Bytes();
  transport.incomingByConnection = {script, script, script};

  test::FakeBinlogProbe delegateProbe;
  ScriptCurrentFile(delegateProbe);
  FlakyProbe probe(delegateProbe, /*failCount=*/3);

  ServerSettings server;
  server.serverId = 42;
  RetryOptions options;
  options.attempts = 3;
  options.sleep = [](std::chrono::seconds) {};
  unsigned retryCallbacks = 0;
  options.onRetry = [&](unsigned, unsigned, const std::string &) {
    ++retryCallbacks;
  };

  StartupSequence sequence(transport, MakeSource(), server,
                           "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                           "ver", probe, options);
  const StartupOutcome outcome = sequence.Run();

  EXPECT_EQ(outcome.result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(outcome.result.message.find("exhausted"), std::string::npos)
      << outcome.result.message;
  EXPECT_EQ(outcome.session, nullptr);
  EXPECT_EQ(transport.connectCallCount, 3u);
  EXPECT_EQ(retryCallbacks, 3u);
}

TEST(StartupSequenceTest,
     ExhaustsAttemptsAndReportsPermanentFailureOnAConnectFailure) {
  test::FakeTransport transport;
  transport.connectAlwaysFails = true;

  test::FakeBinlogProbe probe;

  ServerSettings server;
  server.serverId = 42;
  RetryOptions options;
  options.attempts = 3;
  options.sleep = [](std::chrono::seconds) {};
  unsigned retryCallbacks = 0;
  options.onRetry = [&](unsigned, unsigned, const std::string &) {
    ++retryCallbacks;
  };

  StartupSequence sequence(transport, MakeSource(), server, "uuid", "name",
                           "ver", probe, options);
  const StartupOutcome outcome = sequence.Run();

  EXPECT_EQ(outcome.result.outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(outcome.result.message.find("exhausted"), std::string::npos)
      << outcome.result.message;
  EXPECT_EQ(transport.connectCallCount, 3u);
  EXPECT_EQ(retryCallbacks, 3u);
  EXPECT_EQ(outcome.session, nullptr);
}

TEST(StartupSequenceTest, RegistersAndResolvesAfterATransientConnectFailure) {
  const auto script = OneAttemptScript().Bytes();
  test::FakeTransport transport;
  transport.connectFailuresRemaining = 1;
  transport.incomingByConnection = {script};

  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe);

  ServerSettings server;
  server.serverId = 42;
  RetryOptions options;
  options.attempts = 3;
  options.sleep = [](std::chrono::seconds) {};
  unsigned retryCallbacks = 0;
  options.onRetry = [&](unsigned, unsigned, const std::string &) {
    ++retryCallbacks;
  };

  StartupSequence sequence(transport, MakeSource(), server,
                           "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name",
                           "ver", probe, options);
  const StartupOutcome outcome = sequence.Run();

  EXPECT_EQ(outcome.result.outcome, SessionOutcome::Registered);
  EXPECT_EQ(transport.connectCallCount, 2u);
  EXPECT_EQ(retryCallbacks, 1u);
  ASSERT_NE(outcome.session, nullptr);
  EXPECT_EQ(outcome.resolution.selectedFileName, "binlog.000050");
}

TEST(StartupSequenceTest,
     StopRequestedBetweenAttemptsEndsTheLoopWithoutAFurtherConnectAttempt) {
  test::FakeTransport transport;
  transport.connectAlwaysFails = true;

  test::FakeBinlogProbe probe;

  ServerSettings server;
  server.serverId = 42;
  RetryOptions options;
  options.attempts = 5;
  options.sleep = [](std::chrono::seconds) {};
  unsigned retryCallbacks = 0;
  std::atomic<bool> stopRequested{false};
  options.onRetry = [&](unsigned, unsigned, const std::string &) {
    ++retryCallbacks;
    stopRequested = true;
  };

  StartupSequence sequence(transport, MakeSource(), server, "uuid", "name",
                           "ver", probe, options, ReplicaSessionOptions{},
                           &stopRequested);
  const StartupOutcome outcome = sequence.Run();

  EXPECT_EQ(outcome.result.outcome, SessionOutcome::Stopped);
  EXPECT_EQ(outcome.session, nullptr);
  EXPECT_EQ(retryCallbacks, 1u);
  EXPECT_EQ(transport.connectCallCount, 1u);
}

}  // namespace
}  // namespace binlog_streamer
