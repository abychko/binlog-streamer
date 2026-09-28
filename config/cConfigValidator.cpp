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

#include "cConfigValidator.hpp"
#include "cache/hCacheDefaults.hpp"

namespace binlog_streamer {
std::vector<ConfigError> ConfigValidator::Validate(
    const Settings &settings,
    const std::map<std::string, KeyPosition> &positions,
    const std::string &fileName) {
  std::vector<ConfigError> errors;
  const auto add = [&](const std::string &key, const std::string &message) {
    const auto found = positions.find(key);
    const auto position =
        found == positions.end() ? KeyPosition{} : found->second;
    errors.push_back({fileName, position.line, position.column, key, message});
  };
  const auto &disk = settings.storage.disk;
  if (disk.purgeLowWatermark >= disk.purgeHighWatermark)
    add("storage.disk.purge_low_watermark",
        "must be less than purge_high_watermark");
  if (disk.purgeHighWatermark > disk.maxSize ||
      disk.recoveryReserve > disk.maxSize - disk.purgeHighWatermark)
    add("storage.disk.purge_high_watermark",
        "purge_high_watermark plus recovery_reserve must not exceed max_size");
  if (disk.minFreeSpace == 0)
    add("storage.disk.min_free_space", "must be greater than zero");
  if (disk.recoveryReserve == 0)
    add("storage.disk.recovery_reserve", "must be greater than zero");
  if (disk.purgeLowWatermark == 0)
    add("storage.disk.purge_low_watermark", "must be greater than zero");
  if (settings.cache.maxSize < 2 * CACHE_SEGMENT_SIZE)
    add("cache.max_size", "must be at least 2M (two 1 MiB cache segments)");
  if (settings.cache.window.count() <= 0)
    add("cache.window", "must be greater than zero");
  if (settings.storage.retention.period.count() <= 0)
    add("storage.retention.period", "must be greater than zero");
  return errors;
}
}  // namespace binlog_streamer
