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

#include "config/cConfigurationLoader.hpp"
#include "cConfigLoader.hpp"
#include "cReplicaConfigLoader.hpp"
#include "cSourceConfigLoader.hpp"
#include "config/hConfigDefaults.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace binlog_streamer {
LoadResult<Configuration> ConfigurationLoader::Load(
    const std::string &settingsPath) {
  const auto parent = std::filesystem::path(settingsPath).parent_path();
  const auto directory = parent.empty() ? std::filesystem::path(".") : parent;
  auto settings = ConfigLoader::Load(settingsPath);
  auto source = SourceConfigLoader(reader_, expectedOwner_)
                    .Load((directory / SOURCE_FILE_NAME).string());
  auto replica = ReplicaConfigLoader(reader_, expectedOwner_)
                     .Load((directory / REPLICA_FILE_NAME).string());
  LoadResult<Configuration> result;
  for (const auto *list : {&settings.errors, &source.errors, &replica.errors}) {
    for (const auto &error : *list)
      if (std::find(result.errors.begin(), result.errors.end(), error) ==
          result.errors.end())
        result.errors.push_back(error);
  }
  if (result.errors.empty() && settings.value && source.value && replica.value)
    result.value =
        Configuration{std::move(*settings.value), std::move(*source.value),
                      std::move(*replica.value)};
  return result;
}
}  // namespace binlog_streamer
