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
// StorageWriter alone creates, appends and closes files; the chained
// sink (m_next) sees every callback before storage validates it.
class StorageEventSink : public EventSink {
 public:
  // Cache, writer, catalog and tracker must outlive this sink.
  // onAbandonedFileClosed reports logical closure of an abandoned file
  // using its recovered size; clearing the disk flag follows
  // separately, on the writer.
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

  // The stream ended and another one is about to take its place, with
  // the same storage behind it. Everything held stays held: the open
  // file, the position in it, a transaction the lost stream ended
  // inside. The next stream begins at that file's start, as a resumed
  // one does, and its events are compared against what is stored rather
  // than written again.
  void RestartStream();

 private:
  // Decided once, up front, in OnEventBegin() - the checks need only
  // the header and position, both already known by then.
  enum class Action {
    Ignore,  // heartbeat, already fully handled in OnEventBegin()
    BufferRotateAnnouncement,  // artificial ROTATE, buffered to decode the
                               // next file's name
    BufferFormatDescription,   // buffered to hand whole to background creation
    BufferPreviousGtids,       // buffered to hand whole to background creation
    BufferGtid,    // buffered so TransactionBoundaryTracker gets its decoded
                   // fields
    AppendDirect,  // appended to the cache as bytes arrive
    AppendThenCloseFile,       // a genuine ROTATE ending the open file
    CompareFormatDescription,  // resuming a file: buffered, compared against
                               // disk with FDE's own field exclusions
    CompareStreamed,  // resuming a file: every other event, compared against
                      // disk chunk by chunk
  };

  enum class State {
    AwaitingRotate,  // no file open; next event must be the artificial ROTATE
                     // naming one
    AwaitingHeader,  // artificial ROTATE seen; next event must be the
                     // Format_description_event
    AwaitingPreviousGtids,  // Format_description_event buffered; next event
                            // must be Previous_gtids_event
    Writing,                // file open; ordinary events are appended to it
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

  // Publishes the recovered size and queues disk closure in stream order.
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

  // Every other event streams straight to the cache per chunk - never
  // buffered whole.
  std::vector<std::uint8_t> m_pendingEventBytes;

  std::vector<std::uint8_t> m_fdeBytes;
  // Captured before the next OnEventBegin() (for the PGE) overwrites
  // m_currentHeader; createdAt/serverId for the StoredFileRecord come
  // from here, not from the PGE's own header.
  EventHeader m_fdeHeader;
  std::uint64_t m_expectedPreviousGtidsOffset = 0;

  // Logical end accepted by the cache; disk may still be behind.
  std::uint64_t m_appendedPosition = 0;

  // The end of the last event accepted whole. Behind m_appendedPosition
  // only after a stream ended in the middle of an event: its first bytes
  // are stored, and the source sends the event again from its start.
  std::uint64_t m_completedPosition = 0;

  // Separate from m_currentEventStartOffset since a streamed comparison
  // has no single moment with the whole event in hand.
  std::uint64_t m_compareOffset = 0;

  // Where the comparison of the event being compared stops: what storage
  // held when that event began. Frozen, unlike m_appendedPosition, which
  // the same event's own appended remainder moves.
  std::uint64_t m_compareLimit = 0;

  // Set by RestartStream(): the next event is the artificial ROTATE of
  // the new stream, and it can arrive with a file of this sink's still
  // open.
  bool m_restarted = false;
  // The event being compared begins before the end of what storage holds
  // and runs past it - the one event a lost stream can cut in half.
  bool m_straddled = false;

  bool m_failed = false;
  StorageError m_error;
};

}  // namespace binlog_streamer
