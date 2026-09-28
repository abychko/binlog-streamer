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
#include "http/cHttpRequestParser.hpp"

namespace binlog_streamer {
namespace {

TEST(HttpRequestParser, SplitsRequestLineIntoMethodPathQueryAndVersion) {
  HttpRequest request;
  std::string error;
  ASSERT_TRUE(HttpRequestParser::Parse(
      "GET /status.json?pretty=1 HTTP/1.1\r\nHost: relay\r\n", request, error))
      << error;
  EXPECT_EQ(request.method, "GET");
  EXPECT_EQ(request.path, "/status.json");
  EXPECT_EQ(request.query, "pretty=1");
  EXPECT_EQ(request.version, "HTTP/1.1");
}

TEST(HttpRequestParser, AcceptsRequestLineAloneAndHttp10) {
  HttpRequest request;
  std::string error;
  ASSERT_TRUE(HttpRequestParser::Parse("HEAD / HTTP/1.0", request, error))
      << error;
  EXPECT_EQ(request.method, "HEAD");
  EXPECT_EQ(request.path, "/");
  EXPECT_TRUE(request.query.empty());
  EXPECT_EQ(request.version, "HTTP/1.0");
}

TEST(HttpRequestParser, RefusesWhatIsNotAnHttpRequestLine) {
  HttpRequest request;
  std::string error;
  for (const char *head :
       {"", "GET", "GET /", "GET  HTTP/1.1", " GET / HTTP/1.1",
        "GET / HTTP/1.1 ", "GET / a b HTTP/1.1"}) {
    EXPECT_FALSE(HttpRequestParser::Parse(head, request, error)) << head;
    EXPECT_EQ(error, "malformed request line") << head;
  }
  EXPECT_FALSE(HttpRequestParser::Parse("GET / HTTP/2", request, error));
  EXPECT_EQ(error, "unsupported protocol version");
  EXPECT_FALSE(HttpRequestParser::Parse("GET / SSH-2.0", request, error));
  EXPECT_EQ(error, "unsupported protocol version");
  EXPECT_FALSE(
      HttpRequestParser::Parse("GET http://relay/ HTTP/1.1", request, error));
  EXPECT_EQ(error, "request target is not a path");
  EXPECT_FALSE(HttpRequestParser::Parse("OPTIONS * HTTP/1.1", request, error));
  EXPECT_EQ(error, "request target is not a path");
}

}  // namespace
}  // namespace binlog_streamer
