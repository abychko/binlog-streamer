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
#include "storage/cStorageIndexRebuild.hpp"

#include <algorithm>
#include <system_error>
#include <utility>
#include "binlog/cBinlogFileName.hpp"
#include "storage/cBinlogFileHeaderReader.hpp"
#include "storage/cBinlogIndexFile.hpp"
#include "storage/cDirectoryLock.hpp"
#include "storage/hStorageDefaults.hpp"
#include "storage/hStorageOwnFiles.hpp"
#include "storage/sStoredFileRecord.hpp"

namespace binlog_streamer {

bool StorageIndexRebuild::Run(const std::filesystem::path &dataDir,
                              Report &report, bool &accessProblem,
                              std::string &error) {
  report = Report{};
  accessProblem = false;
  DirectoryLock lock;
  if (!lock.Acquire(dataDir, accessProblem, error)) return false;

  const std::string indexPath = (dataDir / INDEX_FILE_NAME).string();
  std::error_code existsError;
  report.previousExisted = std::filesystem::exists(indexPath, existsError);
  if (report.previousExisted &&
      !BinlogIndexFile::Load(indexPath, report.previous, report.previousError))
    report.previous.clear();

  std::vector<StoredFileRecord> files;
  std::vector<std::string> refusals;
  std::error_code listError;
  for (const auto &entry : std::filesystem::directory_iterator(
           dataDir, std::filesystem::directory_options::none, listError)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (IsStorageOwnFile(name)) continue;
    StoredFileRecord file;
    file.name = name;
    std::string parseError;
    if (!BinlogFileName::Parse(name, file.basename, file.number, parseError)) {
      refusals.push_back(name + ": not a binary log (" + parseError + ")");
      continue;
    }
    files.push_back(std::move(file));
  }
  if (listError) {
    error = "listing " + dataDir.string() + ": " + listError.message();
    return false;
  }
  std::sort(files.begin(), files.end(),
            [](const StoredFileRecord &a, const StoredFileRecord &b) {
              return a.basename != b.basename ? a.basename < b.basename
                                              : a.number < b.number;
            });
  if (!files.empty() && files.front().basename != files.back().basename)
    refusals.push_back("binary logs under more than one basename (" +
                       files.front().basename + ", " + files.back().basename +
                       "): their order is not known");

  if (refusals.empty()) {
    for (std::size_t i = 0; i < files.size(); ++i) {
      StoredFileRecord &file = files[i];
      std::string readError;
      if (BinlogFileHeaderReader::Read(dataDir / file.name, file.name, file,
                                       readError))
        continue;
      if (i + 1 == files.size()) {
        report.leftOut = file.name;
        files.pop_back();
        break;
      }
      refusals.push_back(file.name + ": " + readError);
    }
  }
  if (refusals.empty()) {
    for (std::size_t i = 1; i < files.size(); ++i) {
      const StoredFileRecord &before = files[i - 1];
      const StoredFileRecord &file = files[i];
      if (file.number != before.number + 1)
        refusals.push_back(
            "no file between " + before.name + " and " + file.name +
            ": the history has a hole where the missing ones were");
      if (before.inUse)
        refusals.push_back(before.name +
                           " has the \"file in use\" bit set, but is not the "
                           "newest file");
      if (!before.previousGtids.IsSubsetOf(file.previousGtids))
        refusals.push_back("the Previous_gtids of " + file.name +
                           " do not hold those of " + before.name);
    }
  }
  if (!refusals.empty()) {
    error = "the binary logs in " + dataDir.string() +
            " do not make one history, so binlog.index is left as it is:";
    for (const std::string &reason : refusals) error += "\n  " + reason;
    return false;
  }

  for (const StoredFileRecord &file : files)
    report.indexed.push_back(file.name);
  return BinlogIndexFile::Replace(indexPath, report.indexed, error);
}

}  // namespace binlog_streamer
