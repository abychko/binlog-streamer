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

#include "http/cHttpResponseWriter.hpp"

namespace binlog_streamer {

std::string_view HttpResponseWriter::ReasonPhrase(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    default:
      return "Unknown";
  }
}

std::string HttpResponseWriter::Serialize(const HttpResponse &response,
                                          bool headOnly) {
  std::string out = "HTTP/1.1 " + std::to_string(response.status) + ' ';
  out += ReasonPhrase(response.status);
  out += "\r\nContent-Type: " + response.contentType;
  out += "\r\nContent-Length: " + std::to_string(response.body.size());
  // The state changes with every poll: nothing in between may keep a copy.
  out += "\r\nCache-Control: no-store";
  if (response.status == 405) out += "\r\nAllow: GET, HEAD";
  out += "\r\nConnection: close\r\n\r\n";
  if (!headOnly) out += response.body;
  return out;
}

}  // namespace binlog_streamer
