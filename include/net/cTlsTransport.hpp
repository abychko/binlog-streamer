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

// Keeps <openssl/ssl.h> out of this header.
struct ssl_st;
struct bio_st;

namespace binlog_streamer {

// Wraps a transport in TLS. Passes bytes through untouched until
// Connect()/Accept() ran the handshake, because the wire turns encrypted
// only after the SSL request that follows the greeting - the greeting
// itself, and the request, go in the clear.
//
// OpenSSL never touches the socket: it reads and writes a pair of memory
// BIOs, and this class moves bytes between them and the inner transport,
// so the inner transport's timeouts, stop flag and ReadOutcome apply to
// an encrypted connection exactly as they do to a plain one.
class TlsTransport final : public Transport {
 public:
  explicit TlsTransport(Transport &inner) : m_inner(inner) {}
  ~TlsTransport() override;

  TlsTransport(const TlsTransport &) = delete;
  TlsTransport &operator=(const TlsTransport &) = delete;

  // Client side: the peer has sent its greeting and read the SSL request.
  // host is sent as SNI and, under VerifyIdentity, checked against the
  // certificate.
  bool Handshake(const TlsContext &context, const std::string &host,
                 std::chrono::milliseconds timeout, std::string &error);
  // Server side: the SSL request has been read.
  // alreadyRead: bytes the caller took off the socket after the client's
  // SSL request, which are the start of the client's hello.
  bool Accept(const TlsContext &context, std::chrono::milliseconds timeout,
              std::string &error,
              std::span<const std::uint8_t> alreadyRead = {});

  bool Enabled() const { return m_ssl != nullptr; }
  // Empty until the handshake finished, e.g. "TLS_AES_256_GCM_SHA384".
  std::string Cipher() const;
  // Same, e.g. "TLSv1.3".
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
  // Sends whatever OpenSSL queued in the write BIO.
  bool Flush(std::chrono::milliseconds timeout, std::string &error);
  // One inner read into the read BIO.
  ReadOutcome Feed(std::chrono::milliseconds timeout, std::string &error);
  void Drop();

  Transport &m_inner;
  ssl_st *m_ssl = nullptr;
  bio_st *m_readBio = nullptr;   // owned by m_ssl once set
  bio_st *m_writeBio = nullptr;  // same
};

}  // namespace binlog_streamer
