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

#include "binlog/hEventLimits.hpp"
#include "binlog/sEventHeader.hpp"
#include "gtid/cGtidSet.hpp"
#include "net/cByteBuffer.hpp"
#include "net/cPacketChannel.hpp"
#include "sDumpEnd.hpp"
#include "sDumpSenderOptions.hpp"
#include "storage/iBinlogStorageReader.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace binlog_streamer {

// The Format_description creation time is zeroed when the replica already holds
// part of the file; non-zero tells it the source restarted, rolling back its
// in-progress transaction.
class DumpSender {
 public:
  DumpSender(BinlogStorageReader &reader, PacketChannel &channel,
             DumpSenderOptions options);

  DumpEnd Run(std::unique_ptr<FileCursor> cursor, const GtidSet &replicaSet);

 private:
  DumpEnd Send(std::unique_ptr<FileCursor> cursor, const GtidSet &replicaSet);
  enum class ReadStatus { Event, NothingYet, Failed };

  ReadStatus ReadHeader(const FileCursor &cursor, std::uint64_t offset,
                        std::uint8_t (&headerBytes)[EVENT_HEADER_LENGTH],
                        EventHeader &header, std::string &error);
  ReadStatus ReadEvent(const FileCursor &cursor, std::uint64_t offset,
                       ByteBuffer &event, std::string &error);
  ReadStatus PeekEvent(const FileCursor &cursor, std::uint64_t offset,
                       EventHeader &header, std::string &error);
  std::size_t ReadAhead(const FileCursor &cursor, std::uint64_t offset,
                        std::span<std::uint8_t> out, std::string &error);
  bool SendEvent(std::span<const std::uint8_t> event, std::string &error);
  bool SendRotate(const std::string &fileName, std::string &error);
  bool SendHeartbeat(const std::string &fileName, std::uint64_t position,
                     std::string &error);
  bool HeartbeatDue() const;
  static bool ReplicaHas(const GtidSet &replicaSet,
                         std::span<const std::uint8_t> gtidEvent,
                         std::size_t checksumLength);
  static void PrepareFormatDescription(std::span<std::uint8_t> event,
                                       bool zeroCreated);
  bool ShouldStop(DumpEnd &end) const;
  DumpEnd Fail(DumpEnd end, const std::string &message);

  BinlogStorageReader &m_reader;
  PacketChannel &m_channel;
  DumpSenderOptions m_options;
  std::chrono::steady_clock::time_point m_lastSent{};
  bool m_eventChecksum = true;
  bool m_heartbeatQueued = false;
  // Left uninitialized: a read that stops at the published boundary refills it
  // every time a replica catches up, and zeroing the whole window each time
  // cost more than the read.
  std::unique_ptr<std::uint8_t[]> m_window;
  std::size_t m_windowSize = 0;
  std::string m_windowFile;
  std::uint64_t m_windowOffset = 0;
};

}  // namespace binlog_streamer
