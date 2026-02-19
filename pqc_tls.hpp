#pragma once
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <stdexcept>
#include <string>

namespace sls {

// Client-side PQC config (TLS1.3 + KEM group + sigalgs) 
inline void apply_pqc_client(asio::ssl::context& ctx) {
  SSL_CTX* c = ctx.native_handle();

  SSL_CTX_set_min_proto_version(c, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION);

  // KEM / key share (hybrid)
  if (SSL_CTX_set1_groups_list(c, "X25519MLKEM768") != 1)
    throw std::runtime_error("SSL_CTX_set1_groups_list failed");

  // PQC signature algorithm (ML-DSA cert)
  if (SSL_CTX_set1_sigalgs_list(c, "ML-DSA-44") != 1)
    throw std::runtime_error("SSL_CTX_set1_sigalgs_list failed");
}

// Server-side PQC load (BIO -> X509 + EVP_PKEY -> SSL_CTX_use_*) 
inline void apply_pqc_server(asio::ssl::context& ctx,
                             const std::string& cert_file,
                             const std::string& key_file) {
  SSL_CTX* c = ctx.native_handle();

  SSL_CTX_set_min_proto_version(c, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(c, TLS1_3_VERSION);

  if (SSL_CTX_set1_groups_list(c, "X25519MLKEM768") != 1)
    throw std::runtime_error("SSL_CTX_set1_groups_list failed");

  if (SSL_CTX_set1_sigalgs_list(c, "ML-DSA-44") != 1)
    throw std::runtime_error("SSL_CTX_set1_sigalgs_list failed");

  // --- Key ---
  BIO* keybio = BIO_new_file(key_file.c_str(), "r");
  if (!keybio) throw std::runtime_error("BIO_new_file(key) failed");

  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(keybio, nullptr, nullptr, nullptr);
  BIO_free(keybio);
  if (!pkey) throw std::runtime_error("PEM_read_bio_PrivateKey failed");

  // --- Cert ---
  BIO* certbio = BIO_new_file(cert_file.c_str(), "r");
  if (!certbio) { EVP_PKEY_free(pkey); throw std::runtime_error("BIO_new_file(cert) failed"); }

  X509* cert = PEM_read_bio_X509(certbio, nullptr, nullptr, nullptr);
  BIO_free(certbio);
  if (!cert) { EVP_PKEY_free(pkey); throw std::runtime_error("PEM_read_bio_X509 failed"); }

  // Attach to ctx
  if (SSL_CTX_use_certificate(c, cert) != 1) {
    X509_free(cert); EVP_PKEY_free(pkey);
    throw std::runtime_error("SSL_CTX_use_certificate failed");
  }
  if (SSL_CTX_use_PrivateKey(c, pkey) != 1) {
    X509_free(cert); EVP_PKEY_free(pkey);
    throw std::runtime_error("SSL_CTX_use_PrivateKey failed");
  }

  // cleanup
  X509_free(cert);
  EVP_PKEY_free(pkey);

  // (Optional) verify that the key matches the certificate:
  if (SSL_CTX_check_private_key(c) != 1)
    throw std::runtime_error("SSL_CTX_check_private_key failed");
}

} // namespace sls
