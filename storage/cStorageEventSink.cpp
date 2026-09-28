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

#include "storage/cStorageEventSink.hpp"
#include "cache/hCacheDefaults.hpp"

#include "binlog/cBinlogFileName.hpp"
#include "binlog/cFormatDescriptionEventCodec.hpp"
#include "binlog/cGtidEventCodec.hpp"
#include "binlog/cRotateEventCodec.hpp"
#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "storage/cBinlogHeaderBuilder.hpp"
#include "storage/hStorageDefaults.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace binlog_streamer {
namespace {

// fileName comes from the source's ROTATE event body (arbitrary bytes), not
// this project's own naming - without these checks, a crafted name like
// "../../etc/passwd.000001" would write outside m_dataDir.
bool IsSafeStoredFileName(const std::string &fileName, std::string &error) {
  if (fileName.empty()) {
    error = "empty file name";
    return false;
  }
  if (fileName.find('/') != std::string::npos) {
    error = "file name contains a path separator";
    return false;
  }
  if (fileName.find("..") != std::string::npos) {
    error = "file name contains '..'";
    return false;
  }
  std::string basename;
  std::uint64_t number = 0;
  std::string parseError;
  if (!BinlogFileName::Parse(fileName, basename, number, parseError)) {
    error = "not a plain '<base>.<digits>' name: " + parseError;
    return false;
  }
  return true;
}

}  // namespace

StorageEventSink::StorageEventSink(
    std::filesystem::path dataDir, std::size_t checksumLength, EventSink &next,
    StorageCatalog &catalog, EventCache &cache, StorageWriter &writer,
    PublishedPositionTracker *publishedPosition,
    std::function<void(const std::string &, std::uint64_t)>
        onAbandonedFileClosed)
    : m_dataDir(std::move(dataDir)),
      m_checksumLength(checksumLength),
      m_next(next),
      m_catalog(catalog),
      m_onAbandonedFileClosed(std::move(onAbandonedFileClosed)),
      m_publishedPosition(publishedPosition),
      m_cache(cache),
      m_writer(writer) {}

bool StorageEventSink::Fail(StorageFailure failure, std::string message) {
  m_failed = true;
  m_error = StorageError{failure, std::move(message)};
  return false;
}

bool StorageEventSink::OnEventBegin(const EventHeader &header,
                                    const StreamPosition &position) {
  if (!m_next.OnEventBegin(header, position)) return false;
  if (m_writer.Failed())
    return Fail(StorageFailure::WriteFailed,
                "background write failed: " + m_writer.LastError());

  m_pendingEventBytes.clear();
  m_currentHeader = header;
  m_straddled = false;

  switch (m_state) {
    case State::AwaitingRotate:
      return BeginAwaitingRotate(header);
    case State::AwaitingHeader:
      return BeginAwaitingHeader(header, position);
    case State::AwaitingPreviousGtids:
      return BeginAwaitingPreviousGtids(header, position);
    case State::Writing:
      return BeginWriting(header, position);
  }
  return Fail(StorageFailure::Malformed, "unreachable storage state");
}

bool StorageEventSink::BeginAwaitingRotate(const EventHeader &header) {
  if (header.type != static_cast<std::uint8_t>(EventType::Rotate) ||
      (header.flags & EVENT_FLAG_ARTIFICIAL) == 0)
    return Fail(StorageFailure::Malformed,
                "expected the artificial ROTATE announcing the next file");
  if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
    return Fail(StorageFailure::Malformed, "ROTATE event too large to buffer");
  m_pendingEventBytes.reserve(header.eventLength);
  m_currentAction = Action::BufferRotateAnnouncement;
  return true;
}

bool StorageEventSink::BeginAwaitingHeader(const EventHeader &header,
                                           const StreamPosition &position) {
  if (header.type != static_cast<std::uint8_t>(EventType::FormatDescription) ||
      position.position != 4)
    return Fail(
        StorageFailure::Malformed,
        "expected the Format_description_event at the start of a new file");
  if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
    return Fail(StorageFailure::Malformed,
                "Format_description_event too large to buffer");
  m_pendingEventBytes.reserve(header.eventLength);
  m_currentAction = Action::BufferFormatDescription;
  return true;
}

bool StorageEventSink::BeginAwaitingPreviousGtids(
    const EventHeader &header, const StreamPosition &position) {
  if (header.type != static_cast<std::uint8_t>(EventType::PreviousGtids) ||
      position.position != m_expectedPreviousGtidsOffset)
    return Fail(StorageFailure::Malformed,
                "file has no Previous_gtids_event right after its "
                "Format_description_event");
  if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
    return Fail(StorageFailure::Malformed,
                "Previous_gtids_event too large to buffer");
  m_pendingEventBytes.reserve(header.eventLength);
  m_currentAction = Action::BufferPreviousGtids;
  return true;
}

bool StorageEventSink::BeginRestartedStream(const EventHeader &header) {
  if (header.type != static_cast<std::uint8_t>(EventType::Rotate) ||
      (header.flags & EVENT_FLAG_ARTIFICIAL) == 0)
    return Fail(StorageFailure::Malformed,
                "expected the artificial ROTATE announcing the file the new "
                "stream begins at");
  if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
    return Fail(StorageFailure::Malformed, "ROTATE event too large to buffer");
  m_pendingEventBytes.reserve(header.eventLength);
  m_currentAction = Action::BufferRotateAnnouncement;
  return true;
}

bool StorageEventSink::BeginWriting(const EventHeader &header,
                                    const StreamPosition &position) {
  // Unlike the same event in AwaitingRotate, this one can arrive with a
  // transaction still open: the stream before it ended inside one, and
  // the source is about to send that transaction again.
  if (m_restarted) return BeginRestartedStream(header);

  const bool isHeartbeat =
      header.type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
      header.type == static_cast<std::uint8_t>(EventType::HeartbeatV2);
  if (isHeartbeat) {
    // position is EventStreamReader's own 64-bit running count, not
    // header.nextPosition (only the low 32 bits) - it may reflect a
    // silently skipped group; a backward one is already rejected upstream.
    if (position.position > m_appendedPosition)
      return Fail(StorageFailure::GapDetected,
                  "stream position is past what storage has accepted for this "
                  "file (a heartbeat announced a "
                  "skipped group storage does not have)");
    m_currentAction = Action::Ignore;
    return true;
  }

  m_currentEventStartOffset = position.position;

  if (position.position < m_appendedPosition) {
    // Only a resumed file has bytes on disk to read back: for a file this
    // run created itself nothing was ever compared, and m_resumeFile is
    // not open. The stream reader does not hand out such a position, so
    // this is a refusal rather than a comparison.
    if (!m_resumeFile)
      return Fail(StorageFailure::Malformed,
                  "event position is behind what storage has written to " +
                      m_currentFileName + ", a file this run created itself");

    // A resumed file's bytes must already be entirely on disk: recovery
    // truncates it to a transaction boundary first, so straddling it means
    // storage and the source have already diverged. The exception is the
    // event a lost stream cut in half: it begins exactly where storage
    // stops holding whole events, and its remainder is appended.
    const std::uint64_t claimedEnd = position.position + header.eventLength;
    const bool cutInHalf = position.position == m_completedPosition &&
                           m_completedPosition < m_appendedPosition;
    if (claimedEnd > m_appendedPosition && !cutInHalf)
      return Fail(StorageFailure::Malformed,
                  "storage does not match source history: event position is "
                  "behind what storage has "
                  "written, but the event's own length runs past it");

    if (header.type ==
        static_cast<std::uint8_t>(EventType::FormatDescription)) {
      if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
        return Fail(StorageFailure::Malformed,
                    "Format_description_event too large to buffer for a resume "
                    "comparison");
      m_pendingEventBytes.reserve(header.eventLength);
      m_currentAction = Action::CompareFormatDescription;
    } else {
      // Streamed, not buffered whole: unlike the FDE, this can be up to
      // MAX_EVENT_LENGTH.
      m_compareOffset = position.position;
      // Frozen here, because appending this event's own remainder moves
      // m_appendedPosition: a later chunk of it would otherwise be read
      // back and compared against bytes this event has just appended,
      // which the writer has not necessarily put on disk yet.
      m_compareLimit = m_appendedPosition;
      m_straddled = claimedEnd > m_appendedPosition;
      m_currentAction = Action::CompareStreamed;
    }
    return true;
  }

  if (header.type == static_cast<std::uint8_t>(EventType::Rotate)) {
    if ((header.flags & EVENT_FLAG_ARTIFICIAL) != 0) {
      // Streamed only to a relay that was already behind when the source
      // moved on to the next file.
      if (m_boundaryTracker.InGroup())
        return Fail(StorageFailure::Malformed,
                    "artificial ROTATE interrupted an open transaction group");
      if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
        return Fail(StorageFailure::Malformed,
                    "ROTATE event too large to buffer");
      m_pendingEventBytes.reserve(header.eventLength);
      m_currentAction = Action::BufferRotateAnnouncement;
      return true;
    }
    if (m_boundaryTracker.InGroup())
      return Fail(StorageFailure::Malformed,
                  "ROTATE interrupted an open transaction group");
    if (position.position > m_appendedPosition)
      return Fail(
          StorageFailure::GapDetected,
          "event position is past what storage has accepted for this file");
    m_currentAction = Action::AppendThenCloseFile;
    return true;
  }

  if (header.type == static_cast<std::uint8_t>(EventType::FormatDescription) ||
      header.type == static_cast<std::uint8_t>(EventType::PreviousGtids))
    return Fail(StorageFailure::Malformed,
                "unexpected file-header event while a file is already open");

  if (position.position > m_appendedPosition)
    return Fail(
        StorageFailure::GapDetected,
        "event position is past what storage has accepted for this file");

  const bool isGtid =
      header.type == static_cast<std::uint8_t>(EventType::Gtid) ||
      header.type == static_cast<std::uint8_t>(EventType::AnonymousGtid) ||
      header.type == static_cast<std::uint8_t>(EventType::GtidTagged);
  if (isGtid) {
    if (header.eventLength > MAX_BUFFERED_EVENT_SIZE)
      return Fail(StorageFailure::Malformed, "GTID event too large to buffer");
    m_pendingEventBytes.reserve(header.eventLength);
    m_currentAction = Action::BufferGtid;
  } else {
    m_currentAction = Action::AppendDirect;
  }
  return true;
}

bool StorageEventSink::OnEventBytes(std::span<const std::uint8_t> bytes) {
  if (!m_next.OnEventBytes(bytes)) return false;

  switch (m_currentAction) {
    case Action::Ignore:
      return true;
    case Action::BufferRotateAnnouncement:
    case Action::BufferFormatDescription:
    case Action::BufferPreviousGtids:
    case Action::BufferGtid:
    case Action::CompareFormatDescription:
      m_pendingEventBytes.insert(m_pendingEventBytes.end(), bytes.begin(),
                                 bytes.end());
      return true;
    case Action::AppendDirect:
    case Action::AppendThenCloseFile: {
      return AppendBytes(bytes);
    }
    case Action::CompareStreamed: {
      // No buffering: each chunk is read back from disk and compared
      // right away, using m_compareOffset as its own separate cursor
      // - m_appendedPosition never moves for a replay.
      auto incoming = bytes;
      if (m_compareOffset < m_compareLimit) {
        const auto compared = static_cast<std::size_t>(std::min<std::uint64_t>(
            incoming.size(), m_compareLimit - m_compareOffset));
        m_compareBytes.resize(compared);
        const auto onDisk = std::span<std::uint8_t>(m_compareBytes);
        std::string readError;
        if (!m_resumeFile->ReadAt(m_compareOffset, onDisk, readError))
          return Fail(StorageFailure::WriteFailed,
                      "reading back " + m_currentFileName + " at offset " +
                          std::to_string(m_compareOffset) +
                          " for a resume comparison: " + readError);
        if (!std::equal(onDisk.begin(), onDisk.end(), incoming.begin()))
          return Fail(StorageFailure::Malformed,
                      "storage does not match source history");
        m_compareOffset += compared;
        incoming = incoming.subspan(compared);
      }
      // Only an event cut in half by a lost stream reaches past what
      // storage holds; every other one was refused in OnEventBegin().
      if (incoming.empty()) return true;
      return AppendBytes(incoming);
    }
  }
  return Fail(StorageFailure::Malformed, "unreachable storage action");
}

bool StorageEventSink::OnEventEnd() {
  if (!m_next.OnEventEnd()) return false;

  switch (m_currentAction) {
    case Action::Ignore:
      return true;
    case Action::BufferRotateAnnouncement:
      return EndRotateAnnouncement();
    case Action::BufferFormatDescription:
      return EndFormatDescription();
    case Action::BufferPreviousGtids:
      return EndPreviousGtids();
    case Action::BufferGtid:
      return EndGtid();
    case Action::AppendDirect:
      return EndAppendDirect();
    case Action::AppendThenCloseFile:
      return EndCloseFile();
    case Action::CompareFormatDescription:
      return EndCompareFormatDescription();
    case Action::CompareStreamed:
      // An event cut in half by a lost stream had its second half
      // appended just now, so it ends like any appended event; for every
      // other one OnEventBytes() already compared every chunk.
      return m_straddled ? EndAppendDirect() : true;
  }
  return Fail(StorageFailure::Malformed, "unreachable storage action");
}

bool StorageEventSink::EndRotateAnnouncement() {
  RotateEvent rotate;
  std::string error;
  const auto body = std::span<const std::uint8_t>(m_pendingEventBytes)
                        .subspan(EVENT_HEADER_LENGTH);
  if (!RotateEventCodec::Parse(body, m_checksumLength, rotate, error))
    return Fail(StorageFailure::Malformed, "malformed ROTATE event: " + error);
  if (!IsSafeStoredFileName(rotate.fileName, error))
    return Fail(StorageFailure::Malformed,
                "unsafe file name in ROTATE event: " + error);

  if (m_restarted) {
    m_restarted = false;
    if (rotate.fileName == m_currentFileName) {
      // The same file, still open: only the comparison needs a handle of
      // its own to read it back. The position, the cached file and the
      // transaction the lost stream ended inside stay as they were.
      const auto path = (m_dataDir / rotate.fileName).string();
      m_resumeFile.emplace();
      std::string openError;
      if (!m_resumeFile->Open(path, openError)) {
        m_resumeFile.reset();
        return Fail(StorageFailure::WriteFailed,
                    "resuming " + rotate.fileName + ": " + openError);
      }
      return true;
    }
    // Another file: the source moved on while the relay was away. What
    // is open is closed below, the way a rotation closes it - which a
    // file ending mid-transaction cannot be, since the source would then
    // have had to send the rest of that transaction first.
    if (m_completedPosition < m_appendedPosition || m_boundaryTracker.InGroup())
      return Fail(StorageFailure::Malformed,
                  "the new stream begins at " + rotate.fileName + ", but " +
                      m_currentFileName +
                      " ends inside a transaction the lost stream never "
                      "finished - the source no longer has the history "
                      "storage stopped at");
  }

  // An announcement naming the file already open would mean something
  // else entirely.
  if (m_state == State::Writing && rotate.fileName == m_currentFileName)
    return Fail(StorageFailure::Malformed,
                "unexpected artificial ROTATE while a file is still open");

  // While this run holds a file open, the catalog's last record is that
  // file; otherwise it is history an earlier run left behind, because
  // StorageRecovery runs first.
  const std::optional<StoredFileRecord> last = m_catalog.Last();
  const bool haveLastRecord = last.has_value();
  const StoredFileRecord lastRecord =
      haveLastRecord ? *last : StoredFileRecord{};

  if (haveLastRecord && lastRecord.inUse &&
      lastRecord.name == rotate.fileName) {
    const auto path = (m_dataDir / rotate.fileName).string();
    m_resumeFile.emplace();
    std::string openError;
    if (!m_resumeFile->Open(path, openError)) {
      m_resumeFile.reset();
      return Fail(StorageFailure::WriteFailed,
                  "resuming " + rotate.fileName + ": " + openError);
    }
    m_currentFileName = rotate.fileName;
    m_appendedPosition = lastRecord.size;
    // Recovery has truncated the file to a transaction boundary, so what
    // it holds ends on a whole event.
    m_completedPosition = m_appendedPosition;
    m_cache.BeginFile(m_currentFileName, m_appendedPosition);
    m_writer.PostOpenExisting(m_currentFileName);
    m_boundaryTracker.Reset();
    m_state = State::Writing;
    return true;
  }

  // A ROTATE numbered before storage's last file under the same base cannot
  // be a legitimate continuation: the source only assigns numbers above
  // every file on disk under that base (find_uniq_filename).
  if (haveLastRecord) {
    std::string parsedBasename;
    std::uint64_t parsedNumber = 0;
    std::string parseError;
    if (!BinlogFileName::Parse(rotate.fileName, parsedBasename, parsedNumber,
                               parseError))
      return Fail(StorageFailure::Malformed,
                  "ROTATE event names " + rotate.fileName +
                      ", which IsSafeStoredFileName() already parsed but this "
                      "second parse cannot: " +
                      parseError);
    if (parsedBasename == lastRecord.basename &&
        parsedNumber < lastRecord.number)
      return Fail(StorageFailure::Malformed,
                  "an artificial ROTATE named " + rotate.fileName +
                      ", numbered before storage's own last file " +
                      lastRecord.name +
                      " with the same base name - the source's file numbering "
                      "appears to have been reset");
  }

  const auto path = m_dataDir / rotate.fileName;
  std::error_code existsError;
  if (std::filesystem::exists(path, existsError))
    return Fail(
        StorageFailure::NotEmpty,
        "an artificial ROTATE named " + rotate.fileName +
            ", which already exists and is not this run's own resumed file");
  if (existsError)
    return Fail(StorageFailure::WriteFailed,
                "checking " + path.string() + ": " + existsError.message());

  // Every refusal above leaves the open file open and the catalog as it
  // was: closing a file for a rotation this method then refuses would
  // leave the next run meeting "already exists" where it expects to
  // resume.
  if (m_state == State::Writing) {
    if (!EndCloseFile()) return false;
  } else if (haveLastRecord && lastRecord.inUse) {
    // Same closing path crash recovery uses for a crashed source's last
    // file; left set, the next restart's Load() would refuse to start.
    std::string closeError;
    if (!CloseAbandonedFile(lastRecord, closeError))
      return Fail(StorageFailure::WriteFailed,
                  "closing " + lastRecord.name + ": " + closeError);
  }

  m_currentFileName = rotate.fileName;
  m_state = State::AwaitingHeader;
  return true;
}

bool StorageEventSink::CloseAbandonedFile(const StoredFileRecord &record,
                                          std::string &error) {
  if (!m_catalog.Close(record.size, error)) return false;
  if (m_onAbandonedFileClosed)
    m_onAbandonedFileClosed(record.name, record.size);
  m_writer.PostCloseAbandoned(record.name);
  return true;
}

bool StorageEventSink::EndFormatDescription() {
  m_fdeBytes = std::move(m_pendingEventBytes);
  // m_currentHeader still holds this event's own header here; the next
  // OnEventBegin() call (for the PGE) is what overwrites it.
  m_fdeHeader = m_currentHeader;
  m_expectedPreviousGtidsOffset = 4 + m_fdeBytes.size();
  m_state = State::AwaitingPreviousGtids;
  return true;
}

bool StorageEventSink::EndPreviousGtids() {
  std::string error;
  StoredFileRecord record;
  record.name = m_currentFileName;
  std::string nameError;
  if (!BinlogFileName::Parse(m_currentFileName, record.basename, record.number,
                             nameError))
    return Fail(StorageFailure::Malformed,
                "file name storage itself accepted: " + nameError);
  record.createdAt = m_fdeHeader.timestamp;
  record.serverId = m_fdeHeader.serverId;
  record.onDisk = false;
  record.inUse = true;

  FormatDescriptionEvent fde;
  const auto fdeBody =
      std::span<const std::uint8_t>(m_fdeBytes).subspan(EVENT_HEADER_LENGTH);
  if (!FormatDescriptionEventCodec::Parse(fdeBody, fde, error))
    return Fail(StorageFailure::Malformed,
                "malformed Format_description_event: " + error);
  record.checksumAlgorithm = fde.checksumAlgorithm;
  record.serverVersion = fde.serverVersion;

  const std::size_t recordChecksumLength =
      record.checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  const auto pgeBody = std::span<const std::uint8_t>(m_pendingEventBytes)
                           .subspan(EVENT_HEADER_LENGTH);
  if (pgeBody.size() < recordChecksumLength)
    return Fail(
        StorageFailure::Malformed,
        "Previous_gtids_event body shorter than its own checksum trailer");
  if (!record.previousGtids.AddFromEncoding(
          pgeBody.first(pgeBody.size() - recordChecksumLength), error))
    return Fail(StorageFailure::Malformed,
                "malformed Previous_gtids_event: " + error);

  std::vector<std::uint8_t> header;
  if (!BinlogHeaderBuilder::Build(m_fdeBytes, m_pendingEventBytes, header,
                                  error))
    return Fail(StorageFailure::Malformed, error);
  record.size = record.headerLength = header.size();
  m_cache.BeginFile(m_currentFileName, 0);
  m_catalog.Add(std::move(record));
  // Queue creation before streaming a header that may exceed the cache.
  // Until Advance, a reader may find an empty cached prefix and retry.
  m_writer.PostCreate(m_currentFileName, m_fdeBytes, m_pendingEventBytes);
  m_appendedPosition = 0;
  // The cached in-use bit stays set after disk closure. MySQL clears it
  // when sending the FDE (sql/rpl_binlog_sender.cc,
  // send_format_description_event).
  if (!AppendBytes(header)) return false;
  m_completedPosition = m_appendedPosition;
  m_fdeBytes.clear();
  if (m_publishedPosition)
    m_publishedPosition->Advance(m_currentFileName, m_appendedPosition);
  m_boundaryTracker.Reset();
  m_state = State::Writing;
  return true;
}

bool StorageEventSink::EndGtid() {
  GtidEvent gtidEvent;
  std::string error;
  const auto body = std::span<const std::uint8_t>(m_pendingEventBytes)
                        .subspan(EVENT_HEADER_LENGTH);
  const bool tagged =
      m_currentHeader.type == static_cast<std::uint8_t>(EventType::GtidTagged);
  const bool parsed =
      tagged ? GtidEventCodec::ParseTagged(body, m_checksumLength, gtidEvent,
                                           error)
             : GtidEventCodec::Parse(body, m_checksumLength, gtidEvent, error);
  if (!parsed)
    return Fail(StorageFailure::Malformed, "malformed GTID event: " + error);

  if (!AppendBytes(m_pendingEventBytes)) return false;
  m_completedPosition = m_appendedPosition;

  std::string trackError;
  const BoundaryOutcome outcome = m_boundaryTracker.OnEvent(
      m_currentHeader, m_currentEventStartOffset, &gtidEvent, trackError);
  if (outcome == BoundaryOutcome::Malformed)
    return Fail(StorageFailure::Malformed, trackError);
  m_writer.NoteEventCompleted();
  if (outcome == BoundaryOutcome::GroupEnd ||
      outcome == BoundaryOutcome::Standalone)
    return PublishBoundary();
  return true;
}

bool StorageEventSink::EndAppendDirect() {
  m_completedPosition = m_appendedPosition;
  std::string trackError;
  const BoundaryOutcome outcome = m_boundaryTracker.OnEvent(
      m_currentHeader, m_currentEventStartOffset, nullptr, trackError);
  if (outcome == BoundaryOutcome::Malformed)
    return Fail(StorageFailure::Malformed, trackError);
  m_writer.NoteEventCompleted();
  if (outcome == BoundaryOutcome::GroupEnd ||
      outcome == BoundaryOutcome::Standalone)
    return PublishBoundary();
  return true;
}

bool StorageEventSink::EndCloseFile() {
  std::string error;
  m_cache.EndFile();
  if (!m_catalog.Close(m_appendedPosition, error))
    return Fail(StorageFailure::WriteFailed, error);
  if (m_publishedPosition)
    m_publishedPosition->Advance(m_currentFileName, m_appendedPosition);
  m_writer.PostClose(m_currentFileName);
  m_resumeFile.reset();
  m_appendedPosition = 0;
  m_completedPosition = 0;
  m_state = State::AwaitingRotate;
  return true;
}

bool StorageEventSink::EndCompareFormatDescription() {
  m_compareBytes.resize(m_pendingEventBytes.size());
  const auto onDisk = std::span<std::uint8_t>(m_compareBytes);
  std::string readError;
  if (!m_resumeFile->ReadAt(m_currentEventStartOffset, onDisk, readError))
    return Fail(
        StorageFailure::WriteFailed,
        "reading back " + m_currentFileName +
            "'s own Format_description_event for a resume comparison: " +
            readError);
  std::string compareError;
  if (!FormatDescriptionEventCodec::EqualForResume(
          onDisk, m_pendingEventBytes, m_checksumLength, compareError))
    return Fail(StorageFailure::Malformed,
                "storage does not match source history: " + compareError);
  return true;
}

void StorageEventSink::RestartStream() {
  m_pendingEventBytes.clear();
  m_fdeBytes.clear();
  m_compareBytes.clear();
  // Reopened by the announcement of the new stream, against the file
  // that is open by then.
  m_resumeFile.reset();
  m_currentAction = Action::Ignore;
  m_straddled = false;
  m_restarted = m_state == State::Writing;
  if (m_restarted) return;

  // No file of this sink's is open: either none ever was, a genuine
  // rotation closed the last one, or the stream ended between a file's
  // announcement and its header - of which nothing is kept, since the
  // next stream sends the whole header again.
  m_state = State::AwaitingRotate;
  m_currentFileName.clear();
  m_expectedPreviousGtidsOffset = 0;
}

bool StorageEventSink::PublishBoundary() {
  if (m_publishedPosition)
    m_publishedPosition->Advance(m_currentFileName, m_appendedPosition);
  m_writer.WakeForData();
  return true;
}

bool StorageEventSink::AppendBytes(std::span<const std::uint8_t> bytes) {
  while (!bytes.empty()) {
    const auto portion =
        bytes.first(std::min(bytes.size(), CACHE_SEGMENT_SIZE));
    const auto result = m_cache.AppendOrWait(portion);
    if (result == AppendOutcome::Stopped) {
      if (m_writer.Failed())
        return Fail(StorageFailure::WriteFailed,
                    "background write failed: " + m_writer.LastError());
      return false;
    }
    if (result == AppendOutcome::NoSpace)
      return Fail(StorageFailure::Malformed,
                  "event cache rejected a portion within its segment limit");
    m_appendedPosition += portion.size();
    bytes = bytes.subspan(portion.size());
  }
  return true;
}

}  // namespace binlog_streamer
