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

#include "net/cTlsCertificateGenerator.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>

namespace binlog_streamer {
namespace {

constexpr int KEY_BITS = 2048;
constexpr long VALID_DAYS = 3650;

using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

std::string OpenSslError(const std::string &what) {
  const unsigned long code = ERR_peek_last_error();
  char text[256];
  if (code == 0) return what;
  ERR_error_string_n(code, text, sizeof(text));
  ERR_clear_error();
  return what + ": " + text;
}

KeyPtr GenerateKey() { return KeyPtr(EVP_RSA_gen(KEY_BITS), &EVP_PKEY_free); }

bool SetRandomSerial(X509 *certificate) {
  // 20 bytes, the largest serial RFC 5280 allows, top bit clear so it
  // stays positive.
  unsigned char bytes[20];
  if (RAND_bytes(bytes, sizeof(bytes)) != 1) return false;
  bytes[0] &= 0x7F;
  const std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(
      BN_bin2bn(bytes, sizeof(bytes), nullptr), &BN_free);
  return serial &&
         BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate)) !=
             nullptr;
}

bool AddExtension(X509 *certificate, X509 *issuer, int nid, const char *value) {
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION *extension =
      X509V3_EXT_conf_nid(nullptr, &context, nid, value);
  if (extension == nullptr) return false;
  const int added = X509_add_ext(certificate, extension, -1);
  X509_EXTENSION_free(extension);
  return added == 1;
}

// issuer null: self-signed with its own key.
X509Ptr MakeCertificate(const std::string &commonName, EVP_PKEY *key,
                        X509 *issuer, EVP_PKEY *issuerKey, bool isCa,
                        std::string &error) {
  X509Ptr certificate(X509_new(), &X509_free);
  if (!certificate ||
      X509_set_version(certificate.get(), X509_VERSION_3) != 1 ||
      !SetRandomSerial(certificate.get()) ||
      X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
      X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                      VALID_DAYS * 24 * 60 * 60) == nullptr ||
      X509_set_pubkey(certificate.get(), key) != 1) {
    error = OpenSslError("preparing " + commonName);
    return X509Ptr(nullptr, &X509_free);
  }
  X509_NAME *subject = X509_get_subject_name(certificate.get());
  if (X509_NAME_add_entry_by_txt(
          subject, "CN", MBSTRING_ASC,
          reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1,
          0) != 1 ||
      X509_set_issuer_name(
          certificate.get(),
          issuer != nullptr ? X509_get_subject_name(issuer) : subject) != 1) {
    error = OpenSslError("naming " + commonName);
    return X509Ptr(nullptr, &X509_free);
  }
  X509 *extensionIssuer = issuer != nullptr ? issuer : certificate.get();
  if (!AddExtension(certificate.get(), extensionIssuer, NID_basic_constraints,
                    isCa ? "critical,CA:TRUE" : "critical,CA:FALSE") ||
      !AddExtension(certificate.get(), extensionIssuer,
                    NID_subject_key_identifier, "hash") ||
      !AddExtension(certificate.get(), extensionIssuer,
                    NID_authority_key_identifier, "keyid:always")) {
    error = OpenSslError("extending " + commonName);
    return X509Ptr(nullptr, &X509_free);
  }
  if (X509_sign(certificate.get(), issuerKey, EVP_sha256()) == 0) {
    error = OpenSslError("signing " + commonName);
    return X509Ptr(nullptr, &X509_free);
  }
  return certificate;
}

bool ToPem(X509 *certificate, std::string &pem, std::string &error) {
  const BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) {
    error = OpenSslError("writing a certificate as PEM");
    return false;
  }
  char *data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  pem.assign(data, static_cast<std::size_t>(length));
  return true;
}

bool ToPem(EVP_PKEY *key, std::string &pem, std::string &error) {
  const BioPtr bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0,
                                       nullptr, nullptr) != 1) {
    error = OpenSslError("writing a private key as PEM");
    return false;
  }
  char *data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  pem.assign(data, static_cast<std::size_t>(length));
  return true;
}

}  // namespace

bool TlsCertificateGenerator::Generate(const std::string &name,
                                       GeneratedCertificates &out,
                                       std::string &error) {
  ERR_clear_error();
  const KeyPtr caKey = GenerateKey();
  const KeyPtr serverKey = GenerateKey();
  if (!caKey || !serverKey) {
    error = OpenSslError("generating an RSA key");
    return false;
  }
  const X509Ptr ca =
      MakeCertificate(name + "_Auto_Generated_CA_Certificate", caKey.get(),
                      nullptr, caKey.get(), true, error);
  if (!ca) return false;
  const X509Ptr server =
      MakeCertificate(name + "_Auto_Generated_Server_Certificate",
                      serverKey.get(), ca.get(), caKey.get(), false, error);
  if (!server) return false;
  GeneratedCertificates generated;
  if (!ToPem(ca.get(), generated.caCertPem, error) ||
      !ToPem(caKey.get(), generated.caKeyPem, error) ||
      !ToPem(server.get(), generated.serverCertPem, error) ||
      !ToPem(serverKey.get(), generated.serverKeyPem, error))
    return false;
  out = std::move(generated);
  error.clear();
  return true;
}

}  // namespace binlog_streamer
