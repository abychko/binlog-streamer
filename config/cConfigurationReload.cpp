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

#include <algorithm>
#include <utility>

namespace binlog_streamer {
namespace {

const ReplicaClient *FindUser(const std::vector<ReplicaClient> &clients,
                              const std::string &user) {
  const auto found =
      std::find_if(clients.begin(), clients.end(),
                   [&user](const ReplicaClient &c) { return c.user == user; });
  return found != clients.end() ? &*found : nullptr;
}

void CompareReplica(const ReplicaSettings &running,
                    const ReplicaSettings &loaded,
                    std::vector<std::string> &ignored) {
  if (running.listenAddress != loaded.listenAddress)
    ignored.push_back("replica.yml: listen_address");
  if (running.listenPort != loaded.listenPort)
    ignored.push_back("replica.yml: listen_port");
  if (running.compression != loaded.compression)
    ignored.push_back("replica.yml: compression");
  if (running.requireSecureTransport != loaded.requireSecureTransport)
    ignored.push_back("replica.yml: require_secure_transport");
  if (running.sslCa != loaded.sslCa || running.sslCert != loaded.sslCert ||
      running.sslKey != loaded.sslKey || running.tls != loaded.tls)
    ignored.push_back("replica.yml: ssl_ca, ssl_cert, ssl_key");
}

}  // namespace

ReloadOutcome ConfigurationReload::Compare(const Configuration &running,
                                           const Configuration &loaded) {
  ReloadOutcome outcome;
  if (running.settings != loaded.settings)
    outcome.ignored.push_back("settings.yml");
  if (running.source != loaded.source) outcome.ignored.push_back("source.yml");
  CompareReplica(running.replica, loaded.replica, outcome.ignored);

  for (const ReplicaClient &client : running.replica.clients) {
    ReplicaClient next = client;
    const ReplicaClient *reloaded =
        FindUser(loaded.replica.clients, client.user);
    if (reloaded == nullptr) {
      outcome.ignored.push_back("replica.yml: user " + client.user +
                                " removed");
    } else {
      if (reloaded->password != client.password)
        outcome.ignored.push_back("replica.yml: password of " + client.user);
      if (reloaded->hosts != client.hosts) {
        next.hosts = reloaded->hosts;
        ++outcome.changedHosts;
      }
    }
    outcome.clients.push_back(std::move(next));
  }
  for (const ReplicaClient &client : loaded.replica.clients)
    if (FindUser(running.replica.clients, client.user) == nullptr)
      outcome.ignored.push_back("replica.yml: user " + client.user + " added");
  return outcome;
}

}  // namespace binlog_streamer
