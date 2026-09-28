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

namespace binlog_streamer {

// How a dump request was resolved against the stored history.
enum class DumpStartKind {
  Found,                // the start file is pinned and open
  NoHistoryYet,         // the relay has not stored its first file yet
  PurgedRequiredGtids,  // the replica needs history from before the first
                        // stored file
  ReplicaHasOwnGtids,   // the replica holds GTIDs carrying the relay's own
                        // server UUID
  BadRequest,           // the request's GTID set cannot be decoded
  StorageError,         // the start file could not be opened
};

}  // namespace binlog_streamer
