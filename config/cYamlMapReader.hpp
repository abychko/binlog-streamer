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

#include <yaml-cpp/yaml.h>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "config/sConfigError.hpp"
#include "config/sKeyPosition.hpp"

namespace binlog_streamer {

// Internal adapter: application-facing headers never expose YAML types.
class YamlMapReader {
 public:
  YamlMapReader(std::string file, YAML::Node map, std::string prefix,
                std::vector<ConfigError> &errors,
                std::map<std::string, KeyPosition> &positions);
  static YAML::Node Document(const std::string &text, const std::string &file,
                             std::vector<ConfigError> &errors);
  bool IsMap() const;
  YAML::Node Required(std::string_view key);
  YAML::Node Optional(std::string_view key);
  std::optional<std::string> String(std::string_view key, bool required = true);
  // Like String(key, false), except a present-but-empty scalar is treated
  // as absent (nullopt, no error) - for template keys where an unfilled
  // value still means "use the default" (source_public_key_path, listen_port).
  std::optional<std::string> OptionalNonEmptyString(std::string_view key);
  std::optional<std::uint64_t> UnsignedInteger(std::string_view key,
                                               std::uint64_t minimum,
                                               std::uint64_t maximum,
                                               bool required = true);
  // UnsignedInteger's counterpart to OptionalNonEmptyString above.
  std::optional<std::uint64_t> OptionalNonEmptyUnsignedInteger(
      std::string_view key, std::uint64_t minimum, std::uint64_t maximum);
  std::optional<bool> Bool(std::string_view key, bool required = true);
  std::optional<std::uint64_t> ByteSize(std::string_view key);
  std::optional<std::chrono::seconds> Duration(std::string_view key);
  // Optional; absent is nullopt without an error.
  std::optional<std::chrono::microseconds> Delay(
      std::string_view key, std::chrono::microseconds maximum);
  template <class Table>
  auto Enumeration(std::string_view key, const Table &names)
      -> std::optional<typename Table::value_type::second_type> {
    return Lookup(key, String(key), names);
  }
  // Enumeration's counterpart to OptionalNonEmptyString above.
  template <class Table>
  auto OptionalNonEmptyEnumeration(std::string_view key, const Table &names)
      -> std::optional<typename Table::value_type::second_type> {
    return Lookup(key, OptionalNonEmptyString(key), names);
  }
  void Finish(std::initializer_list<std::string_view> allowedKeys);
  void Error(std::string_view key, const std::string &message);
  void ErrorAt(std::string_view path, const std::string &message,
               YAML::Mark mark);
  void Redirect(std::string_view key, const std::string &destination);
  std::string Path(std::string_view key) const;

 private:
  template <class Table>
  auto Lookup(std::string_view key, const std::optional<std::string> &text,
              const Table &names)
      -> std::optional<typename Table::value_type::second_type> {
    if (!text) return std::nullopt;
    std::string message = "supported values:";
    for (const auto &[name, value] : names) {
      if (*text == name) return value;
      message += " " + std::string(name);
    }
    Error(key, message);
    return std::nullopt;
  }

  // Shared by UnsignedInteger() and OptionalNonEmptyUnsignedInteger(): the
  // two differ only in how they obtain the source text (String(key,
  // required) vs. OptionalNonEmptyString(key)), not in how it is parsed.
  std::optional<std::uint64_t> ParseUnsignedInteger(
      std::string_view key, const std::optional<std::string> &text,
      std::uint64_t minimum, std::uint64_t maximum);
  std::string file_;
  YAML::Node map_;
  std::string prefix_;
  std::vector<ConfigError> &errors_;
  std::map<std::string, KeyPosition> &positions_;
};

}  // namespace binlog_streamer
