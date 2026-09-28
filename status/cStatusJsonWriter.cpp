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

#include "status/cStatusJsonWriter.hpp"

#include <cstdio>
#include <optional>
#include <string_view>
#include "status/hRelayStateNames.hpp"
#include "status/hReplicaStateNames.hpp"

namespace binlog_streamer {
namespace {

// Appends members one after another and puts the commas between them.
class JsonObject {
 public:
  explicit JsonObject(std::string &out) : m_out(out) { m_out += '{'; }
  ~JsonObject() { m_out += '}'; }
  JsonObject(const JsonObject &) = delete;
  JsonObject &operator=(const JsonObject &) = delete;

  void Key(std::string_view key) {
    if (!m_first) m_out += ',';
    m_first = false;
    String(m_out, key);
    m_out += ':';
  }
  void Member(std::string_view key, std::string_view value) {
    Key(key);
    String(m_out, value);
  }
  void Member(std::string_view key, std::uint64_t value) {
    Key(key);
    m_out += std::to_string(value);
  }
  void Member(std::string_view key, int value) {
    Key(key);
    m_out += std::to_string(value);
  }
  void Member(std::string_view key, bool value) {
    Key(key);
    m_out += value ? "true" : "false";
  }
  void Member(std::string_view key, std::optional<std::uint64_t> value) {
    Key(key);
    m_out += value ? std::to_string(*value) : "null";
  }
  // Empty text is null: a file name or a UUID not known yet.
  void MemberOrNull(std::string_view key, std::string_view value) {
    Key(key);
    if (value.empty())
      m_out += "null";
    else
      String(m_out, value);
  }

  static void String(std::string &out, std::string_view text) {
    out += '"';
    for (const char c : text) {
      switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x",
                          static_cast<unsigned>(c));
            out += escaped;
          } else {
            out += c;
          }
      }
    }
    out += '"';
  }

 private:
  std::string &m_out;
  bool m_first = true;
};

void WritePoint(JsonObject &object, const StreamPoint &point) {
  object.MemberOrNull("file", point.file);
  object.Member("position", point.position);
  object.Member("timestamp",
                point.timestamp == 0
                    ? std::optional<std::uint64_t>()
                    : std::optional<std::uint64_t>(point.timestamp));
}

}  // namespace

std::string StatusJsonWriter::Write(const RelayStatus &status) {
  std::string out;
  {
    JsonObject root(out);
    root.Member("name", status.name);
    root.Member("version", status.version);
    root.Member("state", RelayStateName(status.state));
    root.Member("state_code", RelayStateCode(status.state));
    root.Member("started_at", status.startedAt);
    root.Member("now", status.now);
    root.Member("uptime_seconds", status.uptimeSeconds);

    root.Key("source");
    {
      const SourceStatus &source = status.source;
      JsonObject object(out);
      object.Member("address", source.address);
      object.Member("connected", source.connected);
      object.Member("since", source.since);
      object.Member("attempt", static_cast<std::uint64_t>(source.attempt));
      object.Member("server_id", static_cast<std::uint64_t>(source.serverId));
      object.MemberOrNull("server_uuid", source.serverUuid);
      object.MemberOrNull("version", source.version);
      object.Member("tls", source.tls);
      object.MemberOrNull("compression", source.compression);
      WritePoint(object, source.seen);
      object.Member("caught_up", source.seen.idle);
      object.Member("clock", source.clock);
      object.Member("behind_seconds", source.behindSeconds);
    }

    root.Key("storage");
    {
      const StorageStatus &storage = status.storage;
      JsonObject object(out);
      object.Member("files", static_cast<std::uint64_t>(storage.files));
      object.Member("bytes", storage.bytes);
      object.Member("max_bytes", storage.maxBytes);
      object.MemberOrNull("file", storage.file);
      object.Member("position", storage.position);
      object.Member("behind_bytes", storage.behindBytes);
    }

    root.Key("memory");
    {
      const MemoryStatus &memory = status.memory;
      JsonObject object(out);
      object.Member("files", static_cast<std::uint64_t>(memory.files));
      object.Member("bytes", memory.bytes);
      object.Member("max_bytes", memory.maxBytes);
    }

    root.Key("replicas");
    out += '[';
    bool first = true;
    for (const ReplicaStatus &replica : status.replicas) {
      if (!first) out += ',';
      first = false;
      JsonObject object(out);
      object.Member("address", replica.facts.address);
      object.MemberOrNull("report_host", replica.reportHost);
      object.Member("user", replica.facts.user);
      object.MemberOrNull("program", replica.facts.program);
      object.MemberOrNull("version", replica.facts.version);
      object.Member("since", replica.since);
      object.Member("tls", replica.facts.tls);
      object.Member("compression", replica.facts.compression);
      object.Member("state", ReplicaStateName(replica.state));
      object.Member("state_code", ReplicaStateCode(replica.state));
      WritePoint(object, replica.sent);
      object.Member("behind_bytes", replica.behindBytes);
      object.Member("behind_seconds", replica.behindSeconds);
    }
    out += ']';
    root.Member("replicas_connected",
                static_cast<std::uint64_t>(status.replicas.size()));
    root.Member("max_connections",
                static_cast<std::uint64_t>(status.maxConnections));
  }
  out += '\n';
  return out;
}

}  // namespace binlog_streamer
