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

#include "net/cTlsTransport.hpp"

#include <arpa/inet.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>

namespace binlog_streamer {
namespace {

constexpr std::size_t CHUNK_SIZE = 16 * 1024;

constexpr std::size_t MAX_RECORD_SIZE =
    SSL3_RT_HEADER_LENGTH + SSL3_RT_MAX_ENCRYPTED_LENGTH;

// Close() has no caller timeout; close_notify is best effort.
constexpr std::chrono::milliseconds CLOSE_TIMEOUT{1000};

std::string OpenSslError(const std::string &what) {
  const unsigned long code = ERR_peek_last_error();
  char text[256];
  if (code == 0) return what;
  ERR_error_string_n(code, text, sizeof(text));
  ERR_clear_error();
  return what + ": " + text;
}

bool IsIpAddress(const std::string &host) {
  unsigned char bytes[16];
  return inet_pton(AF_INET, host.c_str(), bytes) == 1 ||
         inet_pton(AF_INET6, host.c_str(), bytes) == 1;
}

const char *OutcomeText(ReadOutcome outcome) {
  switch (outcome) {
    case ReadOutcome::TimedOut:
      return "timed out";
    case ReadOutcome::Interrupted:
      return "interrupted";
    case ReadOutcome::Closed:
      return "connection closed";
    case ReadOutcome::Data:
    case ReadOutcome::Failed:
      break;
  }
  return "";
}

}  // namespace

TlsTransport::~TlsTransport() { Drop(); }

void TlsTransport::Drop() {
  if (m_ssl != nullptr) SSL_free(m_ssl);
  m_ssl = nullptr;
  m_readBio = nullptr;
  m_writeBio = nullptr;
}

bool TlsTransport::Begin(const TlsContext &context, std::string &error) {
  Drop();
  if (!context.Loaded()) {
    error = "TLS context not loaded";
    return false;
  }
  ERR_clear_error();
  m_ssl = SSL_new(context.Raw());
  m_readBio = BIO_new(BIO_s_mem());
  m_writeBio = BIO_new(BIO_s_mem());
  if (m_ssl == nullptr || m_readBio == nullptr || m_writeBio == nullptr) {
    if (m_readBio != nullptr) BIO_free(m_readBio);
    if (m_writeBio != nullptr) BIO_free(m_writeBio);
    Drop();
    error = OpenSslError("creating the TLS connection");
    return false;
  }
  // An empty read BIO means nothing arrived yet, never end of stream.
  BIO_set_mem_eof_return(m_readBio, -1);
  SSL_set_bio(m_ssl, m_readBio, m_writeBio);
  return true;
}

bool TlsTransport::Handshake(const TlsContext &context, const std::string &host,
                             std::chrono::milliseconds timeout,
                             std::string &error) {
  if (!Begin(context, error)) return false;
  SSL_set_connect_state(m_ssl);
  const bool byName = !IsIpAddress(host);
  // SNI names a host, never an address (RFC 6066, 3).
  if (byName) SSL_set_tlsext_host_name(m_ssl, host.c_str());
  if (context.mode() == SslMode::VerifyIdentity) {
    const int set = byName ? SSL_set1_host(m_ssl, host.c_str())
                           : X509_VERIFY_PARAM_set1_ip_asc(
                                 SSL_get0_param(m_ssl), host.c_str());
    if (set != 1) {
      error = OpenSslError("setting the host to verify");
      Drop();
      return false;
    }
  }
  return RunHandshake(timeout, error);
}

bool TlsTransport::Accept(const TlsContext &context,
                          std::chrono::milliseconds timeout, std::string &error,
                          std::span<const std::uint8_t> alreadyRead) {
  if (!Begin(context, error)) return false;
  SSL_set_accept_state(m_ssl);
  if (!alreadyRead.empty())
    BIO_write(m_readBio, alreadyRead.data(),
              static_cast<int>(alreadyRead.size()));
  return RunHandshake(timeout, error);
}

bool TlsTransport::RunHandshake(std::chrono::milliseconds timeout,
                                std::string &error) {
  for (;;) {
    ERR_clear_error();
    const int result = SSL_do_handshake(m_ssl);
    const int reason =
        result == 1 ? SSL_ERROR_NONE : SSL_get_error(m_ssl, result);
    if (!Flush(timeout, error)) {
      error = "TLS handshake: " + error;
      Drop();
      return false;
    }
    if (result == 1) {
      error.clear();
      return true;
    }
    if (reason == SSL_ERROR_WANT_READ) {
      const ReadOutcome outcome = Feed(timeout, error);
      if (outcome == ReadOutcome::Data) continue;
      error = std::string("TLS handshake: ") +
              (outcome == ReadOutcome::Failed ? error : OutcomeText(outcome));
      Drop();
      return false;
    }
    const long verify = SSL_get_verify_result(m_ssl);
    if (verify != X509_V_OK) {
      error = std::string("TLS handshake: certificate verification failed: ") +
              X509_verify_cert_error_string(verify);
      ERR_clear_error();
    } else {
      error = OpenSslError("TLS handshake");
    }
    Drop();
    return false;
  }
}

bool TlsTransport::Flush(std::chrono::milliseconds timeout,
                         std::string &error) {
  std::uint8_t buffer[MAX_RECORD_SIZE];
  while (BIO_pending(m_writeBio) > 0) {
    const int count = BIO_read(m_writeBio, buffer, sizeof(buffer));
    if (count <= 0) break;
    if (!m_inner.Write(std::span<const std::uint8_t>(
                           buffer, static_cast<std::size_t>(count)),
                       timeout, error))
      return false;
  }
  return true;
}

ReadOutcome TlsTransport::Feed(std::chrono::milliseconds timeout,
                               std::string &error) {
  std::uint8_t buffer[CHUNK_SIZE];
  std::size_t count = 0;
  const ReadOutcome outcome = m_inner.Read(buffer, count, timeout, error);
  if (outcome == ReadOutcome::Data)
    BIO_write(m_readBio, buffer, static_cast<int>(count));
  return outcome;
}

std::string TlsTransport::Cipher() const {
  if (m_ssl == nullptr) return {};
  const char *name = SSL_get_cipher_name(m_ssl);
  return name != nullptr ? name : "";
}

std::string TlsTransport::Version() const {
  if (m_ssl == nullptr) return {};
  const char *name = SSL_get_version(m_ssl);
  return name != nullptr ? name : "";
}

bool TlsTransport::Connect(const std::string &host, std::uint16_t port,
                           std::chrono::milliseconds timeout,
                           std::string &error) {
  Drop();
  return m_inner.Connect(host, port, timeout, error);
}

void TlsTransport::Close() {
  if (m_ssl != nullptr) {
    std::string ignored;
    SSL_shutdown(m_ssl);
    Flush(CLOSE_TIMEOUT, ignored);
    Drop();
  }
  m_inner.Close();
}

ReadOutcome TlsTransport::Read(std::span<std::uint8_t> buffer,
                               std::size_t &bytesRead,
                               std::chrono::milliseconds timeout,
                               std::string &error) {
  bytesRead = 0;
  if (!Enabled()) return m_inner.Read(buffer, bytesRead, timeout, error);
  if (buffer.empty()) return ReadOutcome::Data;
  for (;;) {
    ERR_clear_error();
    std::size_t count = 0;
    if (SSL_read_ex(m_ssl, buffer.data(), buffer.size(), &count) == 1) {
      bytesRead = count;
      return ReadOutcome::Data;
    }
    const int reason = SSL_get_error(m_ssl, 0);
    if (reason == SSL_ERROR_WANT_READ) {
      if (!Flush(timeout, error)) return ReadOutcome::Failed;
      const ReadOutcome outcome = Feed(timeout, error);
      if (outcome != ReadOutcome::Data) return outcome;
      continue;
    }
    if (reason == SSL_ERROR_ZERO_RETURN) return ReadOutcome::Closed;
    error = OpenSslError("TLS read");
    return ReadOutcome::Failed;
  }
}

bool TlsTransport::Write(std::span<const std::uint8_t> data,
                         std::chrono::milliseconds timeout,
                         std::string &error) {
  if (!Enabled()) return m_inner.Write(data, timeout, error);
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t chunk = std::min(data.size() - offset, CHUNK_SIZE);
    ERR_clear_error();
    std::size_t written = 0;
    if (SSL_write_ex(m_ssl, data.data() + offset, chunk, &written) == 1) {
      offset += written;
      if (!Flush(timeout, error)) return false;
      continue;
    }
    const int reason = SSL_get_error(m_ssl, 0);
    if (reason == SSL_ERROR_WANT_READ) {
      if (!Flush(timeout, error)) return false;
      const ReadOutcome outcome = Feed(timeout, error);
      if (outcome == ReadOutcome::Data) continue;
      if (outcome != ReadOutcome::Failed)
        error = std::string("TLS write: ") + OutcomeText(outcome);
      return false;
    }
    error = OpenSslError("TLS write");
    return false;
  }
  return true;
}

}  // namespace binlog_streamer
