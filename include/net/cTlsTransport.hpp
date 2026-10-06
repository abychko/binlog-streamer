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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include "net/cTlsContext.hpp"
#include "net/eReadOutcome.hpp"
#include "net/iTransport.hpp"

struct ssl_st;
struct bio_st;

namespace binlog_streamer {

// Passes bytes through until the handshake: the greeting and the SSL request go
// in the clear.
class TlsTransport final : public Transport {
 public:
  explicit TlsTransport(Transport &inner) : m_inner(inner) {}
  ~TlsTransport() override;

  TlsTransport(const TlsTransport &) = delete;
  TlsTransport &operator=(const TlsTransport &) = delete;

  bool Handshake(const TlsContext &context, const std::string &host,
                 std::chrono::milliseconds timeout, std::string &error);
  bool Accept(const TlsContext &context, std::chrono::milliseconds timeout,
              std::string &error,
              std::span<const std::uint8_t> alreadyRead = {});

  bool Enabled() const { return m_ssl != nullptr; }
  std::string Cipher() const;
  std::string Version() const;

  bool Connect(const std::string &host, std::uint16_t port,
               std::chrono::milliseconds timeout, std::string &error) override;

  ReadOutcome Read(std::span<std::uint8_t> buffer, std::size_t &bytesRead,
                   std::chrono::milliseconds timeout,
                   std::string &error) override;

  bool Write(std::span<const std::uint8_t> data,
             std::chrono::milliseconds timeout, std::string &error) override;

  void Close() override;

 private:
  bool Begin(const TlsContext &context, std::string &error);
  bool RunHandshake(std::chrono::milliseconds timeout, std::string &error);
  bool Flush(std::chrono::milliseconds timeout, std::string &error);
  ReadOutcome Feed(std::chrono::milliseconds timeout, std::string &error);
  void Drop();

  Transport &m_inner;
  ssl_st *m_ssl = nullptr;
  bio_st *m_readBio = nullptr;  // owned by m_ssl once set
  bio_st *m_writeBio = nullptr;
};

}  // namespace binlog_streamer
