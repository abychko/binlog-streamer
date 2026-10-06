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

#include "cTlsFileLoader.hpp"
#include "cPublicKeyFileReader.hpp"
#include "net/cTlsContext.hpp"

#include <utility>

namespace binlog_streamer {
namespace {

constexpr std::string_view KEYS[] = {"ssl_ca", "ssl_cert", "ssl_key"};

ConfigError At(const std::string &fileName,
               const std::map<std::string, KeyPosition> &positions,
               std::string_view key, std::string message) {
  const auto found = positions.find(std::string(key));
  const KeyPosition position =
      found != positions.end() ? found->second : KeyPosition{};
  return {fileName, position.line, position.column, std::string(key),
          std::move(message)};
}

std::string_view KeyOf(const std::string &message) {
  for (const auto key : KEYS)
    if (message.rfind(key, 0) == 0) return key;
  return "ssl_cert";
}

}  // namespace

TlsFileLoader::TlsFileLoader(ProtectedFileReader &reader,
                             std::string expectedOwner)
    : reader_(reader), expectedOwner_(std::move(expectedOwner)) {}

bool TlsFileLoader::Read(const std::string &fileName,
                         const std::map<std::string, KeyPosition> &positions,
                         const std::string &caPath, const std::string &certPath,
                         const std::string &keyPath, TlsMaterial &material,
                         std::vector<ConfigError> &errors) {
  TlsMaterial read;
  bool ok = true;
  PublicKeyFileReader publicReader(expectedOwner_);
  const auto readPublic = [&](std::string_view key, const std::string &path,
                              std::string &content) {
    if (path.empty()) return;
    std::string message;
    const auto status = publicReader.Read(path, content, message);
    if (status == ProtectedFileStatus::Ok) return;
    if (status == ProtectedFileStatus::Absent) message = "file not found";
    errors.push_back(At(fileName, positions, key, message));
    ok = false;
  };
  readPublic("ssl_ca", caPath, read.caPem);
  readPublic("ssl_cert", certPath, read.certPem);
  if (!keyPath.empty()) {
    std::vector<ConfigError> fileErrors;
    const auto status = reader_.Read(keyPath, read.keyPem, fileErrors);
    if (status != ProtectedFileStatus::Ok) {
      std::string message;
      for (const auto &error : fileErrors) {
        if (!message.empty()) message += "; ";
        message += error.message;
      }
      if (status == ProtectedFileStatus::Absent) message = "file not found";
      errors.push_back(At(fileName, positions, "ssl_key", message));
      ok = false;
    }
  }
  if (!ok) return false;
  std::string message;
  if (!TlsContext::Check(read, message)) {
    errors.push_back(At(fileName, positions, KeyOf(message), message));
    return false;
  }
  material = std::move(read);
  return true;
}

}  // namespace binlog_streamer
