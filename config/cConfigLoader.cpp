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

#include "cConfigLoader.hpp"
#include "cAddressRangeParser.hpp"
#include "cConfigValidator.hpp"
#include "cYamlMapReader.hpp"
#include "hCachePolicyNames.hpp"
#include "hRetentionPolicyNames.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace binlog_streamer {
LoadResult<Settings> ConfigLoader::Load(const std::string &path) {
  errno = 0;
  std::ifstream input(path);
  if (!input)
    return {{}, {{path, 0, 0, {}, std::strerror(errno ? errno : EIO)}}, {}};
  std::string text;
  try {
    text.assign(std::istreambuf_iterator<char>(input), {});
  } catch (const std::ios_base::failure &) {
    return {{}, {{path, 0, 0, {}, "failed to read file"}}, {}};
  }
  if (input.bad()) return {{}, {{path, 0, 0, {}, "failed to read file"}}, {}};
  return Parse(text, path);
}
LoadResult<Settings> ConfigLoader::Parse(const std::string &text,
                                         const std::string &fileName) {
  LoadResult<Settings> result;
  auto root = YamlMapReader::Document(text, fileName, result.errors);
  if (!result.errors.empty()) return result;
  Settings settings;
  YamlMapReader reader(fileName, root, "", result.errors, result.positions);
  reader.Finish(
      {"server", "storage", "cache", "monitoring", "source", "replica"});
  reader.Redirect("source", "source.yml");
  reader.Redirect("replica", "replica.yml");
  const auto server = reader.Required("server");
  if (server.IsDefined()) {
    YamlMapReader section(fileName, server, "server", result.errors,
                          result.positions);
    section.Finish({"server_id", "max_connections", "send_linger",
                    "listen_address", "listen_port"});
    section.Redirect("listen_address", "replica.yml");
    section.Redirect("listen_port", "replica.yml");
    if (const auto value = section.UnsignedInteger(
            "server_id", 1, std::numeric_limits<std::uint32_t>::max()))
      settings.server.serverId = static_cast<std::uint32_t>(*value);
    if (const auto value =
            section.UnsignedInteger("max_connections", 1, MAX_CONNECTIONS_LIMIT,
                                    /*required=*/false))
      settings.server.maxConnections = static_cast<unsigned>(*value);
    if (const auto value = section.Delay("send_linger", MAX_SEND_LINGER))
      settings.server.sendLinger = *value;
  }
  const auto storage = reader.Required("storage");
  if (storage.IsDefined()) {
    YamlMapReader section(fileName, storage, "storage", result.errors,
                          result.positions);
    section.Finish({"data_dir", "retention", "disk"});
    if (const auto value = section.String("data_dir", false)) {
      settings.storage.dataDir = *value;
      if (value->find('\0') != std::string::npos ||
          !settings.storage.dataDir.is_absolute())
        section.Error("data_dir", "expected an absolute filesystem path");
    }
    const auto retention = section.Required("retention");
    if (retention.IsDefined()) {
      YamlMapReader child(fileName, retention, "storage.retention",
                          result.errors, result.positions);
      child.Finish({"policy", "period"});
      if (const auto value = child.Enumeration("policy", RETENTIONPOLICY_NAMES))
        settings.storage.retention.policy = *value;
      if (const auto value = child.Duration("period"))
        settings.storage.retention.period = *value;
    }
    const auto disk = section.Required("disk");
    if (disk.IsDefined()) {
      YamlMapReader child(fileName, disk, "storage.disk", result.errors,
                          result.positions);
      child.Finish({"max_size", "purge_high_watermark", "purge_low_watermark",
                    "min_free_space", "recovery_reserve"});
      if (const auto value = child.ByteSize("max_size"))
        settings.storage.disk.maxSize = *value;
      if (const auto value = child.ByteSize("purge_high_watermark"))
        settings.storage.disk.purgeHighWatermark = *value;
      if (const auto value = child.ByteSize("purge_low_watermark"))
        settings.storage.disk.purgeLowWatermark = *value;
      if (const auto value = child.ByteSize("min_free_space"))
        settings.storage.disk.minFreeSpace = *value;
      if (const auto value = child.ByteSize("recovery_reserve"))
        settings.storage.disk.recoveryReserve = *value;
    }
  }
  const auto cache = reader.Required("cache");
  if (cache.IsDefined()) {
    YamlMapReader section(fileName, cache, "cache", result.errors,
                          result.positions);
    section.Finish({"policy", "window", "max_size"});
    if (const auto value = section.Enumeration("policy", CACHEPOLICY_NAMES))
      settings.cache.policy = *value;
    if (const auto value = section.Duration("window"))
      settings.cache.window = *value;
    if (const auto value = section.ByteSize("max_size"))
      settings.cache.maxSize = *value;
  }
  // Optional as a whole, and each key inside optional too: the packaged
  // file names the defaults, and an unfilled key means the default, as in
  // replica.yml.
  // A section left with nothing under it means the defaults, like an
  // unfilled key does.
  const auto monitoring = reader.Optional("monitoring");
  if (monitoring.IsDefined() && !monitoring.IsNull()) {
    YamlMapReader section(fileName, monitoring, "monitoring", result.errors,
                          result.positions);
    section.Finish({"http"});
    const auto http = section.Optional("http");
    if (http.IsDefined() && !http.IsNull()) {
      YamlMapReader subsection(fileName, http, "monitoring.http", result.errors,
                               result.positions);
      subsection.Finish({"listen_address", "listen_port", "html_dir"});
      HttpSettings &target = settings.monitoring.http;
      if (const auto value =
              subsection.OptionalNonEmptyString("listen_address")) {
        std::string addressError;
        if (!AddressRangeParser::ParseAddress(*value, target.listenAddress,
                                              addressError))
          subsection.Error("listen_address", addressError);
      }
      if (const auto value = subsection.OptionalNonEmptyUnsignedInteger(
              "listen_port", 1, 65535))
        target.listenPort = static_cast<std::uint16_t>(*value);
      if (const auto value = subsection.OptionalNonEmptyString("html_dir")) {
        target.htmlDir = *value;
        if (value->find('\0') != std::string::npos ||
            !target.htmlDir.is_absolute())
          subsection.Error("html_dir", "expected an absolute filesystem path");
      }
    }
  }
  if (result.errors.empty())
    result.errors =
        ConfigValidator::Validate(settings, result.positions, fileName);
  if (result.errors.empty()) result.value = settings;
  return result;
}
}  // namespace binlog_streamer
