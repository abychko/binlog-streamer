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

#include "cDumpStartResolver.hpp"

#include <gtest/gtest.h>
#include <vector>

namespace binlog_streamer {
namespace {

constexpr char RELAY_UUID[] = "8a94f357-aab4-11df-86ab-c80aa9429562";
constexpr char SOURCE_UUID[] = "3e11fa47-71ca-11e1-9e33-c80aa9429562";

// Scripts the storage: which file FindStartFile() names on each call and
// whether Open() succeeds. A default-constructed cursor stands for an open
// file; nothing here reads through it.
class ScriptedReader : public BinlogStorageReader {
 public:
  std::string publishedFile = "binlog.000003";
  std::vector<std::optional<std::string>>
      startFiles;        // one per FindStartFile() call, the last one repeating
  int failingOpens = 0;  // how many Open() calls fail before one succeeds
  mutable int findCalls = 0;
  int openCalls = 0;
  mutable GtidSet lastReplicaSet;

  std::optional<std::string> FindStartFile(
      const GtidSet &replicaSet) const override {
    lastReplicaSet = replicaSet;
    const std::size_t index = std::min<std::size_t>(
        static_cast<std::size_t>(findCalls), startFiles.size() - 1);
    ++findCalls;
    return startFiles[index];
  }
  std::unique_ptr<FileCursor> Open(const std::string &,
                                   std::string &error) override {
    ++openCalls;
    if (openCalls <= failingOpens) {
      error = "file removed";
      return nullptr;
    }
    return std::make_unique<FileCursor>();
  }
  std::size_t Read(const FileCursor &, std::uint64_t, std::span<std::uint8_t>,
                   std::string &) override {
    return 0;
  }
  NextFileOutcome Next(const FileCursor &, std::uint64_t,
                       std::unique_ptr<FileCursor> &, std::string &) override {
    return NextFileOutcome::NotYetAvailable;
  }
  WaitOutcome WaitForNewEvents(const PublishedPosition &,
                               std::chrono::milliseconds, WaitStyle) override {
    return WaitOutcome::TimedOut;
  }
  PublishedPosition Published() const override {
    return PublishedPosition{publishedFile, 4};
  }
};

BinlogDumpGtidCommand DumpRequest(const std::string &gtidSetText) {
  GtidSet set;
  std::string error;
  EXPECT_TRUE(set.AddFromText(gtidSetText, error)) << error;
  BinlogDumpGtidCommand command;
  command.serverId = 2002;
  command.gtidSetEncoded = set.Encode(false);
  return command;
}

TEST(DumpStartResolverTest, PinsTheFileTheStorageNamesForTheReplicasSet) {
  ScriptedReader reader;
  reader.startFiles = {std::string("binlog.000002")};
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(SOURCE_UUID) + ":1-10"), RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::Found);
  EXPECT_NE(start.cursor, nullptr);
  EXPECT_EQ(start.replicaSet.ToText(), std::string(SOURCE_UUID) + ":1-10");
  EXPECT_EQ(reader.lastReplicaSet.ToText(), std::string(SOURCE_UUID) + ":1-10");
}

TEST(DumpStartResolverTest,
     AReplicaOlderThanTheStoredHistoryIsToldTheSourcePurgedIt) {
  ScriptedReader reader;
  reader.startFiles = {std::nullopt};
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(SOURCE_UUID) + ":1-10"), RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::PurgedRequiredGtids);
  EXPECT_EQ(start.cursor, nullptr);
  EXPECT_EQ(
      start.message.rfind(
          "Cannot replicate because the source purged required binary logs.",
          0),
      0u)
      << start.message;
  EXPECT_EQ(reader.openCalls, 0);
}

TEST(DumpStartResolverTest, AReplicaHoldingGtidsOfTheRelaysOwnUuidIsRefused) {
  ScriptedReader reader;
  reader.startFiles = {std::string("binlog.000002")};
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(SOURCE_UUID) + ":1-10," + RELAY_UUID + ":1"),
      RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::ReplicaHasOwnGtids);
  EXPECT_EQ(
      start.message.rfind("Replica has more GTIDs than the source has", 0), 0u)
      << start.message;
  EXPECT_EQ(reader.findCalls, 0);
}

TEST(DumpStartResolverTest, ATaggedGtidOfTheRelaysOwnUuidIsRefusedToo) {
  ScriptedReader reader;
  reader.startFiles = {std::string("binlog.000002")};
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(RELAY_UUID) + ":some_tag:1-3"), RELAY_UUID,
      reader);
  EXPECT_EQ(start.kind, DumpStartKind::ReplicaHasOwnGtids);
}

TEST(DumpStartResolverTest, AnEmptyStorageIsNotARefusal) {
  ScriptedReader reader;
  reader.publishedFile.clear();
  reader.startFiles = {std::nullopt};
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(SOURCE_UUID) + ":1-10"), RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::NoHistoryYet);
  EXPECT_EQ(reader.findCalls, 0);
}

TEST(DumpStartResolverTest,
     ChoosesAgainWhenTheChosenFileIsRemovedBeforeItIsOpened) {
  ScriptedReader reader;
  reader.startFiles = {std::string("binlog.000001"),
                       std::string("binlog.000002")};
  reader.failingOpens = 1;
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(SOURCE_UUID) + ":1-10"), RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::Found);
  EXPECT_EQ(reader.findCalls, 2);
  EXPECT_EQ(reader.openCalls, 2);
}

TEST(DumpStartResolverTest, GivesUpWhenTheStartFileNeverOpens) {
  ScriptedReader reader;
  reader.startFiles = {std::string("binlog.000001")};
  reader.failingOpens = 100;
  const DumpStart start = DumpStartResolver::Resolve(
      DumpRequest(std::string(SOURCE_UUID) + ":1-10"), RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::StorageError);
  EXPECT_NE(start.message.find("file removed"), std::string::npos)
      << start.message;
}

TEST(DumpStartResolverTest, AnUndecodableGtidSetIsABadRequest) {
  ScriptedReader reader;
  reader.startFiles = {std::string("binlog.000002")};
  BinlogDumpGtidCommand command;
  command.gtidSetEncoded = {0x01, 0x02, 0x03};
  const DumpStart start =
      DumpStartResolver::Resolve(command, RELAY_UUID, reader);
  EXPECT_EQ(start.kind, DumpStartKind::BadRequest);
  EXPECT_EQ(reader.findCalls, 0);
}

}  // namespace
}  // namespace binlog_streamer
