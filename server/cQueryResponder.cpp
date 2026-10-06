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

#include "cQueryResponder.hpp"

#include "server/hServerVersion.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace binlog_streamer {

namespace {

bool IsSpace(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

bool IsWordChar(char c) {
  const unsigned char byte = static_cast<unsigned char>(c);
  return std::isalnum(byte) != 0 || c == '_' || c == '$';
}

char UpperChar(char c) {
  return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  return text;
}

void SkipSpace(std::string_view &text) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
}

bool EqualsIgnoringCase(std::string_view text, std::string_view other) {
  if (text.size() != other.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i)
    if (UpperChar(text[i]) != UpperChar(other[i])) return false;
  return true;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

std::string Upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return UpperChar(c); });
  return out;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

// A keyword ends at a word boundary: SELECTED does not begin with SELECT, while
// "SELECT@@version" does.
bool TakeKeyword(std::string_view &text, std::string_view keyword) {
  std::string_view rest = text;
  SkipSpace(rest);
  if (rest.size() < keyword.size()) return false;
  if (!EqualsIgnoringCase(rest.substr(0, keyword.size()), keyword))
    return false;
  rest.remove_prefix(keyword.size());
  if (!rest.empty() && IsWordChar(rest.front())) return false;
  text = rest;
  return true;
}

// Quotes, backticks and parentheses hide what they enclose: the comma in SET @a
// = 'x, y' separates nothing.
std::size_t FindTopLevel(std::string_view text, char wanted) {
  char quote = 0;
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"' || c == '`')
      quote = c;
    else if (c == '(')
      ++depth;
    else if (c == ')' && depth > 0)
      --depth;
    else if (c == wanted && depth == 0)
      return i;
  }
  return std::string_view::npos;
}

std::vector<std::string_view> SplitTopLevel(std::string_view text,
                                            char separator) {
  std::vector<std::string_view> parts;
  for (;;) {
    const std::size_t at = FindTopLevel(text, separator);
    if (at == std::string_view::npos) break;
    parts.push_back(text.substr(0, at));
    text.remove_prefix(at + 1);
  }
  parts.push_back(text);
  return parts;
}

std::string_view TakeIdentifier(std::string_view &text) {
  SkipSpace(text);
  if (text.empty()) return {};
  const char opening = text.front();
  if (opening == '`' || opening == '\'' || opening == '"') {
    const std::size_t end = text.find(opening, 1);
    if (end == std::string_view::npos) return {};
    const std::string_view name = text.substr(1, end - 1);
    text.remove_prefix(end + 1);
    return name;
  }
  std::size_t length = 0;
  while (length < text.size() && IsWordChar(text[length])) ++length;
  const std::string_view name = text.substr(0, length);
  text.remove_prefix(length);
  return name;
}

struct VariableReference {
  // False when what follows "@@" is not a variable reference at all: an
  // unsupported statement, not an unknown variable.
  bool valid = false;
  std::string name;
};

// A qualifier other than GLOBAL/SESSION/LOCAL is read as part of the name, as a
// server reads @@foo.bar.
VariableReference ParseVariableReference(std::string_view reference) {
  std::string_view rest = reference;
  const std::string_view first = TakeIdentifier(rest);
  if (first.empty()) return {};
  std::string_view name = first;
  SkipSpace(rest);
  if (!rest.empty() && rest.front() == '.') {
    rest.remove_prefix(1);
    const std::string_view second = TakeIdentifier(rest);
    if (second.empty()) return {};
    const bool scoped = EqualsIgnoringCase(first, "GLOBAL") ||
                        EqualsIgnoringCase(first, "SESSION") ||
                        EqualsIgnoringCase(first, "LOCAL");
    name = scoped ? second : Trim(reference);
  }
  SkipSpace(rest);
  if (!rest.empty()) return {};

  VariableReference parsed;
  parsed.valid = true;
  parsed.name = std::string(name);
  return parsed;
}

// "SELECT @@version_comment LIMIT 1", as the command-line client asks on
// connect.
std::string_view StripTrailingLimitOne(std::string_view expressions) {
  std::string_view head = Trim(expressions);
  if (head.empty() || head.back() != '1') return expressions;
  head = Trim(head.substr(0, head.size() - 1));
  constexpr std::string_view LIMIT = "LIMIT";
  if (head.size() < LIMIT.size()) return expressions;
  if (!EqualsIgnoringCase(head.substr(head.size() - LIMIT.size()), LIMIT))
    return expressions;
  const std::string_view rest = head.substr(0, head.size() - LIMIT.size());
  if (!rest.empty() && IsWordChar(rest.back())) return expressions;
  return rest;
}

QueryResponse MakeError(std::uint16_t code, std::string sqlState,
                        std::string message) {
  QueryResponse response;
  response.kind = QueryResponseKind::Error;
  response.errorCode = code;
  response.sqlState = std::move(sqlState);
  response.message = std::move(message);
  return response;
}

// ER_NOT_SUPPORTED_YET for everything the scanners do not recognise, so a
// client learns which of its statements went unanswered.
QueryResponse NotSupported(std::string_view statement) {
  constexpr std::size_t QUOTED_LENGTH = 64;
  return MakeError(1235, "42000",
                   "This version of MySQL doesn't yet support '" +
                       std::string(statement.substr(0, QUOTED_LENGTH)) + "'");
}

// ER_UNKNOWN_SYSTEM_VARIABLE: a replica reads it as "source too old for this
// variable" and carries on; only a name read as a name gets it.
QueryResponse UnknownSystemVariable(const std::string &name) {
  return MakeError(1193, "HY000", "Unknown system variable '" + name + "'");
}

}  // namespace

QueryResponder::QueryResponder(ServerIdentity identity,
                               const ServerState *state)
    : m_identity(std::move(identity)), m_state(state) {}

QueryResponse QueryResponder::Respond(std::string_view sql,
                                      SessionVariables &session) const {
  std::string_view statement = Trim(sql);
  while (!statement.empty() && statement.back() == ';')
    statement = Trim(statement.substr(0, statement.size() - 1));

  std::string_view rest = statement;
  if (TakeKeyword(rest, "SELECT")) return Select(rest, statement, session);
  rest = statement;
  if (TakeKeyword(rest, "SET")) return Set(rest, statement, session);
  rest = statement;
  if (TakeKeyword(rest, "SHOW") && TakeKeyword(rest, "BINLOG") &&
      TakeKeyword(rest, "EVENTS") && TakeKeyword(rest, "IN"))
    return ShowPreviousGtids(rest, statement);
  return NotSupported(statement);
}

std::string QueryResponder::ServerVersion() const {
  return ServerVersionString(
      m_state != nullptr ? m_state->SourceVersion() : std::string(),
      m_identity.relayName, m_identity.relayVersion);
}

std::optional<std::string> QueryResponder::SystemVariable(
    std::string_view name) const {
  const std::string upper = Upper(name);
  if (upper == "SERVER_ID") return std::to_string(m_identity.serverId);
  if (upper == "SERVER_UUID") return m_identity.serverUuid;
  if (upper == "GTID_MODE") return std::string("ON");
  if (upper == "GTID_EXECUTED")
    return m_state != nullptr ? m_state->GtidExecuted() : std::string();
  if (upper == "GTID_PURGED")
    return m_state != nullptr ? m_state->GtidPurged() : std::string();
  if (upper == "BINLOG_CHECKSUM")
    return m_state != nullptr ? m_state->BinlogChecksum()
                              : std::string("CRC32");
  if (upper == "VERSION") return ServerVersion();
  if (upper == "VERSION_COMMENT") return m_identity.versionComment;
  return std::nullopt;
}

QueryResponse QueryResponder::Select(std::string_view expressions,
                                     std::string_view statement,
                                     const SessionVariables &session) const {
  QueryResponse response;
  response.kind = QueryResponseKind::Row;
  for (const std::string_view part :
       SplitTopLevel(StripTrailingLimitOne(expressions), ',')) {
    const std::string_view expression = Trim(part);
    if (expression.empty()) return NotSupported(statement);

    QueryColumn column{std::string(expression), std::nullopt};
    if (EqualsIgnoringCase(expression, "UNIX_TIMESTAMP()")) {
      column.value = std::to_string(static_cast<long long>(std::time(nullptr)));
    } else if (EqualsIgnoringCase(expression, "VERSION()")) {
      // mysqlbinlog reading from a remote server asks this first, to learn the
      // binary log format.
      column.value = ServerVersion();
    } else if (StartsWith(expression, "@@")) {
      const VariableReference reference =
          ParseVariableReference(expression.substr(2));
      if (!reference.valid) return NotSupported(statement);
      std::optional<std::string> value = SystemVariable(reference.name);
      if (!value) return UnknownSystemVariable(reference.name);
      column.value = std::move(value);
    } else if (expression.front() == '@') {
      std::string_view rest = expression.substr(1);
      const std::string_view name = TakeIdentifier(rest);
      if (name.empty() || !Trim(rest).empty()) return NotSupported(statement);
      column.value = session.Get(Lower(name));
    } else {
      return NotSupported(statement);
    }
    response.columns.push_back(std::move(column));
  }
  return response;
}

// "SHOW BINLOG EVENTS IN '<file>' LIMIT 1,1": the file's Previous_gtids; a
// relay whose source is this relay asks for it before its first dump.
QueryResponse QueryResponder::ShowPreviousGtids(
    std::string_view arguments, std::string_view statement) const {
  std::string_view rest = arguments;
  SkipSpace(rest);
  if (rest.empty() || rest.front() != '\'') return NotSupported(statement);
  const std::size_t end = rest.find('\'', 1);
  if (end == std::string_view::npos) return NotSupported(statement);
  const std::string fileName(rest.substr(1, end - 1));
  rest.remove_prefix(end + 1);

  if (!TakeKeyword(rest, "LIMIT")) return NotSupported(statement);
  std::string bounds;
  for (const char c : rest)
    if (!IsSpace(c)) bounds.push_back(c);
  if (bounds != "1,1") return NotSupported(statement);

  std::optional<PreviousGtidsEvent> event;
  if (m_state != nullptr) event = m_state->PreviousGtids(fileName);
  if (!event) {
    return MakeError(1220, "HY000",
                     "Error when executing command SHOW BINLOG EVENTS: Could "
                     "not find target log");
  }

  QueryResponse response;
  response.kind = QueryResponseKind::Row;
  response.columns.push_back(QueryColumn{"Log_name", fileName});
  response.columns.push_back(
      QueryColumn{"Pos", std::to_string(event->position)});
  response.columns.push_back(
      QueryColumn{"Event_type", std::string("Previous_gtids")});
  response.columns.push_back(
      QueryColumn{"Server_id", std::to_string(event->serverId)});
  response.columns.push_back(
      QueryColumn{"End_log_pos", std::to_string(event->endPosition)});
  response.columns.push_back(QueryColumn{"Info", event->gtids});
  return response;
}

QueryResponse QueryResponder::Set(std::string_view assignments,
                                  std::string_view statement,
                                  SessionVariables &session) const {
  std::string_view names = assignments;
  if (TakeKeyword(names, "NAMES")) return QueryResponse{};

  std::vector<std::pair<std::string, std::string>> parsed;
  for (const std::string_view part : SplitTopLevel(assignments, ',')) {
    std::string_view assignment = Trim(part);
    if (assignment.size() < 2 || assignment.front() != '@' ||
        assignment[1] == '@')
      return NotSupported(statement);
    assignment.remove_prefix(1);
    const std::string_view name = TakeIdentifier(assignment);
    if (name.empty()) return NotSupported(statement);

    SkipSpace(assignment);
    if (!assignment.empty() && assignment.front() == ':')
      assignment.remove_prefix(1);
    if (assignment.empty() || assignment.front() != '=')
      return NotSupported(statement);
    assignment.remove_prefix(1);

    const std::string_view text = Trim(assignment);
    if (text.empty()) return NotSupported(statement);
    std::string value;
    if (StartsWith(text, "@@")) {
      const VariableReference reference =
          ParseVariableReference(text.substr(2));
      if (!reference.valid) return NotSupported(statement);
      std::optional<std::string> resolved = SystemVariable(reference.name);
      if (!resolved) return UnknownSystemVariable(reference.name);
      value = std::move(*resolved);
    } else if (text.size() >= 2 && text.front() == '\'' &&
               text.back() == '\'') {
      value = std::string(text.substr(1, text.size() - 2));
    } else {
      value = std::string(text);
    }
    parsed.emplace_back(Lower(name), std::move(value));
  }
  // Nothing is stored unless the whole statement is understood.
  for (auto &[name, value] : parsed) session.Set(name, std::move(value));
  return QueryResponse{};
}

}  // namespace binlog_streamer
