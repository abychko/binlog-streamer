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

#include "cQueryResponder.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace binlog_streamer {
namespace {

class FixedState : public ServerState {
 public:
  std::string GtidPurged() const override {
    return "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5";
  }
  std::string GtidExecuted() const override {
    return "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9";
  }
  std::string SourceVersion() const override { return "8.4.11"; }
  std::string BinlogChecksum() const override { return "CRC32"; }
  std::optional<PreviousGtidsEvent> PreviousGtids(
      const std::string &fileName) const override {
    if (fileName != "binlog.000007") return std::nullopt;
    return PreviousGtidsEvent{127, 198, 1,
                              "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5"};
  }
};

class QueryResponderTest : public ::testing::Test {
 protected:
  FixedState state;
  QueryResponder responder{
      ServerIdentity{1001, "8a94f357-aab4-11df-86ab-c80aa9429562", "relay 1.0",
                     "relay", "1.0"},
      &state};
  SessionVariables session;

  void ExpectRow(const std::string &sql, const std::string &column,
                 const std::optional<std::string> &value) {
    const QueryResponse response = responder.Respond(sql, session);
    ASSERT_EQ(response.kind, QueryResponseKind::Row)
        << sql << ": " << response.message;
    ASSERT_EQ(response.columns.size(), 1u);
    EXPECT_EQ(response.columns[0].name, column);
    EXPECT_EQ(response.columns[0].value, value);
  }
};

TEST_F(QueryResponderTest,
       AnswersTheSystemVariablesAReplicaReadsBeforeItsDump) {
  ExpectRow("SELECT @@GLOBAL.SERVER_ID", "@@GLOBAL.SERVER_ID", "1001");
  ExpectRow("SELECT @@GLOBAL.SERVER_UUID", "@@GLOBAL.SERVER_UUID",
            "8a94f357-aab4-11df-86ab-c80aa9429562");
  ExpectRow("SELECT @@GLOBAL.GTID_MODE", "@@GLOBAL.GTID_MODE", "ON");
}

TEST_F(QueryResponderTest,
       MatchesWithoutRegardToCaseSpacingOrATrailingSemicolon) {
  ExpectRow("  select   @@global.server_id ; ", "@@global.server_id", "1001");
  ExpectRow("SELECT @@server_id", "@@server_id", "1001");
}

TEST_F(QueryResponderTest, ReportsTheHistoryTheRelayNoLongerHolds) {
  ExpectRow("SELECT @@GLOBAL.gtid_purged", "@@GLOBAL.gtid_purged",
            "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5");
}

TEST_F(QueryResponderTest, ReportsEverythingTheRelayHasStoredOrOnceHad) {
  ExpectRow("SELECT @@GLOBAL.gtid_executed", "@@GLOBAL.gtid_executed",
            "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9");
}

TEST_F(QueryResponderTest,
       ReadsEmptyHistoryAndTheDefaultChecksumWithoutAState) {
  const QueryResponder bare(
      ServerIdentity{1, "8a94f357-aab4-11df-86ab-c80aa9429562", "", "", ""},
      nullptr);
  QueryResponse response = bare.Respond("SELECT @@GLOBAL.gtid_purged", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Row);
  EXPECT_EQ(response.columns.at(0).value, std::optional<std::string>(""));
  response = bare.Respond("SELECT @@GLOBAL.binlog_checksum", session);
  EXPECT_EQ(response.columns.at(0).value, std::optional<std::string>("CRC32"));
}

TEST_F(QueryResponderTest, AnswersUnixTimestampWithTheCurrentTime) {
  const QueryResponse response =
      responder.Respond("SELECT UNIX_TIMESTAMP()", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Row);
  ASSERT_TRUE(response.columns.at(0).value.has_value());
  EXPECT_GT(std::stoll(*response.columns.at(0).value), 1700000000LL);
}

TEST_F(QueryResponderTest, TheChecksumNegotiationOfAReplicaRoundTrips) {
  const QueryResponse set = responder.Respond(
      "SET @master_binlog_checksum = @@global.binlog_checksum, "
      "@source_binlog_checksum = @@global.binlog_checksum",
      session);
  EXPECT_EQ(set.kind, QueryResponseKind::Ok) << set.message;
  ExpectRow("SELECT @source_binlog_checksum", "@source_binlog_checksum",
            "CRC32");
  ExpectRow("SELECT @master_binlog_checksum", "@master_binlog_checksum",
            "CRC32");
}

TEST_F(QueryResponderTest, StoresQuotedAndNumericUserVariables) {
  EXPECT_EQ(responder
                .Respond("SET @master_heartbeat_period = 30000000000, "
                         "@source_heartbeat_period = 30000000000",
                         session)
                .kind,
            QueryResponseKind::Ok);
  EXPECT_EQ(
      responder
          .Respond("SET @slave_uuid = '11111111-2222-3333-4444-555555555555', "
                   "@replica_uuid = '11111111-2222-3333-4444-555555555555'",
                   session)
          .kind,
      QueryResponseKind::Ok);
  EXPECT_EQ(session.Get("source_heartbeat_period"),
            std::optional<std::string>("30000000000"));
  EXPECT_EQ(session.Get("replica_uuid"),
            std::optional<std::string>("11111111-2222-3333-4444-555555555555"));
}

TEST_F(QueryResponderTest,
       UserVariableNamesAreCaseInsensitiveAndAnUnsetOneReadsAsNull) {
  EXPECT_EQ(responder.Respond("SET @Foo = 'a, b'", session).kind,
            QueryResponseKind::Ok);
  ExpectRow("SELECT @FOO", "@FOO", "a, b");
  ExpectRow("SELECT @never_set", "@never_set", std::nullopt);
}

TEST_F(QueryResponderTest, AnUnknownSystemVariableIsError1193) {
  const QueryResponse response = responder.Respond(
      "SELECT @@GLOBAL.rpl_semi_sync_source_enabled", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Error);
  EXPECT_EQ(response.errorCode, 1193);
  EXPECT_EQ(response.sqlState, "HY000");
  EXPECT_EQ(response.message,
            "Unknown system variable 'rpl_semi_sync_source_enabled'");
}

TEST_F(QueryResponderTest, ASetThatIsNotFullyUnderstoodStoresNothing) {
  const QueryResponse response =
      responder.Respond("SET @a = 1, @@global.read_only = 1", session);
  EXPECT_EQ(response.kind, QueryResponseKind::Error);
  EXPECT_EQ(response.errorCode, 1235);
  EXPECT_FALSE(session.Get("a").has_value());
}

TEST_F(QueryResponderTest, AnyOtherStatementIsError1235) {
  const QueryResponse response = responder.Respond("SHOW DATABASES", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Error);
  EXPECT_EQ(response.errorCode, 1235);
  EXPECT_EQ(response.sqlState, "42000");
}

TEST_F(QueryResponderTest,
       AnswersTheVersionMysqlbinlogAsksForWithTheOneOfTheGreeting) {
  ExpectRow("SELECT VERSION()", "VERSION()", "8.4.11-relay-1.0");
  ExpectRow("select version();", "version()", "8.4.11-relay-1.0");
  ExpectRow("SELECT @@version", "@@version", "8.4.11-relay-1.0");
}

TEST_F(QueryResponderTest, ShowsThePreviousGtidsOfAStoredFileAsAServerDoes) {
  const QueryResponse response = responder.Respond(
      "show binlog events in 'binlog.000007' limit 1, 1", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Row);
  ASSERT_EQ(response.columns.size(), 6u);
  const std::vector<std::string> names{"Log_name",  "Pos",         "Event_type",
                                       "Server_id", "End_log_pos", "Info"};
  const std::vector<std::string> values{
      "binlog.000007",
      "127",
      "Previous_gtids",
      "1",
      "198",
      "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(response.columns[i].name, names[i]);
    EXPECT_EQ(response.columns[i].value, std::optional<std::string>(values[i]));
  }
}

TEST_F(
    QueryResponderTest,
    AFileTheRelayDoesNotHoldIsError1220AndAnyOtherShowBinlogEventsError1235) {
  EXPECT_EQ(
      responder
          .Respond("SHOW BINLOG EVENTS IN 'binlog.000001' LIMIT 1,1", session)
          .errorCode,
      1220);
  EXPECT_EQ(
      responder
          .Respond("SHOW BINLOG EVENTS IN 'binlog.000007' LIMIT 1,1", session)
          .kind,
      QueryResponseKind::Row);
  EXPECT_EQ(responder.Respond("SHOW BINLOG EVENTS IN 'binlog.000007'", session)
                .errorCode,
            1235);
  EXPECT_EQ(
      responder
          .Respond("SHOW BINLOG EVENTS IN 'binlog.000007' LIMIT 2,1", session)
          .errorCode,
      1235);
  EXPECT_EQ(responder.Respond("SHOW BINLOG EVENTS", session).errorCode, 1235);
}

TEST_F(QueryResponderTest, AnswersWhatTheCommandLineClientAsksOnConnect) {
  ExpectRow("select @@version_comment limit 1", "@@version_comment",
            "relay 1.0");
  EXPECT_EQ(responder.Respond("SET NAMES utf8mb4", session).kind,
            QueryResponseKind::Ok);
}

TEST_F(QueryResponderTest, SeveralExpressionsAnswerInColumnsOfTheirOwn) {
  const QueryResponse response = responder.Respond(
      "SELECT @@server_id, @@global.gtid_executed , VERSION()", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Row) << response.message;
  ASSERT_EQ(response.columns.size(), 3u);
  EXPECT_EQ(response.columns[0].name, "@@server_id");
  EXPECT_EQ(response.columns[0].value, std::optional<std::string>("1001"));
  EXPECT_EQ(response.columns[1].name, "@@global.gtid_executed");
  EXPECT_EQ(
      response.columns[1].value,
      std::optional<std::string>("3e11fa47-71ca-11e1-9e33-c80aa9429562:1-9"));
  EXPECT_EQ(response.columns[2].name, "VERSION()");
  EXPECT_EQ(response.columns[2].value,
            std::optional<std::string>("8.4.11-relay-1.0"));
}

TEST_F(QueryResponderTest, AnUnknownVariableInAListIsNamedAloneInTheError) {
  const QueryResponse response = responder.Respond(
      "SELECT @@server_id, @@global.rpl_semi_sync_source_enabled", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Error);
  EXPECT_EQ(response.errorCode, 1193);
  EXPECT_EQ(response.message,
            "Unknown system variable 'rpl_semi_sync_source_enabled'");
}

TEST_F(QueryResponderTest, AKeywordIsReadOnlyWhereAWordEnds) {
  // The space after SELECT is not what makes it a keyword: a longer word
  // starting with it is a different statement, and a sign is not a letter.
  EXPECT_EQ(responder.Respond("SELECTED @@server_id", session).errorCode, 1235);
  EXPECT_EQ(responder.Respond("SETTINGS @a = 1", session).errorCode, 1235);
  ExpectRow("SELECT@@server_id", "@@server_id", "1001");
}

TEST_F(QueryResponderTest, ReadsNamesWrittenInBackticks) {
  ExpectRow("SELECT @@`server_id`", "@@`server_id`", "1001");
  ExpectRow("SELECT @@GLOBAL.`server_id`", "@@GLOBAL.`server_id`", "1001");
  EXPECT_EQ(responder.Respond("SET @`replica uuid` = 'u-1'", session).kind,
            QueryResponseKind::Ok);
  ExpectRow("SELECT @`Replica Uuid`", "@`Replica Uuid`", "u-1");
}

TEST_F(QueryResponderTest, WhatIsNotAVariableReferenceIsError1235) {
  // Not "a variable the relay does not have" - a statement it did not
  // read. Answering 1193 here would tell a client its source is too old
  // for a name it never asked about.
  for (const std::string &sql :
       {std::string("SELECT @@"), std::string("SELECT @@global."),
        std::string("SELECT @@server_id server_id"),
        std::string("SELECT @@server_id, "), std::string("SELECT @")}) {
    const QueryResponse response = responder.Respond(sql, session);
    ASSERT_EQ(response.kind, QueryResponseKind::Error) << sql;
    EXPECT_EQ(response.errorCode, 1235) << sql;
  }
}

TEST_F(QueryResponderTest, TheSpacingOfShowBinlogEventsIsNotWhatIsRead) {
  const QueryResponse response = responder.Respond(
      "  show   binlog\tevents  in  'binlog.000007'  limit 1 , 1 ; ", session);
  ASSERT_EQ(response.kind, QueryResponseKind::Row) << response.message;
  EXPECT_EQ(response.columns.at(0).value,
            std::optional<std::string>("binlog.000007"));
}

}  // namespace
}  // namespace binlog_streamer
