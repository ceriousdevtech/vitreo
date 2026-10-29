// SPDX-License-Identifier: LGPL-2.1-only
/// @file crypt_msg_signer.cpp
/// @brief CryptMsgGetAndVerifySigner and CryptHashPublicKeyInfo.
///
/// Contract sources:
///   https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptmsggetandverifysigner
///   https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-crypthashpublickeyinfo
/// What Learn leaves open (errors, outputs on failure, which ALG_IDs hash,
/// how the key info is encoded) was measured on Windows 11 by
/// tools/oracle_probe/crypt_signer_trio_probe.c; the output is
/// tests/oracle_data/crypt_signer_trio_probe{,.x86}.expected.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 kCmsgTrustedSignerFlag = 0x1;
constexpr u32 kCmsgSignerOnlyFlag = 0x2;
constexpr u32 kCmsgUseSignerIndexFlag = 0x4;

constexpr u32 kCryptEInvalidIndex = 0x80091008u;
constexpr u32 kCryptENoTrustedSigner = 0x8009202Bu;
constexpr u32 kNteBadKey = 0x80090003u;
constexpr u32 kNteBadAlgid = 0x80090008u;
constexpr u32 kCryptEAsn1BadArgs = 0x80093109u;
constexpr u32 kEInvalidArg = 0x80070057u;

constexpr u32 kX509AsnEncoding = 0x00000001u;
constexpr u32 kPkcs7AsnEncoding = 0x00010000u;

std::vector<u8> strip_leading_zeros(std::vector<u8> v) {
    while (v.size() > 1 && v.front() == 0) v.erase(v.begin());
    return v;
}

void put_length(std::vector<u8>& out, usize n) {
    if (n < 0x80) {
        out.push_back(static_cast<u8>(n));
        return;
    }
    std::vector<u8> len;
    for (usize v = n; v; v >>= 8) len.insert(len.begin(), static_cast<u8>(v));
    out.push_back(static_cast<u8>(0x80 | len.size()));
    out.insert(out.end(), len.begin(), len.end());
}

void put_tlv(std::vector<u8>& out, u8 tag, const std::vector<u8>& value) {
    out.push_back(tag);
    put_length(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

/// One hash ALG_ID's outcome (crypt_signer_trio_probe "hpki" rows).
struct HashAlgorithm {
    const char* name;  ///< host digest, or nullptr when the id fails
    u32 error;         ///< last error when it fails
};

HashAlgorithm hash_algorithm(u32 alg_id) {
    switch (alg_id) {
        case 0:       return {"MD5", 0};     // Learn: zero selects MD5
        case 0x8001:  return {"MD2", 0};     // CALG_MD2
        case 0x8002:  return {"MD4", 0};     // CALG_MD4
        case 0x8003:  return {"MD5", 0};     // CALG_MD5
        case 0x8004:  return {"SHA1", 0};    // CALG_SHA1
        case 0x800c:  return {"SHA256", 0};  // CALG_SHA_256
        case 0x800d:  return {"SHA384", 0};  // CALG_SHA_384
        case 0x800e:  return {"SHA512", 0};  // CALG_SHA_512
        case 0x8005:  return {nullptr, kNteBadKey};                       // CALG_MAC
        case 0x8009:  return {nullptr, kNteBadKey};                       // CALG_HMAC
        case 0x8008:  return {nullptr, error_codes::ERROR_INVALID_HANDLE};  // CALG_SSL3_SHAMD5
        case 0x2400:  return {nullptr, error_codes::ERROR_NOT_SUPPORTED};   // CALG_RSA_SIGN
        default: break;
    }
    // Measured: an id with no CryptoAPI meaning (0x1234) is NTE_BAD_ALGID.
    // Other defined ALG_IDs were not measured; they fail the same way and
    // say so once.
    static constexpr u32 kKnownUndefined = 0x1234;
    if (alg_id != kKnownUndefined) {
        char detail[64];
        std::snprintf(detail, sizeof detail, "ALG_ID 0x%x (unmeasured)", alg_id);
        crypt32::report_unimplemented("crypt32!CryptHashPublicKeyInfo", detail);
    }
    return {nullptr, kNteBadAlgid};
}

std::vector<u8> host_digest(const char* name, const std::vector<u8>& data) {
    std::vector<u8> out;
    const auto len = static_cast<CC_LONG>(data.size());
    const std::string n = name;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (n == "MD2") { out.resize(CC_MD2_DIGEST_LENGTH); CC_MD2(data.data(), len, out.data()); }
    else if (n == "MD4") { out.resize(CC_MD4_DIGEST_LENGTH); CC_MD4(data.data(), len, out.data()); }
    else if (n == "MD5") { out.resize(CC_MD5_DIGEST_LENGTH); CC_MD5(data.data(), len, out.data()); }
#pragma clang diagnostic pop
    else if (n == "SHA1") { out.resize(CC_SHA1_DIGEST_LENGTH); CC_SHA1(data.data(), len, out.data()); }
    else if (n == "SHA256") { out.resize(CC_SHA256_DIGEST_LENGTH); CC_SHA256(data.data(), len, out.data()); }
    else if (n == "SHA384") { out.resize(CC_SHA384_DIGEST_LENGTH); CC_SHA384(data.data(), len, out.data()); }
    else if (n == "SHA512") { out.resize(CC_SHA512_DIGEST_LENGTH); CC_SHA512(data.data(), len, out.data()); }
    return out;
}

} // namespace

// BOOL CryptHashPublicKeyInfo(HCRYPTPROV_LEGACY hCryptProv, ALG_ID Algid,
//     DWORD dwFlags, DWORD dwCertEncodingType, PCERT_PUBLIC_KEY_INFO pInfo,
//     BYTE* pbComputedHash, DWORD* pcbComputedHash)
// The key info is DER-encoded as SubjectPublicKeyInfo -- an algorithm with
// no parameters gets NULL ones, a parameters blob is copied as given -- and
// hashed. Measured: hCryptProv and dwFlags are ignored; X509_ASN_ENCODING or
// PKCS_7_ASN_ENCODING selects the encoder, anything else is
// ERROR_FILE_NOT_FOUND; an empty OID is E_INVALIDARG and a malformed one
// CRYPT_E_ASN1_BADARGS; a NULL buffer is a size query; a short buffer is
// ERROR_MORE_DATA with the needed size and the buffer untouched; every other
// failure leaves *pcbComputedHash 0; success sets the last error to 0.
u32 Crypt32Manager::crypt_hash_public_key_info(Marshaler& m) {
    const u32 alg_id = m.arg32(1);
    const u32 encoding = m.arg32(3);
    const GuestAddr info = m.arg_ptr(4);
    const GuestAddr out = m.arg_ptr(5);
    const GuestAddr size_ptr = m.arg_ptr(6);
    const bool is64 = m.cpu_is_64bit();

    const u32 capacity = size_ptr ? m.read_u32(size_ptr).value_or(0) : 0;
    auto fail = [&](u32 error) {
        if (size_ptr) (void)m.write_u32(size_ptr, 0);
        set_last_error(error);
        return 0u;
    };
    if ((encoding & (kX509AsnEncoding | kPkcs7AsnEncoding)) == 0) {
        return fail(error_codes::ERROR_FILE_NOT_FOUND);
    }

    // CERT_PUBLIC_KEY_INFO { CRYPT_ALGORITHM_IDENTIFIER { LPSTR pszObjId;
    // CRYPT_OBJID_BLOB Parameters; } Algorithm; CRYPT_BIT_STRING PublicKey; }
    const u32 ps = is64 ? 8 : 4;
    auto read_ptr = [&](GuestAddr at) -> GuestAddr {
        return is64 ? static_cast<GuestAddr>(m.read_u64(at).value_or(0)) : m.read_u32(at).value_or(0);
    };
    const GuestAddr oid_ptr = read_ptr(info);
    const u32 params_len = m.read_u32(info + ps).value_or(0);
    const GuestAddr params_ptr = read_ptr(info + 2 * ps);
    const u32 key_len = m.read_u32(info + 3 * ps).value_or(0);
    const GuestAddr key_ptr = read_ptr(info + 4 * ps);
    const u32 unused_bits = m.read_u32(info + 5 * ps).value_or(0);

    const std::string oid = oid_ptr ? m.read_ansi_string(oid_ptr, 256).value_or("") : std::string{};
    if (oid.empty()) return fail(kEInvalidArg);
    const std::vector<u8> oid_der = x509::encode_oid(oid);
    if (oid_der.empty()) return fail(kCryptEAsn1BadArgs);

    std::vector<u8> params(params_len);
    if (params_len && m.memory().read_bytes(params_ptr, params.data(), params_len).is_err()) {
        crypt32::raise_access_violation(m, params_ptr, false);
        return 0;
    }
    std::vector<u8> key(key_len);
    if (key_len && m.memory().read_bytes(key_ptr, key.data(), key_len).is_err()) {
        crypt32::raise_access_violation(m, key_ptr, false);
        return 0;
    }

    std::vector<u8> algorithm;
    put_tlv(algorithm, 0x06, oid_der);
    if (params.empty()) {
        algorithm.push_back(0x05);
        algorithm.push_back(0x00);
    } else {
        algorithm.insert(algorithm.end(), params.begin(), params.end());
    }
    std::vector<u8> bits{static_cast<u8>(unused_bits)};
    bits.insert(bits.end(), key.begin(), key.end());
    // DER: the unused trailing bits of a BIT STRING are encoded as zero.
    if (key_len != 0 && unused_bits != 0 && unused_bits < 8) {
        bits.back() = static_cast<u8>(bits.back() & ~((1u << unused_bits) - 1u));
    }
    std::vector<u8> body;
    put_tlv(body, 0x30, algorithm);
    put_tlv(body, 0x03, bits);
    std::vector<u8> encoded;
    put_tlv(encoded, 0x30, body);

    const HashAlgorithm hash = hash_algorithm(alg_id);
    if (hash.name == nullptr) return fail(hash.error);
    const std::vector<u8> digest = host_digest(hash.name, encoded);
    const u32 needed = static_cast<u32>(digest.size());
    if (out == 0) {
        (void)m.write_u32(size_ptr, needed);
        set_last_error(error_codes::ERROR_SUCCESS);
        return 1;
    }
    if (capacity < needed) {
        (void)m.write_u32(size_ptr, needed);
        set_last_error(error_codes::ERROR_MORE_DATA);
        return 0;
    }
    if (m.memory().write_bytes(out, digest.data(), needed).is_err()) {
        crypt32::raise_access_violation(m, out, true);
        return 0;
    }
    (void)m.write_u32(size_ptr, needed);
    set_last_error(error_codes::ERROR_SUCCESS);
    return 1;
}

// BOOL CryptMsgGetAndVerifySigner(HCRYPTMSG hCryptMsg, DWORD cSignerStore,
//     HCERTSTORE* rghSignerStore, DWORD dwFlags, PCCERT_CONTEXT* ppSigner,
//     DWORD* pdwSignerIndex)
// Measured (crypt_signer_trio_probe "gav" rows, both bitnesses):
//  * Each signer (or only *pdwSignerIndex with CMSG_USE_SIGNER_INDEX_FLAG)
//    is looked up by issuer and serial number in the message's certificates
//    and then the given stores, or only the given stores with
//    CMSG_TRUSTED_SIGNER_FLAG; unknown flag bits are ignored.
//  * Unless CMSG_SIGNER_ONLY_FLAG, the signer's signed attributes are
//    verified against its key and the content: a bad signature is
//    STATUS_INVALID_SIGNATURE, a content digest mismatch CRYPT_E_HASH_VALUE.
//  * Success: *ppSigner a new context (in a store of its own), *pdwSignerIndex
//    the signer index, last error 0.
//  * Failure: *ppSigner NULL; *pdwSignerIndex 0 unless the index was an
//    input; an index past the signers is CRYPT_E_INVALID_INDEX, no signer
//    certificate found (or no signers, or a message that is not signed data)
//    CRYPT_E_NO_TRUSTED_SIGNER.
u32 Crypt32Manager::crypt_msg_get_and_verify_signer(Marshaler& m) {
    const u32 message_handle = static_cast<u32>(m.arg(0));
    const u32 store_count = m.arg32(1);
    const GuestAddr stores_ptr = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    const GuestAddr signer_out = m.arg_ptr(4);
    const GuestAddr index_ptr = m.arg_ptr(5);
    const bool is64 = m.cpu_is_64bit();
    const bool use_index = (flags & kCmsgUseSignerIndexFlag) != 0;

    std::vector<u32> stores;
    for (u32 i = 0; i < store_count && stores_ptr != 0; ++i) {
        const u64 h = is64 ? m.read_u64(stores_ptr + i * 8).value_or(0) : m.read_u32(stores_ptr + i * 4).value_or(0);
        stores.push_back(static_cast<u32>(h));
    }
    const u32 requested = (use_index && index_ptr) ? m.read_u32(index_ptr).value_or(0) : 0;

    auto fail = [&](u32 error) {
        if (signer_out) (void)m.write_ptr(signer_out, 0);
        if (index_ptr && !use_index) (void)m.write_u32(index_ptr, 0);
        set_last_error(error);
        return 0u;
    };

    std::vector<u8> signer_der;
    std::optional<x509::ParsedCertificate> signer_parsed;
    u32 signer_index = 0;
    u32 error = kCryptENoTrustedSigner;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_query_messages.find(message_handle);
        if (it == m_query_messages.end()) return fail(kCryptENoTrustedSigner);
        const QueryMessage& msg = it->second;
        if (use_index && requested >= msg.signers.size()) return fail(kCryptEInvalidIndex);

        std::vector<std::vector<u8>> candidates;
        if ((flags & kCmsgTrustedSignerFlag) == 0) {
            for (const auto& c : msg.certificates) candidates.push_back(c.der);
        }
        for (u32 store : stores) {
            for (auto& der : store_certificates_locked(store)) candidates.push_back(std::move(der));
        }

        const usize first = use_index ? requested : 0;
        const usize last = use_index ? requested + 1 : msg.signers.size();
        for (usize i = first; i < last; ++i) {
            const auto& signer = msg.signers[i];
            const auto serial = strip_leading_zeros(signer.serial);
            std::optional<x509::ParsedCertificate> parsed;
            const std::vector<u8>* der = nullptr;
            for (const auto& candidate : candidates) {
                auto p = x509::parse_certificate(candidate);
                if (p && p->issuer_der == signer.issuer && strip_leading_zeros(p->serial) == serial) {
                    parsed = std::move(p);
                    der = &candidate;
                    break;
                }
            }
            if (!parsed) {
                error = kCryptENoTrustedSigner;
                continue;
            }
            if ((flags & kCmsgSignerOnlyFlag) == 0) {
                const u32 verify = crypt32::verify_signer_attributes(signer, *parsed, msg.content_value);
                if (verify != 0) {
                    error = verify;
                    continue;
                }
            }
            signer_der = *der;
            signer_parsed = std::move(parsed);
            signer_index = static_cast<u32>(i);
            break;
        }
    }
    if (!signer_parsed) return fail(error);

    if (signer_out) {
        const GuestAddr context = create_owned_cert_context(m, signer_der, *signer_parsed, kX509AsnEncoding);
        if (context == 0) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        (void)m.write_ptr(signer_out, context);
    }
    if (index_ptr) (void)m.write_u32(index_ptr, signer_index);
    set_last_error(error_codes::ERROR_SUCCESS);
    return 1;
}

} // namespace vitreo::winapi
