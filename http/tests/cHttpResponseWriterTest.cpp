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

#include <gtest/gtest.h>
#include "http/cHttpResponseWriter.hpp"

namespace binlog_streamer {
namespace {

TEST(HttpResponseWriter, WritesStatusLineHeadersAndBody) {
  HttpResponse response;
  response.contentType = "application/json";
  response.body = "{}\n";
  EXPECT_EQ(HttpResponseWriter::Serialize(response, false),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 3\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{}\n");
}

TEST(HttpResponseWriter, HeadKeepsContentLengthAndDropsBody) {
  HttpResponse response;
  response.body = "hello\n";
  const std::string head = HttpResponseWriter::Serialize(response, true);
  EXPECT_NE(head.find("Content-Length: 6\r\n"), std::string::npos);
  EXPECT_TRUE(head.ends_with("\r\n\r\n"));
  EXPECT_EQ(head.find("hello"), std::string::npos);
}

TEST(HttpResponseWriter, NamesEveryStatusItSendsAndAllowsOn405) {
  EXPECT_EQ(HttpResponseWriter::ReasonPhrase(404), "Not Found");
  EXPECT_EQ(HttpResponseWriter::ReasonPhrase(408), "Request Timeout");
  EXPECT_EQ(HttpResponseWriter::ReasonPhrase(431),
            "Request Header Fields Too Large");
  EXPECT_EQ(HttpResponseWriter::ReasonPhrase(418), "Unknown");
  HttpResponse response;
  response.status = 405;
  const std::string out = HttpResponseWriter::Serialize(response, false);
  EXPECT_TRUE(out.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
  EXPECT_NE(out.find("\r\nAllow: GET, HEAD\r\n"), std::string::npos);
}

}  // namespace
}  // namespace binlog_streamer
