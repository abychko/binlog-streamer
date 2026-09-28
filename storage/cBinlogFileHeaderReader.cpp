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

#include "storage/cBinlogFileHeaderReader.hpp"

#include "binlog/cBinlogFileName.hpp"
#include "binlog/cEventHeaderCodec.hpp"
#include "binlog/cFormatDescriptionEventCodec.hpp"
#include "binlog/eEventType.hpp"
#include "binlog/hEventFlags.hpp"
#include "binlog/hEventLimits.hpp"
#include "storage/hStorageDefaults.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>

namespace binlog_streamer {
namespace {

// A short pread() on a regular file only happens at EOF, which here means
// the file is shorter than its own header claims - corruption to report,
// not a partial read to retry.
bool ReadExact(int fd, off_t offset, std::span<std::uint8_t> out,
               std::string &error) {
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t count = pread(fd, out.data() + done, out.size() - done,
                                offset + static_cast<off_t>(done));
    if (count < 0) {
      if (errno == EINTR) continue;
      error = std::strerror(errno);
      return false;
    }
    if (count == 0) {
      error = "file is shorter than its own header claims";
      return false;
    }
    done += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

bool BinlogFileHeaderReader::Read(const std::filesystem::path &path,
                                  const std::string &fileName,
                                  StoredFileRecord &record,
                                  std::string &error) {
  StoredFileRecord parsed;
  parsed.name = fileName;
  if (!BinlogFileName::Parse(fileName, parsed.basename, parsed.number, error))
    return false;

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
  parsed.size = static_cast<std::uint64_t>(status.st_size);

  std::array<std::uint8_t, BINLOG_MAGIC.size()> magic{};
  if (!ReadExact(fd, 0, magic, error)) {
    close(fd);
    return false;
  }
  if (magic != BINLOG_MAGIC) {
    error = "missing binlog magic";
    close(fd);
    return false;
  }

  std::array<std::uint8_t, EVENT_HEADER_LENGTH> fdeHeaderBytes{};
  if (!ReadExact(fd, 4, fdeHeaderBytes, error)) {
    close(fd);
    return false;
  }
  EventHeader fdeHeader;
  std::string headerParseError;  // fixed-size header: Parse() cannot fail
  EventHeaderCodec::Parse(fdeHeaderBytes, fdeHeader, headerParseError);
  if (fdeHeader.type !=
      static_cast<std::uint8_t>(EventType::FormatDescription)) {
    error = "file does not start with a Format_description_event";
    close(fd);
    return false;
  }
  // eventLength is a 32-bit wire field; widen before adding to avoid a
  // 32-bit wraparound (usual arithmetic conversions add it to the literal
  // 4 as unsigned int, not as parsed.size's uint64_t).
  if (fdeHeader.eventLength < EVENT_HEADER_LENGTH ||
      4 + static_cast<std::uint64_t>(fdeHeader.eventLength) > parsed.size) {
    error = "Format_description_event length runs past the end of the file";
    close(fd);
    return false;
  }

  std::vector<std::uint8_t> fdeBody(fdeHeader.eventLength -
                                    EVENT_HEADER_LENGTH);
  if (!ReadExact(fd, 4 + EVENT_HEADER_LENGTH, fdeBody, error)) {
    close(fd);
    return false;
  }
  FormatDescriptionEvent fde;
  if (!FormatDescriptionEventCodec::Parse(fdeBody, fde, error)) {
    close(fd);
    return false;
  }
  parsed.createdAt =
      fdeHeader.timestamp;  // Common-Header timestamp, not fde.created
  parsed.serverId = fdeHeader.serverId;
  parsed.checksumAlgorithm = fde.checksumAlgorithm;
  parsed.serverVersion = fde.serverVersion;
  parsed.inUse = (fdeHeader.flags & EVENT_FLAG_BINLOG_IN_USE) != 0;

  // Same widen-before-adding reasoning as above.
  const std::uint64_t pgeOffset =
      4 + static_cast<std::uint64_t>(fdeHeader.eventLength);
  std::array<std::uint8_t, EVENT_HEADER_LENGTH> pgeHeaderBytes{};
  if (!ReadExact(fd, static_cast<off_t>(pgeOffset), pgeHeaderBytes, error)) {
    close(fd);
    return false;
  }
  EventHeader pgeHeader;
  EventHeaderCodec::Parse(pgeHeaderBytes, pgeHeader, headerParseError);
  if (pgeHeader.type != static_cast<std::uint8_t>(EventType::PreviousGtids)) {
    error =
        "file has no Previous_gtids_event right after its "
        "Format_description_event";
    close(fd);
    return false;
  }
  if (pgeHeader.eventLength < EVENT_HEADER_LENGTH ||
      pgeOffset + pgeHeader.eventLength > parsed.size) {
    error = "Previous_gtids_event length runs past the end of the file";
    close(fd);
    return false;
  }

  // checksumLength comes from this file's own FDE, not a negotiated value:
  // no session exists yet at Load() time, before any source has connected.
  const std::size_t checksumLength =
      parsed.checksumAlgorithm == "CRC32" ? CHECKSUM_LENGTH : 0;
  const std::size_t pgeBodyLength = pgeHeader.eventLength - EVENT_HEADER_LENGTH;
  if (pgeBodyLength < checksumLength) {
    error = "Previous_gtids_event body shorter than its own checksum trailer";
    close(fd);
    return false;
  }
  std::vector<std::uint8_t> pgeBody(pgeBodyLength);
  if (!ReadExact(fd, static_cast<off_t>(pgeOffset + EVENT_HEADER_LENGTH),
                 pgeBody, error)) {
    close(fd);
    return false;
  }
  close(fd);

  const auto encoded = std::span<const std::uint8_t>(pgeBody).first(
      pgeBodyLength - checksumLength);
  if (!parsed.previousGtids.AddFromEncoding(encoded, error)) return false;

  parsed.headerLength = pgeOffset + pgeHeader.eventLength;

  record = std::move(parsed);
  return true;
}

}  // namespace binlog_streamer
