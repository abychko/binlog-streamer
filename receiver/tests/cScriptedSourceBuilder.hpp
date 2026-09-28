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

#include "cScriptedSourcePayloads.hpp"
#include "protocol/cPacketFramer.hpp"
#include "receiver/hSessionDefaults.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace binlog_streamer::test {

class ScriptedSourceBuilder {
 public:
  void ResetSequence() { m_sequenceId = 0; }
  void SkipClientPacket() { ++m_sequenceId; }
  void Push(std::span<const std::uint8_t> payload) {
    PacketFramer::Encode(payload, m_sequenceId, m_bytes);
  }
  const std::vector<std::uint8_t> &Bytes() const { return m_bytes; }

  void AppendGreetingAndFastAuthSuccess(const std::string &serverVersion,
                                        std::uint32_t extraCapabilities = 0) {
    Push(ScriptedSourcePayloads::Greeting(
        serverVersion, CACHING_SHA2_PASSWORD_PLUGIN_NAME,
        ScriptedSourcePayloads::Scramble(), extraCapabilities));
    SkipClientPacket();                                // HandshakeResponse41
    Push(ScriptedSourcePayloads::AuthMoreData(0x03));  // fast_auth_success
    Push(ScriptedSourcePayloads::Ok(false));
  }

  void AppendSingleColumnRow(const std::string &columnName,
                             std::optional<std::string> value) {
    ResetSequence();
    SkipClientPacket();
    Push(ScriptedSourcePayloads::ColumnCount(1));
    Push(ScriptedSourcePayloads::ColumnDefinition(columnName));
    Push(ScriptedSourcePayloads::TextRow(std::move(value)));
    Push(ScriptedSourcePayloads::Ok(
        true));  // row terminator under CLIENT_DEPRECATE_EOF
  }

  void AppendSingleColumnError(std::uint16_t code, const std::string &message) {
    ResetSequence();
    SkipClientPacket();
    Push(ScriptedSourcePayloads::Err(code, message));
  }

  void AppendCommandOk() {
    ResetSequence();
    SkipClientPacket();
    Push(ScriptedSourcePayloads::Ok(false));
  }

  void AppendCommandError(std::uint16_t code, const std::string &message) {
    ResetSequence();
    SkipClientPacket();
    Push(ScriptedSourcePayloads::Err(code, message));
  }

  // Factored out so full-authentication scenarios, which skip
  // ThroughReplicaUuid()'s fixed prefix, can share it too.
  void AppendPreDumpQueries(const std::string &sourceServerId,
                            const std::string &gtidMode,
                            const std::string &sourceUuid) {
    AppendSingleColumnRow("UNIX_TIMESTAMP()", "1700000000");
    AppendSingleColumnRow("@@GLOBAL.SERVER_ID", sourceServerId);
    AppendCommandOk();  // heartbeat period
    AppendCommandOk();  // checksum SET
    AppendSingleColumnRow("@source_binlog_checksum", "CRC32");
    AppendSingleColumnRow("@@GLOBAL.GTID_MODE", gtidMode);
    AppendSingleColumnRow("@@GLOBAL.SERVER_UUID", sourceUuid);
    AppendCommandOk();  // @slave_uuid/@replica_uuid
  }

  // Appended after MySQL's perform_full_authentication plugin callback.
  void AppendFullAuthenticationExchange(
      bool requestsPublicKey, std::span<const std::uint8_t> publicKeyPem = {}) {
    if (requestsPublicKey) {
      SkipClientPacket();  // the 0x02 public key request
      Push(ScriptedSourcePayloads::AuthMoreData(publicKeyPem));
    }
    SkipClientPacket();  // the RSA-OAEP-encrypted password
    Push(ScriptedSourcePayloads::Ok(false));
  }

  // sourceServerId/gtidMode/sourceUuid let a caller fail one step while
  // keeping earlier ones successful.
  static ScriptedSourceBuilder ThroughReplicaUuid(
      const std::string &sourceServerId, const std::string &gtidMode = "ON",
      const std::string &sourceUuid = "11111111-1111-1111-1111-111111111111",
      const std::string &serverVersion = "8.4.11") {
    ScriptedSourceBuilder script;
    script.AppendGreetingAndFastAuthSuccess(serverVersion);
    script.AppendPreDumpQueries(sourceServerId, gtidMode, sourceUuid);
    return script;
  }

 private:
  std::uint8_t m_sequenceId = 0;
  std::vector<std::uint8_t> m_bytes;
};

}  // namespace binlog_streamer::test
