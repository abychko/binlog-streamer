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

#include "http/cHttpRequestParser.hpp"

namespace binlog_streamer {

bool HttpRequestParser::Parse(std::string_view head, HttpRequest &request,
                              std::string &error) {
  std::string_view line = head.substr(0, head.find("\r\n"));
  const auto firstSpace = line.find(' ');
  const auto lastSpace = line.rfind(' ');
  if (firstSpace == std::string_view::npos || firstSpace == 0 ||
      lastSpace == firstSpace || lastSpace + 1 == line.size()) {
    error = "malformed request line";
    return false;
  }
  const std::string_view method = line.substr(0, firstSpace);
  const std::string_view target =
      line.substr(firstSpace + 1, lastSpace - firstSpace - 1);
  const std::string_view version = line.substr(lastSpace + 1);
  if (target.empty() || target.find(' ') != std::string_view::npos) {
    error = "malformed request line";
    return false;
  }
  if (!version.starts_with("HTTP/1.")) {
    error = "unsupported protocol version";
    return false;
  }
  if (target[0] != '/') {
    error = "request target is not a path";
    return false;
  }
  const auto question = target.find('?');
  request.method = std::string(method);
  request.path = std::string(target.substr(0, question));
  request.query = question == std::string_view::npos
                      ? std::string()
                      : std::string(target.substr(question + 1));
  request.version = std::string(version);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
