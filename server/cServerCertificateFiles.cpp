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

#include "server/cServerCertificateFiles.hpp"

#include "net/cTlsCertificateGenerator.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace binlog_streamer {
namespace {

bool ReadFile(const std::filesystem::path &path, std::string &content,
              std::string &error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot read " + path.string();
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  content = buffer.str();
  return true;
}

bool WriteFile(const std::filesystem::path &path, const std::string &content,
               mode_t mode, std::string &error) {
  const int fd =
      open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) {
    error = "cannot write " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  std::size_t offset = 0;
  while (offset < content.size()) {
    const ssize_t count =
        write(fd, content.data() + offset, content.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      error = "cannot write " + path.string() + ": " + std::strerror(errno);
      close(fd);
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  // Written under a fresh mode too: O_CREAT's mode does not apply to a file
  // that already existed.
  if (fchmod(fd, mode) != 0 || close(fd) != 0) {
    error = "cannot write " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

bool ServerCertificateFiles::LoadOrCreate(const std::filesystem::path &dataDir,
                                          const std::string &name,
                                          TlsMaterial &material,
                                          std::string &error) {
  const auto caPath = dataDir / CA_FILE_NAME;
  const auto caKeyPath = dataDir / CA_KEY_FILE_NAME;
  const auto certPath = dataDir / CERT_FILE_NAME;
  const auto keyPath = dataDir / KEY_FILE_NAME;
  std::error_code ignored;
  const bool certExists = std::filesystem::exists(certPath, ignored);
  const bool keyExists = std::filesystem::exists(keyPath, ignored);
  if (certExists != keyExists) {
    error = (certExists ? keyPath : certPath).string() + " is missing while " +
            (certExists ? certPath : keyPath).string() +
            " exists; restore it, or remove both to generate a new pair";
    return false;
  }
  if (certExists) {
    if (!ReadFile(certPath, material.certPem, error) ||
        !ReadFile(keyPath, material.keyPem, error))
      return false;
    if (material.caPem.empty() && std::filesystem::exists(caPath, ignored) &&
        !ReadFile(caPath, material.caPem, error))
      return false;
    error.clear();
    return true;
  }
  GeneratedCertificates generated;
  if (!TlsCertificateGenerator::Generate(name, generated, error)) return false;
  if (!WriteFile(caKeyPath, generated.caKeyPem, 0600, error) ||
      !WriteFile(caPath, generated.caCertPem, 0644, error) ||
      !WriteFile(keyPath, generated.serverKeyPem, 0600, error) ||
      !WriteFile(certPath, generated.serverCertPem, 0644, error))
    return false;
  material.certPem = std::move(generated.serverCertPem);
  material.keyPem = std::move(generated.serverKeyPem);
  if (material.caPem.empty()) material.caPem = std::move(generated.caCertPem);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
