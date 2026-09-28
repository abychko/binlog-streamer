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

// Does not retry itself - StartupSequence owns the retry loop.
class ReplicaSession {
 public:
  // transport must already support being (re)connected. replicaUuid
  // must be the same value across attempts (generated once via
  // SessionUuid::Generate(), not here).
  ReplicaSession(Transport &transport, const SourceSettings &source,
                 const ServerSettings &server, std::string replicaUuid,
                 std::string relayName, std::string relayVersion,
                 ReplicaSessionOptions options = {});

  // Does not itself start a dump; that's StartDump(). Stops one step
  // earlier when options.registerAsReplica is false.
  SessionResult Run();

  // Only valid after Run() returned Registered (or CheckGtidMode() for
  // a probe) - calling it otherwise is undetected here.
  std::optional<SessionResult> StartDump(const GtidSet &gtidSet);

  // ok is false only for a transport failure or server-side ERR; a
  // NULL/empty result leaves value at nullopt but ok true.
  struct TextQueryResult {
    bool ok = false;
    std::optional<std::string> value;
    SessionResult failure;
  };
  TextQueryResult QueryText(std::string_view sql);

  // Same ok/failure split as QueryText(); columns is empty when there's
  // no matching row, not on failure.
  struct RowQueryResult {
    bool ok = false;
    std::vector<std::optional<std::string>> columns;
    SessionResult failure;
  };
  RowQueryResult QueryRow(std::string_view sql, std::size_t columnCount);

  std::uint8_t NextSequenceId() const { return m_channel.NextSequenceId(); }
  // The compressing decorator, not the socket: whoever reads the dump
  // after this session must read it through the same layer, and through
  // the same frame counter, the session left the connection on.
  Transport &transport() { return m_compressed; }
  // False until the source accepted a compression request, so a caller
  // reading the dump itself knows whether packet sequence ids inside
  // the stream are still meaningful.
  bool compressed() const { return m_compressed.Enabled(); }
  // True once the handshake switched the connection to TLS.
  bool encrypted() const { return m_tls.Enabled(); }
  // Empty unless encrypted(): "TLSv1.3 TLS_AES_256_GCM_SHA384".
  std::string tlsDescription() const {
    return encrypted() ? m_tls.Version() + " " + m_tls.Cipher() : "";
  }

 private:
  std::optional<SessionResult> Connect();
  std::optional<SessionResult> Handshake();
  std::optional<SessionResult> Authenticate();
  std::optional<SessionResult> CheckVersion();
  // (only a transport failure stops the session here)
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

  // Under the compressing layer: TLS encrypts the compressed frames, as
  // it does between a replica and its source. Both pass bytes through
  // untouched until the handshake turns them on, and back on every
  // reconnect.
  TlsTransport m_tls;
  CompressedTransport m_compressed;
  TlsContext m_tlsContext;  // loaded on the first handshake that needs it
  // Own the settings so their lifetime does not depend on the caller.
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
