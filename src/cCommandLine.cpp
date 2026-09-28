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

#include "cCommandLine.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace binlog_streamer {
LoadResult<CommandLineOptions> CommandLine::Parse(int argc,
                                                  const char *const *argv) {
  LoadResult<CommandLineOptions> result;
  CommandLineOptions options;
  const auto error = [&](const std::string &message) {
    result.errors.push_back({"command line", 0, 0, {}, message});
  };
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--config") {
      if (index + 1 == argc || std::string_view(argv[index + 1]).empty() ||
          std::string_view(argv[index + 1]).starts_with("--"))
        error("option requires a value: --config");
      else
        options.settingsPath = argv[++index];
    } else if (argument.starts_with("--config=")) {
      if (argument.size() == 9)
        error("option requires a value: --config");
      else
        options.settingsPath = argument.substr(9);
    } else if (argument == "--validate-config")
      options.validateOnly = true;
    else if (argument == "--help")
      options.showHelp = true;
    else if (argument == "--version")
      options.showVersion = true;
    else if (argument.starts_with('-'))
      error("unknown option: " + std::string(argument));
    else
      error("unexpected argument: " + std::string(argument));
  }
  if (result.errors.empty()) result.value = std::move(options);
  return result;
}
}  // namespace binlog_streamer
