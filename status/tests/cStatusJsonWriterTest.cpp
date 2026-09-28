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

#include "status/cStatusJsonWriter.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

RelayStatus Sample() {
  RelayStatus status;
  status.name = "binlog-streamer";
  status.version = "0.30.0";
  status.state = RelayState::Serving;
  status.startedAt = 1'700'000'000;
  status.now = 1'700'003'600;
  status.uptimeSeconds = 3600;
  status.maxConnections = 151;
  status.source.address = "source.example:3306";
  status.source.connected = true;
  status.source.since = 1'700'000'010;
  status.source.serverId = 7;
  status.source.serverUuid = "3e11d2a0-0000-0000-0000-000000000007";
  status.source.version = "8.4.11";
  status.source.tls = true;
  status.source.compression = "zlib";
  status.source.seen = {"binlog.000412", 5'183'920, 1'700'003'598, false};
  status.source.clock = 1'700'003'600;
  status.source.behindSeconds = 2;
  status.storage.files = 3;
  status.storage.bytes = 3'221'225'472;
  status.storage.maxBytes = 274'877'906'944;
  status.storage.file = "binlog.000412";
  status.storage.position = 5'183'900;
  status.storage.behindBytes = 20;
  status.memory.files = 2;
  status.memory.bytes = 7'340'032;
  status.memory.maxBytes = 4'294'967'296;
  ReplicaStatus replica;
  replica.facts = {"10.0.1.7:41822", "replica1", true,
                   "zstd",           "mysqld",   "8.4.6-6"};
  replica.since = 1'700'001'000;
  replica.state = ReplicaState::Streaming;
  replica.sent = {"binlog.000412", 5'183'900, 1'700'003'598, true};
  replica.behindBytes = 0;
  replica.behindSeconds = 0;
  replica.reportHost = "replica1.example";
  status.replicas.push_back(replica);
  return status;
}

TEST(StatusJsonWriterTest, WritesEveryFieldAsPlainNumbersAndNames) {
  const std::string json = StatusJsonWriter::Write(Sample());
  EXPECT_EQ(
      json,
      "{\"name\":\"binlog-streamer\",\"version\":\"0.30.0\","
      "\"state\":\"serving\",\"state_code\":1,"
      "\"started_at\":1700000000,\"now\":1700003600,\"uptime_seconds\":3600,"
      "\"source\":{\"address\":\"source.example:3306\",\"connected\":true,"
      "\"since\":1700000010,\"attempt\":0,\"server_id\":7,"
      "\"server_uuid\":\"3e11d2a0-0000-0000-0000-000000000007\","
      "\"version\":\"8.4.11\",\"tls\":true,\"compression\":\"zlib\","
      "\"file\":\"binlog.000412\","
      "\"position\":5183920,\"timestamp\":1700003598,\"caught_up\":false,"
      "\"clock\":1700003600,\"behind_seconds\":2},"
      "\"storage\":{\"files\":3,\"bytes\":3221225472,"
      "\"max_bytes\":274877906944,"
      "\"file\":\"binlog.000412\",\"position\":5183900,\"behind_bytes\":20},"
      "\"memory\":{\"files\":2,\"bytes\":7340032,\"max_bytes\":4294967296},"
      "\"replicas\":[{\"address\":\"10.0.1.7:41822\","
      "\"report_host\":\"replica1.example\",\"user\":\"replica1\","
      "\"program\":\"mysqld\",\"version\":\"8.4.6-6\","
      "\"since\":1700001000,\"tls\":true,\"compression\":\"zstd\","
      "\"state\":\"streaming\",\"state_code\":2,\"file\":\"binlog.000412\","
      "\"position\":5183900,\"timestamp\":1700003598,\"behind_bytes\":0,"
      "\"behind_seconds\":0}],"
      "\"replicas_connected\":1,\"max_connections\":151}\n");
}

TEST(StatusJsonWriterTest, UnknownIsNullNotZeroOrEmpty) {
  RelayStatus status;
  status.name = "relay";
  const std::string json = StatusJsonWriter::Write(status);
  EXPECT_NE(json.find("\"state\":\"starting\",\"state_code\":0"),
            std::string::npos);
  EXPECT_NE(json.find("\"connected\":false,\"since\":null"), std::string::npos);
  EXPECT_NE(json.find("\"server_uuid\":null,\"version\":null,"
                      "\"tls\":false,\"compression\":null,\"file\":null,"
                      "\"position\":0,\"timestamp\":null,\"caught_up\":false,"
                      "\"clock\":null,\"behind_seconds\":null"),
            std::string::npos);
  EXPECT_NE(json.find("\"storage\":{\"files\":0,\"bytes\":0,\"max_bytes\":0,"
                      "\"file\":null,\"position\":0,\"behind_bytes\":null},"
                      "\"memory\":{\"files\":0,\"bytes\":0,\"max_bytes\":0}"),
            std::string::npos);
  EXPECT_NE(json.find("\"replicas\":[],\"replicas_connected\":0"),
            std::string::npos);
}

TEST(StatusJsonWriterTest, EscapesWhatAStringMayCarry) {
  RelayStatus status;
  status.source.address = "a\"b\\c\n\x01";
  const std::string json = StatusJsonWriter::Write(status);
  EXPECT_NE(json.find("\"address\":\"a\\\"b\\\\c\\n\\u0001\""),
            std::string::npos);
}

TEST(StatusJsonWriterTest, ReplicaThatSentNoVersionHasNullProgramAndVersion) {
  RelayStatus status;
  ReplicaStatus replica;
  replica.facts = {"10.0.1.7:41822", "replica1", false, "none", "", ""};
  status.replicas.push_back(replica);
  const std::string json = StatusJsonWriter::Write(status);
  EXPECT_NE(json.find("\"report_host\":null,\"user\":\"replica1\","
                      "\"program\":null,\"version\":null,"),
            std::string::npos)
      << json;
}

}  // namespace
}  // namespace binlog_streamer
