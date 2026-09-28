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

#include "gtid/cGtidSet.hpp"
#include "server/iServerState.hpp"
#include "storage/cStorageCatalog.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace binlog_streamer {

class StorageServerState : public ServerState {
 public:
  // catalog is not owned and must outlive this object.
  StorageServerState(const StorageCatalog &catalog,
                     std::filesystem::path dataDir)
      : m_catalog(catalog), m_dataDir(std::move(dataDir)) {}

  // Previous_gtids of the first stored file: everything before it is
  // history the relay never had or has already removed.
  std::string GtidPurged() const override;
  // Previous_gtids of the last stored file plus the transactions that
  // file holds on disk. The file is scanned only from where the previous
  // call stopped, so asking often costs little.
  std::string GtidExecuted() const override;
  // The newest stored file's own header, not a running session's
  // greeting: after a restart with the source down, the header is what
  // is left to read it from.
  std::string SourceVersion() const override;
  std::string BinlogChecksum() const override;
  std::optional<PreviousGtidsEvent> PreviousGtids(
      const std::string &fileName) const override;

 private:
  const StorageCatalog &m_catalog;
  std::filesystem::path m_dataDir;
  mutable std::mutex m_mutex;
  mutable std::string m_scannedFile;
  mutable std::uint64_t m_scannedTo = 0;
  mutable GtidSet m_executed;
};

}  // namespace binlog_streamer
