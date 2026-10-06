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

#include "cReplicaConfigLoader.hpp"
#include "cAddressRangeParser.hpp"
#include "cTlsFileLoader.hpp"
#include "cYamlMapReader.hpp"
#include "config/hConfigDefaults.hpp"
#include "hCompressionNames.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <utility>

namespace binlog_streamer {
LoadResult<ReplicaSettings> ReplicaConfigLoader::Load(const std::string &path) {
  std::string content;
  LoadResult<ReplicaSettings> result;
  switch (reader_.Read(path, content, result.errors)) {
    case ProtectedFileStatus::Absent:
      return Parse("", path);
    case ProtectedFileStatus::Failed:
      return result;
    case ProtectedFileStatus::Ok:
      result = Parse(content, path);
      if (result.value) ReadTlsFiles(result, path);
      return result;
  }
  return result;
}
void ReplicaConfigLoader::ReadTlsFiles(LoadResult<ReplicaSettings> &result,
                                       const std::string &fileName) const {
  ReplicaSettings &settings = *result.value;
  if (settings.sslCa.empty() && settings.sslCert.empty()) return;
  if (!TlsFileLoader(reader_, expectedOwner_)
           .Read(fileName, result.positions, settings.sslCa, settings.sslCert,
                 settings.sslKey, settings.tls, result.errors))
    result.value.reset();
}
LoadResult<ReplicaSettings> ReplicaConfigLoader::Parse(
    const std::string &text, const std::string &fileName) {
  LoadResult<ReplicaSettings> result;
  const auto root = YamlMapReader::Document(text, fileName, result.errors);
  if (!result.errors.empty()) return result;
  ReplicaSettings settings;
  std::string addressError;
  AddressRangeParser::ParseAddress(DEFAULT_LISTEN_ADDRESS,
                                   settings.listenAddress, addressError);
  if (root.IsNull()) {
    result.value = settings;
    return result;
  }
  YamlMapReader reader(fileName, root, "", result.errors, result.positions);
  reader.Finish({"listen_address", "listen_port", "compression", "ssl_ca",
                 "ssl_cert", "ssl_key", "require_secure_transport", "clients"});
  if (const auto value = reader.OptionalNonEmptyString("listen_address")) {
    if (!AddressRangeParser::ParseAddress(*value, settings.listenAddress,
                                          addressError))
      reader.Error("listen_address", addressError);
  }
  if (const auto value =
          reader.OptionalNonEmptyUnsignedInteger("listen_port", 1, 65535))
    settings.listenPort = static_cast<std::uint16_t>(*value);
  if (const auto value =
          reader.OptionalNonEmptyEnumeration("compression", COMPRESSION_NAMES))
    settings.compression = *value;
  const auto absolutePath = [&](std::string_view key, std::string &path) {
    const auto value = reader.OptionalNonEmptyString(key);
    if (!value) return;
    path = *value;
    if (path.find('\0') != std::string::npos ||
        !std::filesystem::path(path).is_absolute())
      reader.Error(key, "expected an absolute filesystem path");
  };
  absolutePath("ssl_ca", settings.sslCa);
  absolutePath("ssl_cert", settings.sslCert);
  absolutePath("ssl_key", settings.sslKey);
  if (settings.sslCert.empty() != settings.sslKey.empty())
    reader.Error(settings.sslCert.empty() ? "ssl_cert" : "ssl_key",
                 "ssl_cert and ssl_key go together");
  if (const auto value = reader.Bool("require_secure_transport", false))
    settings.requireSecureTransport = *value;
  const auto clients = reader.Optional("clients");
  if (clients.IsDefined() && !clients.IsNull()) {
    if (!clients.IsSequence())
      reader.Error("clients", "wrong type: expected a sequence");
    else {
      std::set<std::string> users;
      for (std::size_t index = 0; index < clients.size(); ++index) {
        const auto prefix = "clients[" + std::to_string(index) + "]";
        YamlMapReader client(fileName, clients[index], prefix, result.errors,
                             result.positions);
        client.Finish({"user", "password", "hosts"});
        ReplicaClient entry;
        if (const auto value = client.String("user")) {
          entry.user = *value;
          if (!users.insert(*value).second)
            client.Error("user", "duplicate user");
        }
        if (const auto value = client.String("password"))
          entry.password = *value;
        const auto hosts = client.Required("hosts");
        if (hosts.IsDefined()) {
          if (!hosts.IsSequence())
            client.Error("hosts", "wrong type: expected a non-empty sequence");
          else if (hosts.size() == 0)
            client.Error("hosts", "expected a non-empty sequence");
          else
            for (std::size_t host = 0; host < hosts.size(); ++host) {
              const auto node = hosts[host];
              AddressRange range;
              std::string error;
              if (!node.IsScalar())
                error = "wrong type: expected an address or CIDR scalar";
              else if (AddressRangeParser::ParseRange(node.Scalar(), range,
                                                      error)) {
                if (std::find(entry.hosts.begin(), entry.hosts.end(), range) !=
                    entry.hosts.end())
                  error = "duplicate address range";
                else
                  entry.hosts.push_back(range);
              }
              if (!error.empty())
                client.ErrorAt(prefix + ".hosts[" + std::to_string(host) + "]",
                               error, node.Mark());
            }
        }
        settings.clients.push_back(std::move(entry));
      }
    }
  }
  if (result.errors.empty()) result.value = std::move(settings);
  return result;
}
}  // namespace binlog_streamer
