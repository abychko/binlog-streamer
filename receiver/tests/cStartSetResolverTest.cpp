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

#include "receiver/cStartSetResolver.hpp"

#include <gtest/gtest.h>
#include "cFakeBinlogProbe.hpp"

namespace binlog_streamer {
namespace {

const std::string EXECUTED_TEXT = "11111111-1111-1111-1111-111111111111:1-100";
const std::string CURRENT_FILE_PREVIOUS_GTIDS_TEXT =
    "11111111-1111-1111-1111-111111111111:1-90";

void ScriptCurrentFile(test::FakeBinlogProbe &probe,
                       const std::string &executedText) {
  GtidSet executedSet;
  std::string parseError;
  ASSERT_TRUE(executedSet.AddFromText(executedText, parseError));
  ProbeResult currentFile;
  currentFile.ok = true;
  currentFile.fileName = "binlog.000050";
  probe.responsesByRequestedSetText[executedSet.ToText()] = currentFile;
}

TEST(StartSetResolverTest, UsesTheCurrentFilesOwnPreviousGtids) {
  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe, EXECUTED_TEXT);
  probe.previousGtidsTextByFileName["binlog.000050"] =
      CURRENT_FILE_PREVIOUS_GTIDS_TEXT;

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve(EXECUTED_TEXT, "", resolution);
  ASSERT_FALSE(failure.has_value()) << (failure ? failure->message : "");

  EXPECT_FALSE(resolution.usedStoredHistory);
  EXPECT_EQ(resolution.selectedFileName, "binlog.000050");
  EXPECT_EQ(resolution.startSet.ToText(), CURRENT_FILE_PREVIOUS_GTIDS_TEXT);
  EXPECT_EQ(probe.probeCallCount, 1u);
  EXPECT_EQ(probe.previousGtidsTextCallCount, 1u);
}

TEST(StartSetResolverTest, AddsGtidPurgedMissingFromPreviousGtids) {
  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe, EXECUTED_TEXT);
  probe.previousGtidsTextByFileName["binlog.000050"] =
      CURRENT_FILE_PREVIOUS_GTIDS_TEXT;

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve(
      EXECUTED_TEXT, "22222222-2222-2222-2222-222222222222:1-5", resolution);
  ASSERT_FALSE(failure.has_value()) << (failure ? failure->message : "");

  EXPECT_EQ(resolution.startSet.ToText(),
            "11111111-1111-1111-1111-111111111111:1-90,\n"
            "22222222-2222-2222-2222-222222222222:1-5");
}

TEST(StartSetResolverTest, MalformedGtidPurgedIsAPermanentFailure) {
  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe, EXECUTED_TEXT);
  probe.previousGtidsTextByFileName["binlog.000050"] =
      CURRENT_FILE_PREVIOUS_GTIDS_TEXT;

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure =
      resolver.Resolve(EXECUTED_TEXT, "not a gtid set", resolution);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(failure->message.find("gtid_purged"), std::string::npos)
      << failure->message;
}

TEST(StartSetResolverTest,
     AnEmptyGtidExecutedProbesTheEmptySetAndStartsFromAnEmptyPreviousGtids) {
  test::FakeBinlogProbe probe;
  ProbeResult firstFile;
  firstFile.ok = true;
  firstFile.fileName = "binlog.000001";
  probe.responsesByRequestedSetText[GtidSet{}.ToText()] = firstFile;
  probe.previousGtidsTextByFileName["binlog.000001"] = "";

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve("", "", resolution);
  ASSERT_FALSE(failure.has_value()) << (failure ? failure->message : "");

  EXPECT_EQ(resolution.selectedFileName, "binlog.000001");
  EXPECT_TRUE(resolution.startSet.ToText().empty());
}

TEST(StartSetResolverTest, ReturnsFailureWhenProbingGtidExecutedFails) {
  test::FakeBinlogProbe probe;
  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve(EXECUTED_TEXT, "", resolution);
  ASSERT_TRUE(failure.has_value());
  EXPECT_FALSE(failure->message.empty());
  EXPECT_EQ(failure->outcome, SessionOutcome::TransientFailure);
}

TEST(StartSetResolverTest, MalformedGtidExecutedIsAPermanentFailure) {
  test::FakeBinlogProbe probe;
  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve("not a gtid set", "", resolution);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(failure->message.find("gtid_executed"), std::string::npos)
      << failure->message;
  EXPECT_EQ(probe.probeCallCount, 0u);
}

TEST(StartSetResolverTest,
     ReturnsFailureWhenReadingTheCurrentFilesPreviousGtidsFails) {
  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe, EXECUTED_TEXT);

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve(EXECUTED_TEXT, "", resolution);
  ASSERT_TRUE(failure.has_value());
  EXPECT_FALSE(failure->message.empty());
}

TEST(StartSetResolverTest,
     PreviousGtidsTextFailureCarriesThePermanentClassificationTheProbeGaveIt) {
  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe, EXECUTED_TEXT);
  SessionResult permanentFailure;
  permanentFailure.outcome = SessionOutcome::PermanentFailure;
  permanentFailure.message =
      "file 'binlog.000050' has no second event (no Previous_gtids row)";
  probe.previousGtidsFailureByFileName["binlog.000050"] = permanentFailure;

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve(EXECUTED_TEXT, "", resolution);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->outcome, SessionOutcome::PermanentFailure);
  EXPECT_EQ(failure->message, permanentFailure.message);
}

TEST(StartSetResolverTest, MalformedPreviousGtidsIsAPermanentFailure) {
  test::FakeBinlogProbe probe;
  ScriptCurrentFile(probe, EXECUTED_TEXT);
  probe.previousGtidsTextByFileName["binlog.000050"] = "garbage";

  StartSetResolver resolver(probe);
  StartSetResolution resolution;
  const auto failure = resolver.Resolve(EXECUTED_TEXT, "", resolution);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->outcome, SessionOutcome::PermanentFailure);
  EXPECT_NE(failure->message.find("binlog.000050"), std::string::npos)
      << failure->message;
}

}  // namespace
}  // namespace binlog_streamer
