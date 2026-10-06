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

#include "cSourceConfigLoader.hpp"
#include "cPublicKeyFileReader.hpp"
#include "cTlsFileLoader.hpp"
#include "cYamlMapReader.hpp"
#include "hCompressionNames.hpp"
#include "hSslModeNames.hpp"
#include "protocol/cRsaPublicKey.hpp"

#include <cstdint>
#include <filesystem>
#include <span>

namespace binlog_streamer {
LoadResult<SourceSettings> SourceConfigLoader::Load(const std::string &path) {
  std::string content;
  LoadResult<SourceSettings> result;
  switch (reader_.Read(path, content, result.errors)) {
    case ProtectedFileStatus::Absent:
      result.errors.push_back(
          {path,
           0,
           0,
           {},
           "source.yml not found: cannot start without a source"});
      return result;
    case ProtectedFileStatus::Failed:
      return result;
    case ProtectedFileStatus::Ok:
      break;
  }
  result = Parse(content, path);
  if (result.value && !result.value->sourcePublicKeyPath.empty())
    ValidatePublicKeyFile(result, path);
  if (result.value && result.value->sslMode != SslMode::Disabled)
    ReadTlsFiles(result, path);
  return result;
}
void SourceConfigLoader::ReadTlsFiles(LoadResult<SourceSettings> &result,
                                      const std::string &fileName) const {
  SourceSettings &settings = *result.value;
  if (!TlsFileLoader(reader_, expectedOwner_)
           .Read(fileName, result.positions, settings.sslCa, settings.sslCert,
                 settings.sslKey, settings.tls, result.errors))
    result.value.reset();
}
LoadResult<SourceSettings> SourceConfigLoader::Parse(
    const std::string &text, const std::string &fileName) {
  LoadResult<SourceSettings> result;
  const auto root = YamlMapReader::Document(text, fileName, result.errors);
  if (!result.errors.empty()) return result;
  YamlMapReader reader(fileName, root, "", result.errors, result.positions);
  reader.Finish({"host", "port", "user", "password", "get_source_public_key",
                 "source_public_key_path", "compression",
                 "zstd_compression_level", "ssl_mode", "ssl_ca", "ssl_cert",
                 "ssl_key"});
  const auto absolutePath = [&](std::string_view key, std::string &path) {
    const auto value = reader.OptionalNonEmptyString(key);
    if (!value) return;
    path = *value;
    if (path.find('\0') != std::string::npos ||
        !std::filesystem::path(path).is_absolute())
      reader.Error(key, "expected an absolute filesystem path");
  };
  SourceSettings settings;
  if (const auto value = reader.String("host")) settings.host = *value;
  if (const auto value = reader.UnsignedInteger("port", 1, 65535, false))
    settings.port = static_cast<std::uint16_t>(*value);
  if (const auto value = reader.String("user")) settings.user = *value;
  if (const auto value = reader.String("password")) settings.password = *value;
  if (const auto value =
          reader.OptionalNonEmptyEnumeration("compression", COMPRESSION_NAMES))
    settings.compression = *value;
  if (const auto value = reader.OptionalNonEmptyUnsignedInteger(
          "zstd_compression_level", MIN_ZSTD_COMPRESSION_LEVEL,
          MAX_ZSTD_COMPRESSION_LEVEL))
    settings.zstdCompressionLevel = static_cast<int>(*value);
  if (const auto value = reader.Bool("get_source_public_key", false))
    settings.getSourcePublicKey = *value;
  absolutePath("source_public_key_path", settings.sourcePublicKeyPath);
  if (const auto value =
          reader.OptionalNonEmptyEnumeration("ssl_mode", SSL_MODE_NAMES))
    settings.sslMode = *value;
  absolutePath("ssl_ca", settings.sslCa);
  absolutePath("ssl_cert", settings.sslCert);
  absolutePath("ssl_key", settings.sslKey);
  if (settings.sslCert.empty() != settings.sslKey.empty())
    reader.Error(settings.sslCert.empty() ? "ssl_cert" : "ssl_key",
                 "ssl_cert and ssl_key go together");
  if ((settings.sslMode == SslMode::VerifyCa ||
       settings.sslMode == SslMode::VerifyIdentity) &&
      settings.sslCa.empty())
    reader.Error("ssl_mode", std::string(SslModeName(settings.sslMode)) +
                                 " requires ssl_ca");
  if (result.errors.empty()) result.value = settings;
  return result;
}
void SourceConfigLoader::ValidatePublicKeyFile(
    LoadResult<SourceSettings> &result, const std::string &fileName) const {
  const auto &keyPath = result.value->sourcePublicKeyPath;
  PublicKeyFileReader keyReader(expectedOwner_);
  std::string content;
  std::string message;
  const auto status = keyReader.Read(keyPath, content, message);
  if (status == ProtectedFileStatus::Absent) {
    message = "file not found";
  } else if (status == ProtectedFileStatus::Ok) {
    RsaPublicKey key;
    std::string parseError;
    const std::span<const std::uint8_t> pem(
        reinterpret_cast<const std::uint8_t *>(content.data()), content.size());
    if (RsaPublicKey::Parse(pem, key, parseError)) {
      result.value->sourcePublicKeyPem = std::move(content);
      return;
    }
    message = parseError;
  }
  const auto found = result.positions.find("source_public_key_path");
  const KeyPosition position =
      found != result.positions.end() ? found->second : KeyPosition{};
  result.errors.push_back({fileName, position.line, position.column,
                           "source_public_key_path", message});
  result.value.reset();
}
}  // namespace binlog_streamer
