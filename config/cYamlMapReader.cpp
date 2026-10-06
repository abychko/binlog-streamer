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

#include "cYamlMapReader.hpp"
#include "cByteSizeParser.hpp"
#include "cDurationParser.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <system_error>
#include <utility>

namespace binlog_streamer {
YamlMapReader::YamlMapReader(std::string file, YAML::Node map,
                             std::string prefix,
                             std::vector<ConfigError> &errors,
                             std::map<std::string, KeyPosition> &positions)
    : file_(std::move(file)),
      map_(std::move(map)),
      prefix_(std::move(prefix)),
      errors_(errors),
      positions_(positions) {
  // yaml-cpp's Mark(int, int, int) constructor is private; the default one
  // gives the same (0, 0, 0).
  if (!map_.IsMap())
    ErrorAt(prefix_, "expected a mapping",
            map_.Mark().is_null() ? YAML::Mark{} : map_.Mark());
}
YAML::Node YamlMapReader::Document(const std::string &text,
                                   const std::string &file,
                                   std::vector<ConfigError> &errors) {
  try {
    const auto documents = YAML::LoadAll(text);
    if (documents.size() > 1) {
      const auto mark = documents[1].Mark();
      errors.push_back({file,
                        mark.is_null() ? 0 : mark.line + 1,
                        mark.is_null() ? 0 : mark.column + 1,
                        {},
                        "expected a single YAML document"});
      return {};
    }
    // LoadAll yields zero documents for empty or comment-only text; normalize
    // to Null as YAML::Load does.
    return documents.empty() ? YAML::Node(YAML::NodeType::Null)
                             : documents.front();
  } catch (const YAML::Exception &error) {
    errors.push_back({file,
                      error.mark.is_null() ? 0 : error.mark.line + 1,
                      error.mark.is_null() ? 0 : error.mark.column + 1,
                      {},
                      "YAML syntax error"});
    return {};
  }
}
bool YamlMapReader::IsMap() const { return map_.IsMap(); }
std::string YamlMapReader::Path(std::string_view key) const {
  return prefix_.empty() ? std::string(key) : prefix_ + "." + std::string(key);
}
void YamlMapReader::ErrorAt(std::string_view path, const std::string &message,
                            YAML::Mark mark) {
  errors_.push_back({file_, mark.is_null() ? 0 : mark.line + 1,
                     mark.is_null() ? 0 : mark.column + 1, std::string(path),
                     message});
}
YAML::Node YamlMapReader::Optional(std::string_view key) {
  if (IsMap()) {
    for (const auto &entry : map_) {
      if (entry.first.IsScalar() && entry.first.Scalar() == key) {
        const auto mark = entry.first.Mark();
        positions_.try_emplace(
            Path(key), mark.is_null()
                           ? KeyPosition{}
                           : KeyPosition{mark.line + 1, mark.column + 1});
        return entry.second;
      }
    }
  }
  return YAML::Node(YAML::NodeType::Undefined);
}
YAML::Node YamlMapReader::Required(std::string_view key) {
  auto node = Optional(key);
  if (!node.IsDefined() && IsMap())
    ErrorAt(Path(key), "required key is missing", map_.Mark());
  return node;
}
void YamlMapReader::Error(std::string_view key, const std::string &message) {
  const auto found = positions_.find(Path(key));
  if (found != positions_.end()) {
    errors_.push_back(
        {file_, found->second.line, found->second.column, Path(key), message});
  } else
    ErrorAt(Path(key), message, map_.Mark());
}
void YamlMapReader::Finish(
    std::initializer_list<std::string_view> allowedKeys) {
  if (!IsMap()) return;
  std::set<std::string> seen;
  for (const auto &entry : map_) {
    if (!entry.first.IsScalar() ||
        std::find(allowedKeys.begin(), allowedKeys.end(),
                  entry.first.Scalar()) == allowedKeys.end()) {
      ErrorAt(prefix_, "unknown key", entry.first.Mark());
      continue;
    }
    const auto &name = entry.first.Scalar();
    if (!seen.insert(name).second)
      ErrorAt(Path(name), "duplicate key", entry.first.Mark());
  }
}
std::optional<std::string> YamlMapReader::String(std::string_view key,
                                                 bool required) {
  const auto node = required ? Required(key) : Optional(key);
  if (!node.IsDefined()) return std::nullopt;
  if (node.IsNull() || (node.IsScalar() && node.Scalar().empty())) {
    Error(key, "value is empty");
    return std::nullopt;
  }
  if (!node.IsScalar()) {
    Error(key, "wrong type");
    return std::nullopt;
  }
  return node.Scalar();
}
std::optional<std::string> YamlMapReader::OptionalNonEmptyString(
    std::string_view key) {
  const auto node = Optional(key);
  if (!node.IsDefined() || node.IsNull() ||
      (node.IsScalar() && node.Scalar().empty()))
    return std::nullopt;
  if (!node.IsScalar()) {
    Error(key, "wrong type");
    return std::nullopt;
  }
  return node.Scalar();
}
std::optional<std::uint64_t> YamlMapReader::ParseUnsignedInteger(
    std::string_view key, const std::optional<std::string> &text,
    std::uint64_t minimum, std::uint64_t maximum) {
  if (!text) return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, status] =
      std::from_chars(text->data(), text->data() + text->size(), value);
  if (text->find_first_not_of("0123456789") != std::string::npos ||
      status != std::errc{} || end != text->data() + text->size() ||
      value < minimum || value > maximum) {
    Error(key, "expected an integer in range " + std::to_string(minimum) +
                   ".." + std::to_string(maximum));
    return std::nullopt;
  }
  return value;
}
std::optional<std::uint64_t> YamlMapReader::UnsignedInteger(
    std::string_view key, std::uint64_t minimum, std::uint64_t maximum,
    bool required) {
  return ParseUnsignedInteger(key, String(key, required), minimum, maximum);
}
std::optional<std::uint64_t> YamlMapReader::OptionalNonEmptyUnsignedInteger(
    std::string_view key, std::uint64_t minimum, std::uint64_t maximum) {
  return ParseUnsignedInteger(key, OptionalNonEmptyString(key), minimum,
                              maximum);
}
std::optional<bool> YamlMapReader::Bool(std::string_view key, bool required) {
  const auto text = String(key, required);
  if (!text) return std::nullopt;
  if (*text == "true") return true;
  if (*text == "false") return false;
  Error(key, "expected true or false");
  return std::nullopt;
}
std::optional<std::uint64_t> YamlMapReader::ByteSize(std::string_view key) {
  const auto text = String(key);
  if (!text) return std::nullopt;
  std::uint64_t value = 0;
  std::string error;
  if (!ByteSizeParser::Parse(*text, value, error)) {
    Error(key, error);
    return std::nullopt;
  }
  return value;
}
std::optional<std::chrono::seconds> YamlMapReader::Duration(
    std::string_view key) {
  const auto text = String(key);
  if (!text) return std::nullopt;
  std::chrono::seconds value{};
  std::string error;
  if (!DurationParser::Parse(*text, value, error)) {
    Error(key, error);
    return std::nullopt;
  }
  return value;
}
std::optional<std::chrono::microseconds> YamlMapReader::Delay(
    std::string_view key, std::chrono::microseconds maximum) {
  const auto text = String(key, /*required=*/false);
  if (!text) return std::nullopt;
  std::chrono::microseconds value{};
  std::string error;
  if (!DurationParser::ParseDelay(*text, value, error)) {
    Error(key, error);
    return std::nullopt;
  }
  if (value > maximum) {
    Error(key, "at most " + std::to_string(maximum.count()) + "us");
    return std::nullopt;
  }
  return value;
}
void YamlMapReader::Redirect(std::string_view key,
                             const std::string &destination) {
  if (Optional(key).IsDefined()) Error(key, "belongs to " + destination);
}
}  // namespace binlog_streamer
