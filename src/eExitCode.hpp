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

// Every `return` in main() uses one of these, not a bare integer literal.
enum class ExitCode {
  // --help, --version, --validate-config with no errors, or a session
  // that reached registration with the source.
  Success = 0,

  // Command line or settings files are invalid. In systemd's
  // RestartPreventExitStatus (packaging/systemd/binlog-streamer.service)
  // alongside SourceError: retrying unchanged fixes neither.
  ConfigurationError = 1,

  // Unrecoverable source problem after settings loaded successfully:
  // an unsupported version, GTID_MODE != ON, a colliding server_id, or
  // StartupSequence's retry budget exhausted without reaching a dump.
  SourceError = 2,

  // Stream ended after a dump had already started, for a reason a later
  // attempt should recover from on its own. Not in RestartPreventExitStatus:
  // systemd restarts, and the new run resumes from stored history.
  SourceStreamLost = 3,

  // Storage refused to open or refused an event (disk full, write/fsync
  // failure, history mismatch, existing writer). Its own code rather than
  // SourceError, so a disk problem doesn't read as a source problem in
  // monitoring.
  StorageError = 4,
};

}  // namespace binlog_streamer
