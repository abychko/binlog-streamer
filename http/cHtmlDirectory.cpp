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

#include "http/cHtmlDirectory.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace binlog_streamer {
namespace {

bool IsFileNameCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

}  // namespace

HtmlDirectory::HtmlDirectory(std::filesystem::path directory)
    : m_directory(std::move(directory)) {}

std::string_view HtmlDirectory::ContentType(std::string_view fileName) {
  const auto dot = fileName.rfind('.');
  const std::string_view extension =
      dot == std::string_view::npos ? std::string_view() : fileName.substr(dot);
  if (extension == ".html") return "text/html; charset=utf-8";
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js") return "text/javascript; charset=utf-8";
  if (extension == ".json") return "application/json";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".png") return "image/png";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

std::optional<HttpResponse> HtmlDirectory::Serve(std::string_view path) const {
  if (path.empty() || path[0] != '/') return std::nullopt;
  const std::string_view name = path == "/" ? "index.html" : path.substr(1);
  if (name.empty() || name[0] == '.') return std::nullopt;
  for (const char c : name)
    if (!IsFileNameCharacter(c)) return std::nullopt;
  const std::filesystem::path file_path = m_directory / name;
  std::error_code ignored;
  if (!std::filesystem::is_regular_file(file_path, ignored))
    return std::nullopt;
  std::ifstream file(file_path, std::ios::binary);
  if (!file) return std::nullopt;
  HttpResponse response;
  response.contentType = std::string(ContentType(name));
  response.body.assign(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
  if (file.bad()) return std::nullopt;
  return response;
}

}  // namespace binlog_streamer
