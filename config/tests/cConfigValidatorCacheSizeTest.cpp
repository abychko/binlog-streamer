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

#include "cConfigLoader.hpp"
#include "cConfigValidator.hpp"
#include "cache/hCacheDefaults.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

namespace binlog_streamer {
namespace {
TEST(ConfigCacheSizeTest,
     LoadingRejectsLessThanTwoSegmentsAndAcceptsExactMinimum) {
  std::ifstream input(std::string(BINLOG_STREAMER_PACKAGING_DIR) +
                      "/settings.yml");
  const std::string packaged((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  ASSERT_FALSE(packaged.empty());
  for (const auto size : {CACHE_SEGMENT_SIZE, 2 * CACHE_SEGMENT_SIZE}) {
    auto text = packaged;
    const auto cacheStart = text.find("\ncache:");
    ASSERT_NE(cacheStart, std::string::npos);
    const auto start =
        text.find("max_size:", cacheStart) + std::string("max_size:").size();
    const auto end = text.find('\n', start);
    text.replace(start, end - start,
                 " " + std::to_string(size / CACHE_SEGMENT_SIZE) + "M");
    const auto loaded = ConfigLoader::Parse(text, "settings.yml");
    const auto &errors = loaded.errors;
    const auto found = std::find_if(
        errors.begin(), errors.end(),
        [](const auto &e) { return e.keyPath == "cache.max_size"; });
    if (size == 2 * CACHE_SEGMENT_SIZE) {
      ASSERT_TRUE(loaded.value);
      EXPECT_EQ(found, errors.end());
      auto settings = *loaded.value;
      settings.cache.maxSize = 2 * CACHE_SEGMENT_SIZE - 1;
      const auto invalid =
          ConfigValidator::Validate(settings, {}, "settings.yml");
      ASSERT_EQ(invalid.size(), 1u);
      EXPECT_EQ(invalid.front().keyPath, "cache.max_size");
    } else {
      EXPECT_FALSE(loaded.value);
      ASSERT_NE(found, errors.end());
      EXPECT_NE(found->message.find("at least 2M"), std::string::npos);
    }
  }
}
}  // namespace
}  // namespace binlog_streamer
