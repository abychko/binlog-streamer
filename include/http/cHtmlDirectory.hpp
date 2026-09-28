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

#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include "http/sHttpResponse.hpp"

namespace binlog_streamer {

// The files of the status page, served from one directory: "/" is its
// index.html, "/name.ext" the file of that name. No subdirectories, no
// dot files, nothing outside the directory; the file is read at every
// request, since an edit is meant to show at the next reload.
class HtmlDirectory {
 public:
  explicit HtmlDirectory(std::filesystem::path directory);

  // nullopt for a path this does not serve or a file that is not there.
  std::optional<HttpResponse> Serve(std::string_view path) const;

  // By the extension; "application/octet-stream" for one not known.
  static std::string_view ContentType(std::string_view fileName);

 private:
  std::filesystem::path m_directory;
};

}  // namespace binlog_streamer
