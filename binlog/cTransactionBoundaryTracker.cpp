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

#include "binlog/cTransactionBoundaryTracker.hpp"

#include <limits>
#include "binlog/eEventType.hpp"

namespace binlog_streamer {
namespace {

bool IsServiceEvent(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(EventType::Rotate) ||
         type == static_cast<std::uint8_t>(EventType::FormatDescription) ||
         type == static_cast<std::uint8_t>(EventType::PreviousGtids) ||
         type == static_cast<std::uint8_t>(EventType::Heartbeat) ||
         type == static_cast<std::uint8_t>(EventType::HeartbeatV2);
}

bool IsGtidEvent(std::uint8_t type) {
  return type == static_cast<std::uint8_t>(EventType::Gtid) ||
         type == static_cast<std::uint8_t>(EventType::AnonymousGtid) ||
         type == static_cast<std::uint8_t>(EventType::GtidTagged);
}

}  // namespace

BoundaryOutcome TransactionBoundaryTracker::OnEvent(const EventHeader &header,
                                                    std::uint64_t fileOffset,
                                                    const GtidEvent *gtidEvent,
                                                    std::string &error) {
  if (!inGroup_) {
    // A tagged GTID (type 42) starts a group like an untagged one; the caller
    // must have decoded it with ParseTagged.
    const bool isGtid =
        header.type == static_cast<std::uint8_t>(EventType::Gtid) ||
        header.type == static_cast<std::uint8_t>(EventType::GtidTagged);
    const bool isAnonymousGtid =
        header.type == static_cast<std::uint8_t>(EventType::AnonymousGtid);
    if (!isGtid && !isAnonymousGtid) {
      error.clear();
      return BoundaryOutcome::Standalone;
    }

    if (gtidEvent == nullptr) {
      error = "GTID event without its decoded fields";
      return BoundaryOutcome::Malformed;
    }
    // GNO sanity check as in the server: GTID_LOG_EVENT requires 1 <= gno <
    // INT64_MAX, ANONYMOUS_GTID_LOG_EVENT exactly 0.
    if (isGtid &&
        (gtidEvent->gno < 1 ||
         gtidEvent->gno >= std::numeric_limits<std::int64_t>::max())) {
      error = "GTID_LOG_EVENT has a GNO outside [1, INT64_MAX)";
      return BoundaryOutcome::Malformed;
    }
    if (isAnonymousGtid && gtidEvent->gno != 0) {
      error = "ANONYMOUS_GTID_LOG_EVENT has a non-zero GNO";
      return BoundaryOutcome::Malformed;
    }
    if (!gtidEvent->hasTransactionLength) {
      error = "GTID event has no transaction length";
      return BoundaryOutcome::Malformed;
    }
    if (gtidEvent->transactionLength == 0) {
      error = "GTID event has a zero transaction length";
      return BoundaryOutcome::Malformed;
    }

    groupEndOffset_ = fileOffset + gtidEvent->transactionLength;
    inGroup_ = true;
    error.clear();
    return BoundaryOutcome::GroupStart;
  }

  // A GTID event always starts a transaction stream; mid-group is an error,
  // unlike an applier's retry, because this relay writes the stream as is.
  if (IsGtidEvent(header.type)) {
    error = "a GTID event interrupted an open transaction group";
    return BoundaryOutcome::Malformed;
  }
  if (IsServiceEvent(header.type)) {
    error = "a service event interrupted an open transaction group";
    return BoundaryOutcome::Malformed;
  }

  const std::uint64_t eventEnd = fileOffset + header.eventLength;
  if (eventEnd < groupEndOffset_) {
    error.clear();
    return BoundaryOutcome::InGroup;
  }
  if (eventEnd == groupEndOffset_) {
    inGroup_ = false;
    error.clear();
    return BoundaryOutcome::GroupEnd;
  }
  // eventEnd > groupEndOffset_: the event runs past the boundary its GTID
  // promised. Left in group on purpose; Reset() is the recovery.
  error = "event crosses the end of its transaction group";
  return BoundaryOutcome::Malformed;
}

bool TransactionBoundaryTracker::InGroup() const { return inGroup_; }

void TransactionBoundaryTracker::Reset() {
  inGroup_ = false;
  groupEndOffset_ = 0;
}

}  // namespace binlog_streamer
