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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "cSessionVariables.hpp"
#include "config/sIpAddress.hpp"
#include "config/sReplicaClient.hpp"
#include "eConnectionRefusal.hpp"
#include "net/cCompressedTransport.hpp"
#include "net/cPacketChannel.hpp"
#include "net/cTlsTransport.hpp"
#include "net/iTransport.hpp"
#include "protocol/hProtocolLimits.hpp"
#include "sConnectionServices.hpp"
#include "server/cReplicaClientList.hpp"
#include "server/hServerVersion.hpp"
#include "status/cReplicaRegistration.hpp"

namespace binlog_streamer {

struct QueryResponse;

// One instance per connection, run on its own thread.
class ReplicaConnection {
 public:
  // A callback rather than writing to std::cerr directly.
  using LogFunction = std::function<void(const std::string &line)>;

  // The 21-byte shape HandshakeV10Codec/AuthSwitchRequestCodec expect:
  // SCRAMBLE_LENGTH random bytes plus a trailing 0x00.
  using NonceGenerator =
      std::function<std::array<std::uint8_t, SCRAMBLE_LENGTH + 1>()>;

  // refusal: set when the listener already decided this session is over
  // before it starts; Run() still needs a channel/peer address to send
  // the ERR packet properly.

  // nonceGenerator: unset uses a real CSPRNG. Tests inject a fixed one,
  // since a login exchange's every later wire byte depends on the nonce
  // a scripted-transport test could not otherwise match.
  ReplicaConnection(Transport &transport, IpAddress peerAddress,
                    ReplicaClientList::Snapshot clients,
                    std::string serverVersion, LogFunction log,
                    ConnectionRefusal refusal = ConnectionRefusal::None,
                    NonceGenerator nonceGenerator = nullptr,
                    ConnectionServices services = {});

  // Never throws.
  void Run();

 private:
  bool CheckHost() const;
  const ReplicaClient *FindClient(const std::string &username) const;
  bool Login(std::string &username);
  void CommandLoop(const std::string &username);
  // True when the connection stays usable for another command, false when
  // it is finished or can no longer be written to.
  bool HandleDump(PacketChannel &channel,
                  const std::vector<std::uint8_t> &payload,
                  const std::string &username);
  bool SendQueryResponse(PacketChannel &channel, const QueryResponse &response);
  void SendErr(PacketChannel &channel, std::uint16_t code,
               std::string_view sqlState, std::string message);
  // Before the greeting nothing is negotiated, so no '#' + SQLSTATE
  // (sql/protocol_classic.cc, net_send_error_packet).
  void SendRefusal(PacketChannel &channel, std::uint16_t code,
                   std::string message);
  void Log(std::string_view event, const std::string &username,
           std::string_view detail);

  Transport &m_transport;
  // Both phases share these: the TLS state, the frame counter and the
  // compression state outlive Login()'s channel, which the command phase
  // replaces. TLS sits under compression, as it does in a server.
  TlsTransport m_tls;
  CompressedTransport m_compressed;
  IpAddress m_peerAddress;
  std::string m_peerAddressText;
  ReplicaClientList::Snapshot m_clients;
  std::string m_serverVersion;
  LogFunction m_log;
  ConnectionRefusal m_refusal;
  NonceGenerator m_nonceGenerator;
  ConnectionServices m_services;
  std::uint32_t m_registeredServerId =
      0;  // from COM_REGISTER_SLAVE, 0 until then
  std::uint32_t m_clientCapabilities = 0;
  SessionVariables m_sessionVariables;
  // From a successful login to the end of Run(); null without a tracker.
  std::shared_ptr<ReplicaRegistration> m_registration;
};

}  // namespace binlog_streamer
