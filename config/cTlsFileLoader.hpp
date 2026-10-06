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

#include <map>
#include <string>
#include <vector>
#include "config/iProtectedFileReader.hpp"
#include "config/sConfigError.hpp"
#include "config/sKeyPosition.hpp"
#include "net/sTlsMaterial.hpp"

namespace binlog_streamer {

class TlsFileLoader {
 public:
  TlsFileLoader(ProtectedFileReader &reader, std::string expectedOwner);

  // An empty path means the key is not set. True when every named file was read
  // and the material loads; material is filled only then.
  bool Read(const std::string &fileName,
            const std::map<std::string, KeyPosition> &positions,
            const std::string &caPath, const std::string &certPath,
            const std::string &keyPath, TlsMaterial &material,
            std::vector<ConfigError> &errors);

 private:
  ProtectedFileReader &reader_;
  std::string expectedOwner_;
};

}  // namespace binlog_streamer
