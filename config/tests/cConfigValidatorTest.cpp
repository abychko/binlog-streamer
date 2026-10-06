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

#include "cConfigValidator.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

Settings ValidSettings() {
  Settings settings;
  settings.server.serverId = 1001;
  settings.storage.retention.policy = RetentionPolicy::Age;
  settings.storage.retention.period = std::chrono::seconds(7 * 86400);
  settings.storage.disk.maxSize = std::uint64_t{2} << 40;
  settings.storage.disk.purgeHighWatermark = std::uint64_t{1900} << 30;
  settings.storage.disk.purgeLowWatermark = std::uint64_t{1800} << 30;
  settings.storage.disk.minFreeSpace = std::uint64_t{50} << 30;
  settings.storage.disk.recoveryReserve = std::uint64_t{10} << 30;
  settings.cache.policy = CachePolicy::TimeWindow;
  settings.cache.window = std::chrono::hours(12);
  settings.cache.maxSize = std::uint64_t{1} << 40;
  return settings;
}

TEST(ConfigValidatorTest, ValidSettingsProduceNoErrors) {
  EXPECT_TRUE(
      ConfigValidator::Validate(ValidSettings(), {}, "settings.yml").empty());
}

TEST(ConfigValidatorTest, LowWatermarkNotBelowHighWatermarkIsError) {
  auto settings = ValidSettings();
  settings.storage.disk.purgeLowWatermark =
      settings.storage.disk.purgeHighWatermark;
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "storage.disk.purge_low_watermark");
}

TEST(ConfigValidatorTest, HighWatermarkPlusReserveOverMaxSizeIsError) {
  auto settings = ValidSettings();
  settings.storage.disk.recoveryReserve =
      settings.storage.disk.maxSize - settings.storage.disk.purgeHighWatermark +
      1;
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "storage.disk.purge_high_watermark");
}

TEST(ConfigValidatorTest, ZeroCacheMaxSizeIsError) {
  auto settings = ValidSettings();
  settings.cache.maxSize = 0;
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "cache.max_size");
}

TEST(ConfigValidatorTest, ZeroCacheWindowIsError) {
  auto settings = ValidSettings();
  settings.cache.window = std::chrono::seconds(0);
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "cache.window");
}

TEST(ConfigValidatorTest, ZeroRetentionPeriodIsError) {
  auto settings = ValidSettings();
  settings.storage.retention.period = std::chrono::seconds(0);
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "storage.retention.period");
}

TEST(ConfigValidatorTest, ZeroMinFreeSpaceIsError) {
  auto settings = ValidSettings();
  settings.storage.disk.minFreeSpace = 0;
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "storage.disk.min_free_space");
}

TEST(ConfigValidatorTest, ZeroRecoveryReserveIsError) {
  auto settings = ValidSettings();
  settings.storage.disk.recoveryReserve = 0;
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "storage.disk.recovery_reserve");
}

TEST(ConfigValidatorTest, ZeroPurgeLowWatermarkIsError) {
  auto settings = ValidSettings();
  settings.storage.disk.purgeLowWatermark = 0;
  const auto errors = ConfigValidator::Validate(settings, {}, "settings.yml");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].keyPath, "storage.disk.purge_low_watermark");
  EXPECT_EQ(errors[0].message, "must be greater than zero");
}

}  // namespace
}  // namespace binlog_streamer
