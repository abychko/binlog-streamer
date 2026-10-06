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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "binlog/cTransactionBoundaryTracker.hpp"
#include "binlog/sEventHeader.hpp"
#include "cache/cEventCache.hpp"
#include "receiver/iEventSink.hpp"
#include "storage/cPublishedPositionTracker.hpp"
#include "storage/cReadOnlyBinlogFile.hpp"
#include "storage/cStorageCatalog.hpp"
#include "storage/cStorageWriter.hpp"
#include "storage/sStorageError.hpp"
namespace binlog_streamer {
class StorageEventSink : public EventSink {
 public:
  // Cache, writer, catalog and tracker must outlive this sink.
  StorageEventSink(std::filesystem::path dataDir, std::size_t checksumLength,
                   EventSink &next, StorageCatalog &catalog, EventCache &cache,
                   StorageWriter &writer,
                   PublishedPositionTracker *published = nullptr,
                   std::function<void(const std::string &, std::uint64_t)>
                       onAbandonedFileClosed = nullptr);
  bool OnEventBegin(const EventHeader &header,
                    const StreamPosition &position) override;
  bool OnEventBytes(std::span<const std::uint8_t> bytes) override;
  bool OnEventEnd() override;
  bool HasFailed() const { return m_failed; }
  const StorageError &LastError() const { return m_error; }

  // Called when the stream ended and another takes its place: the open file,
  // the position and any interrupted transaction stay held. The next stream
  // restarts at the file's start and its events are compared against what is
  // stored.
  void RestartStream();

 private:
  enum class Action {
    Ignore,
    BufferRotateAnnouncement,
    BufferFormatDescription,
    BufferPreviousGtids,
    BufferGtid,
    AppendDirect,
    AppendThenCloseFile,
    CompareFormatDescription,
    CompareStreamed,
  };

  enum class State {
    AwaitingRotate,
    AwaitingHeader,
    AwaitingPreviousGtids,
    Writing,
  };

  bool Fail(StorageFailure failure, std::string message);

  bool BeginAwaitingRotate(const EventHeader &header);
  bool BeginAwaitingHeader(const EventHeader &header,
                           const StreamPosition &position);
  bool BeginAwaitingPreviousGtids(const EventHeader &header,
                                  const StreamPosition &position);
  bool BeginWriting(const EventHeader &header, const StreamPosition &position);
  bool BeginRestartedStream(const EventHeader &header);

  bool EndRotateAnnouncement();
  bool EndFormatDescription();
  bool EndPreviousGtids();
  bool EndGtid();
  bool EndAppendDirect();
  bool EndCloseFile();
  bool EndCompareFormatDescription();
  bool PublishBoundary();
  bool AppendBytes(std::span<const std::uint8_t> bytes);

  bool CloseAbandonedFile(const StoredFileRecord &record, std::string &error);

  std::filesystem::path m_dataDir;
  std::size_t m_checksumLength;
  EventSink &m_next;
  StorageCatalog &m_catalog;
  std::function<void(const std::string &, std::uint64_t)>
      m_onAbandonedFileClosed;
  PublishedPositionTracker *m_publishedPosition;
  EventCache &m_cache;
  StorageWriter &m_writer;

  State m_state = State::AwaitingRotate;
  std::string m_currentFileName;
  std::optional<ReadOnlyBinlogFile> m_resumeFile;
  std::vector<std::uint8_t> m_compareBytes;
  TransactionBoundaryTracker m_boundaryTracker;

  Action m_currentAction = Action::Ignore;
  EventHeader m_currentHeader;
  std::uint64_t m_currentEventStartOffset = 0;

  std::vector<std::uint8_t> m_pendingEventBytes;

  std::vector<std::uint8_t> m_fdeBytes;
  // Captured before the next OnEventBegin() overwrites m_currentHeader.
  EventHeader m_fdeHeader;
  std::uint64_t m_expectedPreviousGtidsOffset = 0;

  std::uint64_t m_appendedPosition = 0;

  // Behind m_appendedPosition only after a stream ended mid-event: the source
  // sends that event again from its start.
  std::uint64_t m_completedPosition = 0;

  std::uint64_t m_compareOffset = 0;

  // What storage held when the compared event began; frozen, unlike
  // m_appendedPosition.
  std::uint64_t m_compareLimit = 0;

  // Set by RestartStream(): the next event is the new stream's artificial
  // ROTATE, which can arrive while a file is still open.
  bool m_restarted = false;
  // The event being compared begins before the end of stored data and runs past
  // it: the one event a lost stream can cut in half.
  bool m_straddled = false;

  bool m_failed = false;
  StorageError m_error;
};

}  // namespace binlog_streamer
