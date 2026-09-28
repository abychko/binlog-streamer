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

#include "config/cConfigErrorPrinter.hpp"

#include <gtest/gtest.h>
#include <sstream>

namespace binlog_streamer {
namespace {

TEST(ConfigErrorPrinterTest, PrintsPositionAndKey) {
  std::ostringstream output;
  ConfigErrorPrinter::Print(
      output,
      {{"settings.yml", 3, 5, "storage.disk.max_size", "value is empty"}});
  EXPECT_EQ(output.str(),
            "binlog-streamer: settings.yml:3:5: storage.disk.max_size: value "
            "is empty\n");
}

TEST(ConfigErrorPrinterTest, OmitsPositionWhenLineIsZero) {
  std::ostringstream output;
  ConfigErrorPrinter::Print(
      output,
      {{"settings.yml", 0, 0, "storage.disk.max_size", "value is empty"}});
  EXPECT_EQ(
      output.str(),
      "binlog-streamer: settings.yml: storage.disk.max_size: value is empty\n");
}

TEST(ConfigErrorPrinterTest, OmitsKeyWhenEmpty) {
  std::ostringstream output;
  ConfigErrorPrinter::Print(
      output, {{"settings.yml", 0, 0, "", "No such file or directory"}});
  EXPECT_EQ(output.str(),
            "binlog-streamer: settings.yml: No such file or directory\n");
}

TEST(ConfigErrorPrinterTest, PrintsMultipleErrorsOnePerLine) {
  std::ostringstream output;
  ConfigErrorPrinter::Print(
      output,
      {
          {"settings.yml", 1, 1, "server.server_id", "required key is missing"},
          {"settings.yml", 2, 3, "storage.disk", "unknown key"},
      });
  EXPECT_EQ(output.str(),
            "binlog-streamer: settings.yml:1:1: server.server_id: required key "
            "is missing\n"
            "binlog-streamer: settings.yml:2:3: storage.disk: unknown key\n");
}

}  // namespace
}  // namespace binlog_streamer
