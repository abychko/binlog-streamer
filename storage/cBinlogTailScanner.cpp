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

#include "storage/cBinlogTailScanner.hpp"

#include "binlog/cEventHeaderCodec.hpp"
#include "binlog/cGtidEventCodec.hpp"
#include "binlog/cTransactionBoundaryTracker.hpp"
#include "binlog/eEventType.hpp"
#include "binlog/hEventLimits.hpp"
#include "gtid/sGtidSource.hpp"
#include "storage/hStorageDefaults.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <vector>

namespace binlog_streamer {
namespace {

// Unlike a typical ReadExact/WriteAll helper, a short result here is not
// itself an error - it's the shape a crash-truncated file takes; Scan()
// decides what it means.
std::size_t ReadUpTo(int fd, off_t offset, std::span<std::uint8_t> out,
                     std::string &error) {
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t count = pread(fd, out.data() + done, out.size() - done,
                                offset + static_cast<off_t>(done));
    if (count < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      return done;
    }
    if (count == 0) break;
    done += static_cast<std::size_t>(count);
  }
  return done;
}

}  // namespace

bool BinlogTailScanner::Scan(const std::filesystem::path &path,
                             std::uint64_t startOffset,
                             std::size_t checksumLength, TailScanResult &result,
                             std::string &error) {
  result = TailScanResult{};
  result.lastBoundary = startOffset;

  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = std::strerror(errno);
    return false;
  }
  struct stat status{};
  if (fstat(fd, &status) != 0) {
    error = std::strerror(errno);
    close(fd);
    return false;
  }
  const auto fileSize = static_cast<std::uint64_t>(status.st_size);
  if (startOffset > fileSize) {
    error = "scan start offset is past the end of the file";
    close(fd);
    return false;
  }

  TransactionBoundaryTracker tracker;
  GtidSource pendingSource;
  std::int64_t pendingGno = 0;
  bool pendingIsAnonymous = false;

  std::uint64_t position = startOffset;
  for (;;) {
    std::array<std::uint8_t, EVENT_HEADER_LENGTH> headerBytes{};
    std::string readError;
    const std::size_t headerRead =
        ReadUpTo(fd, static_cast<off_t>(position), headerBytes, readError);
    if (!readError.empty()) {
      error = readError;
      close(fd);
      return false;
    }
    if (headerRead < headerBytes.size()) break;

    EventHeader header;
    std::string headerParseError;
    EventHeaderCodec::Parse(headerBytes, header, headerParseError);

    // A length shorter than the header is another shape an unfinished write
    // leaves: trimmed, not an error.
    if (header.eventLength < EVENT_HEADER_LENGTH) break;

    const GtidEvent *gtidEventPtr = nullptr;
    GtidEvent gtidEvent;
    const bool isTaggedGtid =
        header.type == static_cast<std::uint8_t>(EventType::GtidTagged);
    const bool isGtid =
        header.type == static_cast<std::uint8_t>(EventType::Gtid) ||
        header.type == static_cast<std::uint8_t>(EventType::AnonymousGtid) ||
        isTaggedGtid;
    // Must run before the fileSize check below: an implausibly large
    // declared length is the one shape treated as fatal, not trimmed -
    // otherwise it would fall through as an ordinary trimmed short body.
    if (isGtid &&
        header.eventLength - EVENT_HEADER_LENGTH > MAX_BUFFERED_EVENT_SIZE) {
      error = "GTID event at offset " + std::to_string(position) +
              " declares a body larger than this scan will buffer";
      close(fd);
      return false;
    }

    if (position + header.eventLength > fileSize) break;

    if (isGtid) {
      std::vector<std::uint8_t> body(header.eventLength - EVENT_HEADER_LENGTH);
      std::string bodyReadError;
      const std::size_t bodyRead =
          ReadUpTo(fd, static_cast<off_t>(position + EVENT_HEADER_LENGTH), body,
                   bodyReadError);
      if (!bodyReadError.empty()) {
        error = bodyReadError;
        close(fd);
        return false;
      }
      if (bodyRead < body.size()) break;
      std::string gtidError;
      const bool decoded = isTaggedGtid
                               ? GtidEventCodec::ParseTagged(
                                     body, checksumLength, gtidEvent, gtidError)
                               : GtidEventCodec::Parse(body, checksumLength,
                                                       gtidEvent, gtidError);
      if (!decoded) break;
      gtidEventPtr = &gtidEvent;
    }

    std::string trackError;
    const BoundaryOutcome outcome =
        tracker.OnEvent(header, position, gtidEventPtr, trackError);
    if (outcome == BoundaryOutcome::Malformed) break;
    if (outcome == BoundaryOutcome::GroupStart) {
      pendingSource = GtidSource{gtidEvent.uuid, gtidEvent.tag};
      pendingGno = gtidEvent.gno;
      pendingIsAnonymous =
          header.type == static_cast<std::uint8_t>(EventType::AnonymousGtid);
    }

    position += header.eventLength;

    if (outcome == BoundaryOutcome::GroupEnd ||
        outcome == BoundaryOutcome::Standalone) {
      result.lastBoundary = position;
      if (outcome == BoundaryOutcome::GroupEnd && !pendingIsAnonymous) {
        // GtidInterval holds actual GTID numbers: the interval is
        // [pendingGno, pendingGno+1).
        result.completedGroups.AddInterval(pendingSource, pendingGno,
                                           pendingGno + 1);
      }
    }
  }

  result.truncatedBytes = fileSize - result.lastBoundary;
  close(fd);
  return true;
}

}  // namespace binlog_streamer
