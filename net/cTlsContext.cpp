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

#include "net/cTlsContext.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <memory>

namespace binlog_streamer {
namespace {

std::string OpenSslError(const std::string &what) {
  const unsigned long code = ERR_peek_last_error();
  char text[256];
  if (code == 0) return what;
  ERR_error_string_n(code, text, sizeof(text));
  ERR_clear_error();
  return what + ": " + text;
}

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

BioPtr MemoryBio(const std::string &pem) {
  return BioPtr(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())),
                &BIO_free);
}

}  // namespace

TlsContext::~TlsContext() {
  if (m_context != nullptr) SSL_CTX_free(m_context);
}

bool TlsContext::LoadMaterial(const TlsMaterial &material, std::string &error) {
  ERR_clear_error();
  if (!material.certPem.empty()) {
    const BioPtr bio = MemoryBio(material.certPem);
    X509Ptr leaf(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                 &X509_free);
    if (!leaf) {
      error = OpenSslError("ssl_cert: not a PEM certificate");
      return false;
    }
    if (SSL_CTX_use_certificate(m_context, leaf.get()) != 1) {
      error = OpenSslError("ssl_cert");
      return false;
    }
    for (;;) {
      X509Ptr link(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                   &X509_free);
      if (!link) break;
      if (SSL_CTX_add1_chain_cert(m_context, link.get()) != 1) {
        error = OpenSslError("ssl_cert: chain certificate");
        return false;
      }
    }
    ERR_clear_error();
  }
  if (!material.keyPem.empty()) {
    const BioPtr bio = MemoryBio(material.keyPem);
    KeyPtr key(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
               &EVP_PKEY_free);
    if (!key) {
      error = OpenSslError("ssl_key: not a PEM private key");
      return false;
    }
    if (SSL_CTX_use_PrivateKey(m_context, key.get()) != 1 ||
        SSL_CTX_check_private_key(m_context) != 1) {
      if (ERR_GET_REASON(ERR_peek_last_error()) == X509_R_KEY_VALUES_MISMATCH) {
        ERR_clear_error();
        error = "ssl_key does not match ssl_cert";
      } else {
        error = OpenSslError("ssl_key");
      }
      return false;
    }
  }
  if (!material.caPem.empty()) {
    const BioPtr bio = MemoryBio(material.caPem);
    X509_STORE *store = SSL_CTX_get_cert_store(m_context);
    unsigned added = 0;
    for (;;) {
      X509Ptr ca(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr),
                 &X509_free);
      if (!ca) break;
      if (X509_STORE_add_cert(store, ca.get()) != 1) {
        error = OpenSslError("ssl_ca");
        return false;
      }
      ++added;
    }
    ERR_clear_error();
    if (added == 0) {
      error = "ssl_ca: not a PEM certificate";
      return false;
    }
  }
  return true;
}

bool TlsContext::Check(const TlsMaterial &material, std::string &error) {
  TlsContext probe;
  probe.m_context = SSL_CTX_new(TLS_method());
  if (probe.m_context == nullptr) {
    error = OpenSslError("creating the TLS context");
    return false;
  }
  if (!probe.LoadMaterial(material, error)) return false;
  error.clear();
  return true;
}

bool TlsContext::LoadServer(const TlsMaterial &material, std::string &error) {
  if (material.certPem.empty() || material.keyPem.empty()) {
    error = "a server needs both ssl_cert and ssl_key";
    return false;
  }
  if (m_context != nullptr) SSL_CTX_free(m_context);
  m_context = SSL_CTX_new(TLS_server_method());
  if (m_context == nullptr) {
    error = OpenSslError("creating the TLS context");
    return false;
  }
  // TLSv1.2 and up, as a MySQL 8.4 server accepts (tls_version).
  SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);
  SSL_CTX_set_options(m_context, SSL_OP_NO_COMPRESSION);
  SSL_CTX_set_session_cache_mode(m_context, SSL_SESS_CACHE_OFF);
  if (!LoadMaterial(material, error)) {
    SSL_CTX_free(m_context);
    m_context = nullptr;
    return false;
  }
  // A client certificate is checked when presented, never demanded.
  SSL_CTX_set_verify(m_context,
                     material.caPem.empty() ? SSL_VERIFY_NONE : SSL_VERIFY_PEER,
                     nullptr);
  m_mode = SslMode::Required;
  error.clear();
  return true;
}

bool TlsContext::LoadClient(SslMode mode, const TlsMaterial &material,
                            std::string &error) {
  if (mode == SslMode::Disabled) {
    error = "ssl_mode DISABLED needs no context";
    return false;
  }
  const bool verifies =
      mode == SslMode::VerifyCa || mode == SslMode::VerifyIdentity;
  if (verifies && material.caPem.empty()) {
    error = "ssl_mode " + std::string(SslModeName(mode)) + " requires ssl_ca";
    return false;
  }
  if (material.certPem.empty() != material.keyPem.empty()) {
    error = "ssl_cert and ssl_key go together";
    return false;
  }
  if (m_context != nullptr) SSL_CTX_free(m_context);
  m_context = SSL_CTX_new(TLS_client_method());
  if (m_context == nullptr) {
    error = OpenSslError("creating the TLS context");
    return false;
  }
  SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION);
  SSL_CTX_set_options(m_context, SSL_OP_NO_COMPRESSION);
  SSL_CTX_set_session_cache_mode(m_context, SSL_SESS_CACHE_OFF);
  if (!LoadMaterial(material, error)) {
    SSL_CTX_free(m_context);
    m_context = nullptr;
    return false;
  }
  SSL_CTX_set_verify(m_context, verifies ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                     nullptr);
  m_mode = mode;
  error.clear();
  return true;
}

}  // namespace binlog_streamer
