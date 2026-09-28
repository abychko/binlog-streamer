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
#include <filesystem>
#include <fstream>
#include "http/cHtmlDirectory.hpp"

namespace binlog_streamer {
namespace {

class HtmlDirectoryTest : public ::testing::Test {
 protected:
  std::filesystem::path directory;

  void SetUp() override {
    directory = std::filesystem::temp_directory_path() /
                ("bs-html-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory / "sub");
    Write("index.html", "<html>status</html>\n");
    Write("app.js", "fetch('/status.json')\n");
    Write(".secret", "no\n");
    Write("sub/inner.html", "no\n");
  }
  void TearDown() override { std::filesystem::remove_all(directory); }

  void Write(const std::string &name, const std::string &content) {
    std::ofstream(directory / name) << content;
  }
};

TEST_F(HtmlDirectoryTest, ServesIndexForTheRootAndFilesByName) {
  const HtmlDirectory html(directory);
  const auto index = html.Serve("/");
  ASSERT_TRUE(index);
  EXPECT_EQ(index->status, 200);
  EXPECT_EQ(index->contentType, "text/html; charset=utf-8");
  EXPECT_EQ(index->body, "<html>status</html>\n");
  const auto script = html.Serve("/app.js");
  ASSERT_TRUE(script);
  EXPECT_EQ(script->contentType, "text/javascript; charset=utf-8");
  EXPECT_EQ(script->body, "fetch('/status.json')\n");
}

TEST_F(HtmlDirectoryTest, ServesNothingOutsideOneFlatDirectory) {
  const HtmlDirectory html(directory);
  for (const char *path :
       {"/missing.html", "/.secret", "/sub/inner.html", "/../index.html",
        "/sub", "/index.html?x", "index.html", "", "/index html"}) {
    EXPECT_FALSE(html.Serve(path)) << path;
  }
  const HtmlDirectory absent(directory / "nowhere");
  EXPECT_FALSE(absent.Serve("/"));
}

TEST_F(HtmlDirectoryTest, NamesTheTypeByExtension) {
  EXPECT_EQ(HtmlDirectory::ContentType("a.css"), "text/css; charset=utf-8");
  EXPECT_EQ(HtmlDirectory::ContentType("a.svg"), "image/svg+xml");
  EXPECT_EQ(HtmlDirectory::ContentType("a.png"), "image/png");
  EXPECT_EQ(HtmlDirectory::ContentType("a.ico"), "image/x-icon");
  EXPECT_EQ(HtmlDirectory::ContentType("a.json"), "application/json");
  EXPECT_EQ(HtmlDirectory::ContentType("a.bin"), "application/octet-stream");
  EXPECT_EQ(HtmlDirectory::ContentType("noext"), "application/octet-stream");
}

}  // namespace
}  // namespace binlog_streamer
