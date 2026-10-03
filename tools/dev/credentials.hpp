#pragma once

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <memory>

namespace credentials {
inline void require(bool ok) {
    if (!ok) throw std::runtime_error("Development credential operation failed");
}
inline std::string hex(const unsigned char* bytes, size_t size) {
    const char* digits = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}
inline void save(BIO* bio, const fs::path& path) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    require(size > 0);
    write(path, std::string(data, static_cast<size_t>(size)));
}
inline void generate(const fs::path& directory) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    require(context && EVP_PKEY_keygen_init(context.get()) > 0 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) > 0);
    EVP_PKEY* raw = nullptr;
    require(EVP_PKEY_keygen(context.get(), &raw) > 0);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    require(bool(cert));
    unsigned char serial[16];
    require(RAND_bytes(serial, sizeof(serial)) == 1);
    serial[0] = (serial[0] & 0x7f) | 1;
    std::unique_ptr<BIGNUM, decltype(&BN_free)> number(BN_bin2bn(serial, sizeof(serial), nullptr), BN_free);
    require(number && BN_to_ASN1_INTEGER(number.get(), X509_get_serialNumber(cert.get())));
    require(X509_set_version(cert.get(), 2) == 1 &&
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) &&
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 13 * 24 * 60 * 60) &&
        X509_set_pubkey(cert.get(), key.get()) == 1);
    X509_NAME* subject = X509_get_subject_name(cert.get());
    require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1 &&
        X509_set_issuer_name(cert.get(), subject) == 1);
    X509V3_CTX extensions{};
    X509V3_set_ctx(&extensions, cert.get(), cert.get(), nullptr, nullptr, 0);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_conf_nid(nullptr, &extensions, NID_subject_alt_name,
            "DNS:localhost,IP:127.0.0.1"), X509_EXTENSION_free);
    require(san && X509_add_ext(cert.get(), san.get(), -1) == 1 &&
        X509_sign(cert.get(), key.get(), EVP_sha256()) > 0);
    std::unique_ptr<BIO, decltype(&BIO_free)> out(BIO_new(BIO_s_mem()), BIO_free);
    require(out && PEM_write_bio_PrivateKey(out.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    save(out.get(), directory / "key.pem");
    require(BIO_reset(out.get()) == 1 && PEM_write_bio_X509(out.get(), cert.get()) == 1);
    save(out.get(), directory / "cert.pem");
    unsigned char token[32];
    require(RAND_bytes(token, sizeof(token)) == 1);
    write(directory / "token", hex(token, sizeof(token)) + "\n");
}
inline std::string fingerprint(const fs::path& certificate, const fs::path& private_key) {
    std::unique_ptr<BIO, decltype(&BIO_free)> in(BIO_new_file(certificate.c_str(), "r"), BIO_free);
    require(bool(in));
    std::unique_ptr<X509, decltype(&X509_free)> cert(PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr), X509_free);
    in.reset(BIO_new_file(private_key.c_str(), "r"));
    require(cert && in);
    // An empty password callback avoids interactive prompts for unsupported encrypted keys.
    auto no_password = [](char*, int, int, void*) { return 0; };
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PrivateKey(in.get(), nullptr, no_password, nullptr), EVP_PKEY_free);
    require(key && X509_check_private_key(cert.get(), key.get()) == 1);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    require(X509_digest(cert.get(), EVP_sha256(), digest, &size) == 1 && size == 32);
    return hex(digest, size);
}
} // namespace credentials
