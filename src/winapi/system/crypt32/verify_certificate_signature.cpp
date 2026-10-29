// SPDX-License-Identifier: LGPL-2.1-only
/// @file verify_certificate_signature.cpp
/// @brief CRYPT32 `CryptVerifyCertificateSignatureEx` over macOS Security.
///
/// Windows checks the subject's signature against the issuer's public key and
/// returns FALSE with NTE_BAD_SIGNATURE when it does not verify. Vitreo
/// previously returned TRUE unconditionally, which makes every caller believe
/// a chain it never actually checked. The signature math is delegated to the host's Security
/// framework rather than reimplemented.
///
/// Contract:
/// https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptverifycertificatesignatureex

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

// dwSubjectType (wincrypt.h)
constexpr u32 CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB = 1;
constexpr u32 CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT = 2;
// dwIssuerType
constexpr u32 CRYPT_VERIFY_CERT_SIGN_ISSUER_NULL   = 0;
constexpr u32 CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY = 1;
constexpr u32 CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT   = 2;

constexpr u32 NTE_BAD_SIGNATURE = 0x80090006u;

/// Pointer-width-aware field walk over a guest struct.
struct GuestReader {
    Marshaler& m;
    bool     is64;
    u32 read32(GuestAddr a) const { auto r = m.read_u32(a); return r.is_ok() ? r.value() : 0u; }
    u64 read_ptr(GuestAddr a) const { auto r = m.read_ptr(a); return r.is_ok() ? r.value() : 0ull; }
    usize ptr_size() const { return is64 ? 8u : 4u; }
};

std::vector<u8> read_blob(Marshaler& m, GuestAddr addr, u32 size) {
    std::vector<u8> out;
    if (addr == 0 || size == 0 || size > (16u << 20)) return out;
    out.resize(size);
    if (m.memory().read_bytes(addr, out.data(), out.size()).is_err()) out.clear();
    return out;
}

struct PublicKeyInfo {
    std::string algorithm_oid;
    std::vector<u8> algorithm_parameters;
    std::vector<u8> public_key;
    u32 unused_bits{};
};

bool read_bounded_blob(Marshaler& m, GuestAddr size_addr,
                       GuestAddr pointer_addr, std::vector<u8>& out) {
    auto size = m.read_u32(size_addr);
    auto pointer = m.read_ptr(pointer_addr);
    if (size.is_err() || pointer.is_err() || size.value() > (16u << 20)) {
        return false;
    }
    if (size.value() == 0) {
        out.clear();
        return true;
    }
    if (pointer.value() == 0) return false;
    out.resize(size.value());
    return m.memory().read_bytes(pointer.value(), out.data(), out.size()).is_ok();
}

bool read_public_key_info(Marshaler& m, GuestAddr address,
                          PublicKeyInfo& out) {
    if (address == 0) return false;
    const GuestAddr pointer_size = m.cpu_is_64bit() ? 8u : 4u;
    auto oid_pointer = m.read_ptr(address);
    if (oid_pointer.is_err() || oid_pointer.value() == 0) return false;
    auto oid = m.read_ansi_string(oid_pointer.value(), 256);
    if (oid.is_err()) return false;
    out.algorithm_oid = oid.value();

    if (!read_bounded_blob(m, address + pointer_size,
                           address + 2 * pointer_size,
                           out.algorithm_parameters)) {
        return false;
    }

    const GuestAddr public_key_blob = address + 3 * pointer_size;
    if (!read_bounded_blob(m, public_key_blob,
                           public_key_blob + pointer_size,
                           out.public_key)) {
        return false;
    }
    auto unused_bits = m.read_u32(public_key_blob + 2 * pointer_size);
    if (unused_bits.is_err()) return false;
    out.unused_bits = unused_bits.value();
    return true;
}

/// CERT_CONTEXT { DWORD dwCertEncodingType; BYTE* pbCertEncoded;
///                DWORD cbCertEncoded; PCERT_INFO; HCERTSTORE }
std::vector<u8> der_from_cert_context(Marshaler& m, GuestAddr ctx, bool is64) {
    if (ctx == 0) return {};
    const usize ps = is64 ? 8u : 4u;
    const GuestAddr pb_off = static_cast<GuestAddr>(is64 ? 8u : 4u);
    GuestReader r{m, is64};
    const u64 pb = r.read_ptr(ctx + pb_off);
    const u32 cb = r.read32(static_cast<GuestAddr>(ctx + pb_off + ps));
    return read_blob(m, static_cast<GuestAddr>(pb), cb);
}

/// CRYPT_DATA_BLOB { DWORD cbData; BYTE* pbData }
std::vector<u8> der_from_blob(Marshaler& m, GuestAddr blob, bool is64) {
    if (blob == 0) return {};
    GuestReader r{m, is64};
    const u32 cb = r.read32(blob);
    const u64 pb = r.read_ptr(static_cast<GuestAddr>(blob + (is64 ? 8u : 4u)));
    return read_blob(m, static_cast<GuestAddr>(pb), cb);
}

struct HostKey {
    SecKeyRef key{nullptr};
    ~HostKey() { if (key) CFRelease(key); }
};

/// SPKI key bytes are already in the shape SecKeyCreateWithData wants: a
/// PKCS#1 RSAPublicKey for RSA, an X9.63 uncompressed point for EC.
bool make_host_key(const std::string& key_alg_oid,
                   const std::vector<u8>& key_bytes, HostKey& out) {
    if (key_bytes.empty()) return false;
    CFStringRef type = nullptr;
    if (key_alg_oid == "1.2.840.113549.1.1.1") {
        type = kSecAttrKeyTypeRSA;
    } else if (key_alg_oid == "1.2.840.10045.2.1") {
        type = kSecAttrKeyTypeECSECPrimeRandom;
    } else {
        return false;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, key_bytes.data(),
                                  static_cast<CFIndex>(key_bytes.size()));
    if (!data) return false;
    const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass};
    const void* vals[] = {type, kSecAttrKeyClassPublic};
    CFDictionaryRef attrs = CFDictionaryCreate(
        kCFAllocatorDefault, keys, vals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!attrs) { CFRelease(data); return false; }
    out.key = SecKeyCreateWithData(data, attrs, nullptr);
    CFRelease(attrs);
    CFRelease(data);
    return out.key != nullptr;
}

/// Map the certificate's signatureAlgorithm OID onto a SecKey algorithm.
/// Returns nullptr for algorithms this path cannot check, so the caller can
/// fail honestly instead of guessing.
SecKeyAlgorithm signature_algorithm_for(const std::string& oid) {
    if (oid == "1.2.840.113549.1.1.5")  return kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA1;
    // szOID_OIWSEC_sha1RSASign (wincrypt.h): the OIW name for the same
    // PKCS#1 v1.5 signature over a SHA-1 DigestInfo. CryptFindOIDInfo maps
    // it to RSA/SHA1 (cert_properties.cpp); Microsoft's 2006-era code-signing
    // PCAs are signed with it.
    if (oid == "1.3.14.3.2.29")         return kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA1;
    if (oid == "1.2.840.113549.1.1.11") return kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256;
    if (oid == "1.2.840.113549.1.1.12") return kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA384;
    if (oid == "1.2.840.113549.1.1.13") return kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA512;
    if (oid == "1.2.840.10045.4.3.2")   return kSecKeyAlgorithmECDSASignatureMessageX962SHA256;
    if (oid == "1.2.840.10045.4.3.3")   return kSecKeyAlgorithmECDSASignatureMessageX962SHA384;
    if (oid == "1.2.840.10045.4.3.4")   return kSecKeyAlgorithmECDSASignatureMessageX962SHA512;
    return nullptr;   // RSASSA-PSS (…1.1.10) and everything else
}

/// md5WithRSAEncryption (szOID_RSA_MD5RSA, 1.2.840.113549.1.1.4): SecKey has
/// no MD5 message algorithm, so the PKCS#1 v1.5 DigestInfo is built here
/// (RFC 8017 section 9.2, note 1) and checked with the raw digest mode.
std::vector<u8> md5_digest_info(const std::vector<u8>& message) {
    static constexpr u8 kPrefix[] = {0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48,
                                     0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10};
    std::vector<u8> out(std::begin(kPrefix), std::end(kPrefix));
    u8 hash[CC_MD5_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5(message.data(), static_cast<CC_LONG>(message.size()), hash);
#pragma clang diagnostic pop
    out.insert(out.end(), hash, hash + sizeof(hash));
    return out;
}

bool host_verify(const x509::ParsedCertificate& subject,
                 const std::string& issuer_key_oid,
                 const std::vector<u8>& issuer_key_bytes) {
    if (subject.tbs_der.empty() || subject.signature_value.empty()) return false;
    const bool md5_rsa = subject.signature_algorithm.oid == "1.2.840.113549.1.1.4" &&
                         issuer_key_oid == "1.2.840.113549.1.1.1";
    SecKeyAlgorithm alg = md5_rsa ? kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw
                                  : signature_algorithm_for(subject.signature_algorithm.oid);
    if (!alg) return false;
    HostKey key;
    if (!make_host_key(issuer_key_oid, issuer_key_bytes, key)) return false;
    if (!SecKeyIsAlgorithmSupported(key.key, kSecKeyOperationTypeVerify, alg)) return false;

    const std::vector<u8> input = md5_rsa ? md5_digest_info(subject.tbs_der) : subject.tbs_der;
    CFDataRef signed_data = CFDataCreate(kCFAllocatorDefault, input.data(),
                                         static_cast<CFIndex>(input.size()));
    CFDataRef signature = CFDataCreate(kCFAllocatorDefault, subject.signature_value.data(),
                                       static_cast<CFIndex>(subject.signature_value.size()));
    bool ok = false;
    if (signed_data && signature) {
        CFErrorRef err = nullptr;
        ok = SecKeyVerifySignature(key.key, alg, signed_data, signature, &err);
        if (err) CFRelease(err);
    }
    if (signed_data) CFRelease(signed_data);
    if (signature) CFRelease(signature);
    return ok;
}

}  // namespace

namespace crypt32 {

void impl_CertComparePublicKeyInfo(Marshaler& m) {
    constexpr u32 X509_ASN_ENCODING = 0x00000001u;
    constexpr u32 PKCS_7_ASN_ENCODING = 0x00010000u;
    const u32 encoding = static_cast<u32>(m.arg(0));
    if ((encoding & (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING)) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    PublicKeyInfo first;
    PublicKeyInfo second;
    if (!read_public_key_info(m, m.arg_ptr(1), first) ||
        !read_public_key_info(m, m.arg_ptr(2), second)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const bool equal =
        first.algorithm_oid == second.algorithm_oid &&
        first.algorithm_parameters == second.algorithm_parameters &&
        first.public_key == second.public_key &&
        first.unused_bits == second.unused_bits;
    m.set_return(equal ? 1u : 0u);
}

} // namespace crypt32

bool certificate_signed_by(const x509::ParsedCertificate& subject,
                           const x509::ParsedCertificate& issuer) {
    return host_verify(subject, issuer.spki.algorithm.oid, issuer.spki.key);
}

bool verify_message_signature(const x509::PublicKeyInfo& key, std::string_view hash,
                              std::span<const u8> message, std::span<const u8> signature) {
    const bool ec = key.algorithm.oid == "1.2.840.10045.2.1";
    SecKeyAlgorithm algorithm = nullptr;
    if (hash == "SHA1") algorithm = ec ? kSecKeyAlgorithmECDSASignatureMessageX962SHA1
                                       : kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA1;
    if (hash == "SHA256") algorithm = ec ? kSecKeyAlgorithmECDSASignatureMessageX962SHA256
                                         : kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256;
    if (hash == "SHA384") algorithm = ec ? kSecKeyAlgorithmECDSASignatureMessageX962SHA384
                                         : kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA384;
    if (hash == "SHA512") algorithm = ec ? kSecKeyAlgorithmECDSASignatureMessageX962SHA512
                                         : kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA512;
    if (algorithm == nullptr || signature.empty()) return false;
    HostKey host_key;
    if (!make_host_key(key.algorithm.oid, key.key, host_key)) return false;
    if (!SecKeyIsAlgorithmSupported(host_key.key, kSecKeyOperationTypeVerify, algorithm)) {
        return false;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, message.data(),
                                  static_cast<CFIndex>(message.size()));
    CFDataRef sig = CFDataCreate(kCFAllocatorDefault, signature.data(),
                                 static_cast<CFIndex>(signature.size()));
    bool ok = false;
    if (data && sig) {
        CFErrorRef error = nullptr;
        ok = SecKeyVerifySignature(host_key.key, algorithm, data, sig, &error);
        if (error) CFRelease(error);
    }
    if (data) CFRelease(data);
    if (sig) CFRelease(sig);
    return ok;
}

u32 Crypt32Manager::crypt_verify_certificate_signature_ex(Marshaler& m) {
    const bool is64 = m.cpu_is_64bit();
    const auto subject_type = static_cast<u32>(m.arg(2));
    const GuestAddr subject  = m.arg_ptr(3);
    const auto issuer_type  = static_cast<u32>(m.arg(4));
    const GuestAddr issuer   = m.arg_ptr(5);

    // Documented: with an ISSUER_NULL issuer the signature is not checked.
    if (issuer_type == CRYPT_VERIFY_CERT_SIGN_ISSUER_NULL) {
        VITREO_TRACE("[crypt32] CryptVerifyCertificateSignatureEx issuer=NULL -> TRUE\n");
        return 1;
    }

    std::vector<u8> subject_der;
    if (subject_type == CRYPT_VERIFY_CERT_SIGN_SUBJECT_CERT) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cert_contexts.find(subject);
            if (it != m_cert_contexts.end()) subject_der = it->second.der_data;
        }
        if (subject_der.empty()) {
            subject_der = der_from_cert_context(m, subject, is64);
        }
    } else if (subject_type == CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB) {
        subject_der = der_from_blob(m, subject, is64);
    } else {
        // CRLs and OCSP responses are a different signed structure; report the
        // honest "not handled" rather than a fabricated success.
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        VITREO_TRACE("[crypt32] CryptVerifyCertificateSignatureEx subject_type=%u"
                     " unsupported -> FALSE\n", subject_type);
        return 0;
    }

    auto parsed_subject = x509::parse_certificate(subject_der);
    if (!parsed_subject) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    std::string issuer_key_oid;
    std::vector<u8> issuer_key;
    if (issuer_type == CRYPT_VERIFY_CERT_SIGN_ISSUER_CERT) {
        std::vector<u8> issuer_der;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_cert_contexts.find(issuer);
            if (it != m_cert_contexts.end()) issuer_der = it->second.der_data;
        }
        if (issuer_der.empty()) {
            issuer_der = der_from_cert_context(m, issuer, is64);
        }
        auto parsed_issuer = x509::parse_certificate(issuer_der);
        if (!parsed_issuer) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        issuer_key_oid = parsed_issuer->spki.algorithm.oid;
        issuer_key     = parsed_issuer->spki.key;
    } else if (issuer_type == CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY) {
        // CERT_PUBLIC_KEY_INFO { CRYPT_ALGORITHM_IDENTIFIER Algorithm
        //   { LPSTR pszObjId; CRYPT_OBJID_BLOB Parameters {cbData, pbData} };
        //   CRYPT_BIT_BLOB PublicKey { cbData, pbData, cUnusedBits } }
        const usize ps = is64 ? 8u : 4u;
        GuestReader r{m, is64};
        const u64 oid_ptr = r.read_ptr(issuer);
        if (oid_ptr != 0) {
            auto oid = m.read_ansi_string(static_cast<GuestAddr>(oid_ptr));
            if (oid.is_ok()) issuer_key_oid = oid.value();
        }
        const GuestAddr bit_blob = static_cast<GuestAddr>(issuer + ps + ps + ps);
        const u32 cb = r.read32(bit_blob);
        const u64 pb = r.read_ptr(static_cast<GuestAddr>(bit_blob + ps));
        issuer_key = read_blob(m, static_cast<GuestAddr>(pb), cb);
    } else {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return 0;
    }

    const bool ok = host_verify(*parsed_subject, issuer_key_oid, issuer_key);
    VITREO_TRACE("[crypt32] CryptVerifyCertificateSignatureEx sig_alg=%s key_alg=%s -> %s\n",
                 parsed_subject->signature_algorithm.oid.c_str(),
                 issuer_key_oid.c_str(), ok ? "TRUE" : "FALSE");
    if (!ok) {
        set_last_error(NTE_BAD_SIGNATURE);
        return 0;
    }
    return 1;
}

}  // namespace vitreo::winapi
