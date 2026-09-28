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

#include "config/cConfigurationReload.hpp"

#include <gtest/gtest.h>

namespace binlog_streamer {
namespace {

AddressRange Ipv4(std::uint8_t first, std::uint8_t prefixLength) {
  AddressRange range;
  range.address.family = AddressFamily::Ipv4;
  range.address.bytes[0] = first;
  range.prefixLength = prefixLength;
  return range;
}

Configuration Running() {
  Configuration configuration;
  configuration.settings.server.serverId = 1001;
  configuration.source.host = "source";
  configuration.replica.clients = {{"alice", "a", {Ipv4(127, 8)}},
                                   {"bob", "b", {Ipv4(10, 8)}}};
  return configuration;
}

TEST(ConfigurationReloadTest, SameConfigurationChangesNothing) {
  const ReloadOutcome outcome =
      ConfigurationReload::Compare(Running(), Running());
  EXPECT_EQ(outcome.clients, Running().replica.clients);
  EXPECT_EQ(outcome.changedHosts, 0u);
  EXPECT_TRUE(outcome.ignored.empty());
}

TEST(ConfigurationReloadTest, NewHostsOfARunningUserAreTaken) {
  Configuration loaded = Running();
  loaded.replica.clients[1].hosts = {Ipv4(10, 8), Ipv4(192, 8)};
  const ReloadOutcome outcome = ConfigurationReload::Compare(Running(), loaded);
  EXPECT_EQ(outcome.clients, loaded.replica.clients);
  EXPECT_EQ(outcome.changedHosts, 1u);
  EXPECT_TRUE(outcome.ignored.empty());
}

TEST(ConfigurationReloadTest, UsersAddedOrRemovedAreNotTaken) {
  Configuration loaded = Running();
  loaded.replica.clients = {{"bob", "b", {Ipv4(192, 8)}},
                            {"carol", "c", {Ipv4(127, 8)}}};
  const ReloadOutcome outcome = ConfigurationReload::Compare(Running(), loaded);
  ASSERT_EQ(outcome.clients.size(), 2u);
  EXPECT_EQ(outcome.clients[0], Running().replica.clients[0]);
  EXPECT_EQ(outcome.clients[1].hosts, loaded.replica.clients[0].hosts);
  EXPECT_EQ(outcome.changedHosts, 1u);
  EXPECT_EQ(outcome.ignored,
            (std::vector<std::string>{"replica.yml: user alice removed",
                                      "replica.yml: user carol added"}));
}

TEST(ConfigurationReloadTest, NewPasswordIsNotTakenButNewHostsAre) {
  Configuration loaded = Running();
  loaded.replica.clients[0].password = "changed";
  loaded.replica.clients[0].hosts = {Ipv4(192, 8)};
  const ReloadOutcome outcome = ConfigurationReload::Compare(Running(), loaded);
  EXPECT_EQ(outcome.clients[0].password, "a");
  EXPECT_EQ(outcome.clients[0].hosts, loaded.replica.clients[0].hosts);
  EXPECT_EQ(outcome.ignored,
            (std::vector<std::string>{"replica.yml: password of alice"}));
}

TEST(ConfigurationReloadTest, OtherChangesAreNamedAndNotTaken) {
  Configuration loaded = Running();
  loaded.settings.cache.maxSize = 1;
  loaded.source.port = 3307;
  loaded.replica.listenPort = 3308;
  loaded.replica.compression = CompressionAlgorithm::Zlib;
  loaded.replica.requireSecureTransport = true;
  loaded.replica.tls.certPem = "other";
  const ReloadOutcome outcome = ConfigurationReload::Compare(Running(), loaded);
  EXPECT_EQ(outcome.clients, Running().replica.clients);
  EXPECT_EQ(
      outcome.ignored,
      (std::vector<std::string>{
          "settings.yml", "source.yml", "replica.yml: listen_port",
          "replica.yml: compression", "replica.yml: require_secure_transport",
          "replica.yml: ssl_ca, ssl_cert, ssl_key"}));
}

}  // namespace
}  // namespace binlog_streamer
