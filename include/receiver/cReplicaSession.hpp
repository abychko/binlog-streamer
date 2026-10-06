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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "config/sServerSettings.hpp"
#include "config/sSourceSettings.hpp"
#include "gtid/cGtidSet.hpp"
#include "net/cCompressedTransport.hpp"
#include "net/cPacketChannel.hpp"
#include "net/cTlsContext.hpp"
#include "net/cTlsTransport.hpp"
#include "net/iTransport.hpp"
#include "receiver/sReplicaSessionOptions.hpp"
#include "receiver/sSessionResult.hpp"

namespace binlog_streamer {

class ReplicaSession {
 public:
  ReplicaSession(Transport &transport, const SourceSettings &source,
                 const ServerSettings &server, std::string replicaUuid,
                 std::string relayName, std::string relayVersion,
                 ReplicaSessionOptions options = {});

  SessionResult Run();

  // Valid only after Run() returned Registered (or CheckGtidMode() for a
  // probe); not checked.
  std::optional<SessionResult> StartDump(const GtidSet &gtidSet);

  // ok is false only on a transport failure or server ERR; a NULL or empty
  // result is ok with value nullopt.
  struct TextQueryResult {
    bool ok = false;
    std::optional<std::string> value;
    SessionResult failure;
  };
  TextQueryResult QueryText(std::string_view sql);

  struct RowQueryResult {
    bool ok = false;
    std::vector<std::optional<std::string>> columns;
    SessionResult failure;
  };
  RowQueryResult QueryRow(std::string_view sql, std::size_t columnCount);

  std::uint8_t NextSequenceId() const { return m_channel.NextSequenceId(); }
  // The compressing decorator, not the socket: read the dump through it, with
  // the same frame counter.
  Transport &transport() { return m_compressed; }
  // False until the source accepted compression; sequence ids inside the dump
  // stream are meaningful only then.
  bool compressed() const { return m_compressed.Enabled(); }
  bool encrypted() const { return m_tls.Enabled(); }
  std::string tlsDescription() const {
    return encrypted() ? m_tls.Version() + " " + m_tls.Cipher() : "";
  }

 private:
  std::optional<SessionResult> Connect();
  std::optional<SessionResult> Handshake();
  std::optional<SessionResult> Authenticate();
  std::optional<SessionResult> CheckVersion();
  std::optional<SessionResult> ReportClockSkew();
  std::optional<SessionResult> CheckServerId();
  std::optional<SessionResult> SetHeartbeatPeriod();
  std::optional<SessionResult> NegotiateChecksum();
  std::optional<SessionResult> CheckGtidMode();
  std::optional<SessionResult> CheckServerUuid();
  std::optional<SessionResult> SetReplicaUuid();
  SessionResult Register();
  SessionResult ReadyWithoutRegistering();

  SessionResult Transient(std::string message) const;
  SessionResult Permanent(std::string message) const;

  TlsTransport m_tls;
  CompressedTransport m_compressed;
  TlsContext m_tlsContext;
  const SourceSettings m_source;
  const ServerSettings m_server;
  std::string m_replicaUuid;
  std::string m_relayName;
  std::string m_relayVersion;
  ReplicaSessionOptions m_options;
  PacketChannel m_channel;
  std::string m_authPluginName;
  // greeting's scramble/nonce, until superseded by an AuthSwitchRequest
  std::vector<std::uint8_t> m_authPluginData;
  SourceIdentity m_identity;
};

}  // namespace binlog_streamer
