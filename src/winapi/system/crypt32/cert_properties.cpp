// SPDX-License-Identifier: LGPL-2.1-only
/// @file cert_properties.cpp
/// @brief Certificate extended properties, enhanced key usage and name
///        comparison.
///
/// Contracts:
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetcertificatecontextproperty
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certsetcertificatecontextproperty
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetenhancedkeyusage
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certsetenhancedkeyusage
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certcomparecertificatename
/// Measured Windows behaviour (x86 and x64):
///  tools/oracle_probe/crypt_prop_probe.c (.expected, .x86.expected) — which
///    property ids are computed and how, sizing, set/replace/delete, the
///    CRYPT_KEY_PROV_INFO serialization, name comparison;
///  tools/oracle_probe/cert_eku_probe.cpp (cert_eku_probe32/64.expected) —
///    CertGetEnhancedKeyUsage over every extension/property/flag/size mode.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/guest_block.hpp"
#include "x509_parse.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

namespace vitreo::winapi {

namespace crypt32 {

void report_unimplemented(const char* api, const char* detail) {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::lock_guard<std::mutex> lock(mutex);
    if (reported.insert(std::string(api) + '\0' + detail).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s: %s\n", api, detail);
    }
}

void raise_access_violation(Marshaler& m, u64 address, bool write) {
    if (auto* cpu = m.cpu()) {
        auto& fault = cpu->pending_raise;
        fault.active = true;
        fault.from_helper = false;
        fault.code = 0xC0000005u;
        fault.flags = 0;
        fault.num_args = 2;
        fault.args[0] = write ? 1 : 0;
        fault.args[1] = address;
    }
}

// BOOL CertCompareCertificateName(DWORD dwCertEncodingType,
//     PCERT_NAME_BLOB pCertName1, PCERT_NAME_BLOB pCertName2)
// Byte comparison of the two encoded names. The encoding type is not
// consulted and the last error is left alone (crypt_prop_probe "cmp" row).
void impl_CertCompareCertificateName(Marshaler& m) {
    const GuestAddr first = m.arg_ptr(1);
    const GuestAddr second = m.arg_ptr(2);
    const GuestAddr data_offset = m.cpu_is_64bit() ? 8u : 4u;
    auto read = [&](GuestAddr blob, std::vector<u8>& out) -> bool {
        const auto size = m.read_u32(blob);
        const auto pointer = m.read_ptr(blob + data_offset);
        if (size.is_err() || pointer.is_err()) return false;
        out.resize(size.value());
        return size.value() == 0 ||
               m.memory().read_bytes(static_cast<GuestAddr>(pointer.value()),
                                     reinterpret_cast<Byte*>(out.data()),
                                     out.size()).is_ok();
    };
    std::vector<u8> a;
    std::vector<u8> b;
    if (!read(first, a) || !read(second, b)) {
        // Windows dereferences both blobs; an unreadable one faults there.
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(a == b ? 1u : 0u);
}

} // namespace crypt32

namespace {

using crypt32::err::kCryptENotFound;
using crypt32::err::kEInvalidArg;

constexpr u32 kKeyProvHandleProp = 1;
constexpr u32 kKeyProvInfoProp = 2;
constexpr u32 kSha1HashProp = 3;
constexpr u32 kMd5HashProp = 4;
constexpr u32 kKeyContextProp = 5;
constexpr u32 kKeySpecProp = 6;
constexpr u32 kEnhKeyUsageProp = 9;
constexpr u32 kAccessStateProp = 14;
constexpr u32 kSignatureHashProp = 15;
constexpr u32 kKeyIdentifierProp = 20;
constexpr u32 kSubjectPublicKeyMd5Prop = 25;
constexpr u32 kIssuerSerialMd5Prop = 28;
constexpr u32 kSubjectNameMd5Prop = 29;
constexpr u32 kNcryptKeyHandleProp = 78;
constexpr u32 kHcryptprovOrNcryptKeyHandleProp = 79;
constexpr u32 kSignHashCngAlgProp = 89;
constexpr u32 kSubjectPubKeyBitLengthProp = 92;
constexpr u32 kPubKeyCngAlgBitLengthProp = 93;
constexpr u32 kNcryptKeyHandleTransferProp = 99;
constexpr u32 kHcryptprovTransferProp = 100;
constexpr u32 kSha256HashProp = 107;
constexpr u32 kSerializableKeyContextProp = 117;
constexpr u32 kOcspMustStapleProp = 121;
constexpr u32 kPinSha256HashProp = 124;
constexpr u32 kSha1Sha256HashProp = 129;
constexpr u32 kLastUserProp = 0xFFFF;

constexpr u32 kSetIgnorePersistErrorFlag = 0x80000000u;
constexpr u32 kSetInhibitPersistFlag = 0x40000000u;

constexpr u32 kAccessStateSystemStore = 0x2;
constexpr u32 kAccessStateLmSystemStore = 0x4;

constexpr const char* kEnhancedKeyUsageOid = "2.5.29.37";

std::vector<u8> sha1(std::span<const u8> data) {
    std::vector<u8> out(CC_SHA1_DIGEST_LENGTH);
    CC_SHA1(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
}
std::vector<u8> sha256(std::span<const u8> data) {
    std::vector<u8> out(CC_SHA256_DIGEST_LENGTH);
    CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
}
std::vector<u8> sha384(std::span<const u8> data) {
    std::vector<u8> out(CC_SHA384_DIGEST_LENGTH);
    CC_SHA384(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
}
std::vector<u8> sha512(std::span<const u8> data) {
    std::vector<u8> out(CC_SHA512_DIGEST_LENGTH);
    CC_SHA512(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
std::vector<u8> md5(std::span<const u8> data) {
    // MD5 is what Windows defines these properties as; not used for trust.
    std::vector<u8> out(CC_MD5_DIGEST_LENGTH);
    CC_MD5(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
}
#pragma clang diagnostic pop

std::vector<u8> utf16z(std::string_view ascii) {
    std::vector<u8> out;
    for (const char c : ascii) {
        out.push_back(static_cast<u8>(c));
        out.push_back(0);
    }
    out.push_back(0);
    out.push_back(0);
    return out;
}

/// Signature algorithm OID -> CNG key algorithm and hash names.
struct SignatureAlgorithm {
    const char* oid;
    const char* key;
    const char* hash;
};
constexpr SignatureAlgorithm kSignatureAlgorithms[] = {
    {"1.2.840.113549.1.1.4", "RSA", "MD5"},
    {"1.2.840.113549.1.1.5", "RSA", "SHA1"},
    {"1.3.14.3.2.29", "RSA", "SHA1"},
    {"1.2.840.113549.1.1.11", "RSA", "SHA256"},
    {"1.2.840.113549.1.1.12", "RSA", "SHA384"},
    {"1.2.840.113549.1.1.13", "RSA", "SHA512"},
    {"1.2.840.10045.4.1", "ECDSA", "SHA1"},
    {"1.2.840.10045.4.3.2", "ECDSA", "SHA256"},
    {"1.2.840.10045.4.3.3", "ECDSA", "SHA384"},
    {"1.2.840.10045.4.3.4", "ECDSA", "SHA512"},
    {"1.2.840.10040.4.3", "DSA", "SHA1"},
};

const SignatureAlgorithm* signature_algorithm(const std::string& oid) {
    for (const auto& entry : kSignatureAlgorithms) {
        if (oid == entry.oid) return &entry;
    }
    return nullptr;
}

std::vector<u8> hash_named(std::string_view name, std::span<const u8> data) {
    if (name == "MD5") return md5(data);
    if (name == "SHA1") return sha1(data);
    if (name == "SHA256") return sha256(data);
    if (name == "SHA384") return sha384(data);
    return sha512(data);
}

const char* public_key_algorithm_name(const std::string& oid) {
    if (oid == "1.2.840.113549.1.1.1") return "RSA";
    if (oid == "1.2.840.10045.2.1") return "ECDSA";
    if (oid == "1.2.840.10040.4.1") return "DSA";
    return nullptr;
}

/// Outcome of computing an implicit property.
struct Computed {
    enum class Kind : u8 { NotComputed, Value, Error } kind{Kind::NotComputed};
    std::vector<u8> value;
    u32 error{0};
    bool clears_last_error{false};  // measured: first computation sets 0
    bool cache{true};
};

Computed compute_property(u32 id, const std::vector<u8>& der,
                          const x509::ParsedCertificate& parsed) {
    Computed out;
    auto value = [&](std::vector<u8> bytes, bool clears = false) {
        out.kind = Computed::Kind::Value;
        out.value = std::move(bytes);
        out.clears_last_error = clears;
        return out;
    };
    auto error = [&](u32 code) {
        out.kind = Computed::Kind::Error;
        out.error = code;
        return out;
    };
    switch (id) {
    case kSha1HashProp: return value(sha1(der));
    case kMd5HashProp: return value(md5(der));
    case kSha256HashProp: return value(sha256(der));
    case kSha1Sha256HashProp: {
        std::vector<u8> both = sha1(der);
        const std::vector<u8> second = sha256(der);
        both.insert(both.end(), second.begin(), second.end());
        return value(std::move(both));
    }
    case kSignatureHashProp: {
        const auto* alg = signature_algorithm(parsed.signature_algorithm.oid);
        if (alg == nullptr) {
            crypt32::report_unimplemented(
                "crypt32!CertGetCertificateContextProperty",
                "CERT_SIGNATURE_HASH_PROP_ID for this signature algorithm");
            return error(crypt32::err::kNteBadAlgid);
        }
        return value(hash_named(alg->hash, parsed.tbs_der), true);
    }
    case kKeyIdentifierProp: {
        std::vector<u8> identifier = x509::certificate_key_identifier(parsed);
        // No subjectKeyIdentifier: SHA-1 of the encoded SubjectPublicKeyInfo
        // (measured with kProbeLegacyNoSkiDer).
        if (identifier.empty()) identifier = sha1(parsed.spki_der);
        return value(std::move(identifier));
    }
    case kSubjectPublicKeyMd5Prop: return value(md5(parsed.spki.key));
    case kIssuerSerialMd5Prop: {
        std::vector<u8> input = parsed.issuer_der;
        input.insert(input.end(), parsed.serial.rbegin(), parsed.serial.rend());
        return value(md5(input));
    }
    case kSubjectNameMd5Prop: return value(md5(parsed.subject_der));
    case kPinSha256HashProp: return value(sha256(parsed.spki_der));
    case kSignHashCngAlgProp: {
        const auto* alg = signature_algorithm(parsed.signature_algorithm.oid);
        if (alg == nullptr) {
            crypt32::report_unimplemented(
                "crypt32!CertGetCertificateContextProperty",
                "CERT_SIGN_HASH_CNG_ALG_PROP_ID for this signature algorithm");
            return error(crypt32::err::kNteBadAlgid);
        }
        return value(utf16z(std::string(alg->key) + "/" + alg->hash), true);
    }
    case kSubjectPubKeyBitLengthProp:
    case kPubKeyCngAlgBitLengthProp: {
        const u32 bits = x509::public_key_bit_length(parsed.spki);
        const char* name = public_key_algorithm_name(parsed.spki.algorithm.oid);
        if (bits == 0 || name == nullptr) {
            crypt32::report_unimplemented(
                "crypt32!CertGetCertificateContextProperty",
                "public key bit length for this key algorithm");
            return error(crypt32::err::kNteBadAlgid);
        }
        if (id == kSubjectPubKeyBitLengthProp) {
            return value({static_cast<u8>(bits), static_cast<u8>(bits >> 8),
                          static_cast<u8>(bits >> 16), static_cast<u8>(bits >> 24)},
                         true);
        }
        return value(utf16z(std::string(name) + "/" + std::to_string(bits)), true);
    }
    case kOcspMustStapleProp: {
        // TLS Feature extension (RFC 7633) listing status_request (5).
        u8 must_staple = 0;
        for (const auto& extension : parsed.extensions) {
            if (extension.oid != "1.3.6.1.5.5.7.1.24") continue;
            const auto& v = extension.value;
            for (usize i = 2; i + 2 < v.size() + 1; ++i) {
                if (v[i - 2] == 0x02 && v[i - 1] == 0x01 && v[i] == 0x05) must_staple = 1;
            }
        }
        return value({must_staple});
    }
    default:
        return out;
    }
}

} // namespace

std::optional<std::vector<u8>> crypt32::compute_certificate_property(
    u32 id, const std::vector<u8>& der, const x509::ParsedCertificate& parsed) {
    Computed computed = compute_property(id, der, parsed);
    if (computed.kind != Computed::Kind::Value) return std::nullopt;
    return computed.value;
}

// BOOL CertGetCertificateContextProperty(PCCERT_CONTEXT pCertContext,
//     DWORD dwPropId, void* pvData, DWORD* pcbData)
u32 Crypt32Manager::cert_get_certificate_context_property(Marshaler& m) {
    const GuestAddr context = m.arg_ptr(0);
    const u32 id = m.arg32(1);
    const GuestAddr data = m.arg_ptr(2);
    const GuestAddr size_ptr = m.arg_ptr(3);
    const bool is64 = m.cpu_is_64bit();

    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_cert_contexts.find(context);
    if (found == m_cert_contexts.end() || size_ptr == 0) {
        set_last_error(kEInvalidArg);
        return 0;
    }
    CertEntry& entry = found->second;

    std::vector<u8> value;
    bool have = false;
    std::optional<u32> error_after;  // last error Windows leaves on success
    auto not_found = [&]() -> u32 {
        (void)m.write_u32(size_ptr, 0);
        set_last_error(kCryptENotFound);
        return 0;
    };

    if (id == kKeyProvInfoProp) {
        if (!entry.props->key_prov_info) return not_found();
        // CRYPT_KEY_PROV_INFO, then the CRYPT_KEY_PROV_PARAM array, each
        // parameter's data (padded to 8), the container and provider names —
        // pointers relative to the caller's buffer (crypt_prop_probe get2p).
        const auto& info = *entry.props->key_prov_info;
        crypt32::GuestBlock block(is64);
        const u32 ps = block.pointer_size();
        const u32 base = block.reserve(is64 ? 48u : 28u, 1);
        block.put_u32(base + 2 * ps, info.prov_type);
        block.put_u32(base + 2 * ps + 4, info.flags);
        block.put_u32(base + 2 * ps + 8, static_cast<u32>(info.params.size()));
        block.put_u32(base + (is64 ? 40u : 24u), info.key_spec);
        const u32 param_size = is64 ? 24u : 16u;
        if (!info.params.empty()) {
            const u32 array = block.reserve(
                static_cast<u32>(info.params.size()) * param_size, 1);
            block.put_ptr(base + (is64 ? 32u : 20u), array);
            for (usize i = 0; i < info.params.size(); ++i) {
                const auto& param = info.params[i];
                const u32 at = array + static_cast<u32>(i) * param_size;
                block.put_u32(at, param.param);
                block.put_u32(at + 2 * ps, static_cast<u32>(param.data.size()));
                block.put_u32(at + 2 * ps + 4, param.flags);
                if (!param.data.empty()) {
                    const u32 bytes = block.add_bytes(param.data, 1);
                    block.reserve(((static_cast<u32>(param.data.size()) + 7u) & ~7u) -
                                      static_cast<u32>(param.data.size()), 1);
                    block.put_ptr(at + ps, bytes);
                }
            }
        }
        auto wide = [&](const std::u16string& text) {
            return block.add_wstring(
                std::span<const u16>(reinterpret_cast<const u16*>(text.data()),
                                     text.size()), 1);
        };
        if (info.has_container) block.put_ptr(base, wide(info.container));
        if (info.has_provider) block.put_ptr(base + ps, wide(info.provider));
        const u32 needed = block.size();
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        (void)m.write_u32(size_ptr, needed);
        if (data == 0) return 1;
        if (capacity < needed) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            return 0;
        }
        const auto image = block.resolve(data);
        (void)m.memory().write_bytes(data, reinterpret_cast<const Byte*>(image.data()),
                                     image.size());
        return 1;
    }

    if (id == kKeySpecProp) {
        if (entry.props->key_context_spec) {
            const u32 spec = *entry.props->key_context_spec;
            value = {static_cast<u8>(spec), static_cast<u8>(spec >> 8),
                     static_cast<u8>(spec >> 16), static_cast<u8>(spec >> 24)};
            have = true;
        } else if (entry.props->key_prov_info) {
            // Falls back to CRYPT_KEY_PROV_INFO.dwKeySpec after the
            // CERT_KEY_CONTEXT lookup failed, which leaves its error behind.
            const u32 spec = entry.props->key_prov_info->key_spec;
            value = {static_cast<u8>(spec), static_cast<u8>(spec >> 8),
                     static_cast<u8>(spec >> 16), static_cast<u8>(spec >> 24)};
            have = true;
            error_after = kCryptENotFound;
        }
    } else if (id == kKeyContextProp) {
        if (entry.props->key_context_spec) {
            // CERT_KEY_CONTEXT { DWORD cbSize; HCRYPTPROV hCryptProv; DWORD dwKeySpec; }
            value.assign(is64 ? 24u : 12u, 0);
            value[0] = static_cast<u8>(value.size());
            const u32 spec = *entry.props->key_context_spec;
            const usize at = is64 ? 16u : 8u;
            for (u32 i = 0; i < 4; ++i) value[at + i] = static_cast<u8>(spec >> (8 * i));
            have = true;
        }
    } else if (id == kAccessStateProp) {
        u32 state = 0;
        auto system = m_system_store_names.find(entry.store_handle);
        if (system != m_system_store_names.end()) {
            state |= kAccessStateSystemStore;
            if (system->second.rfind("LocalMachine\\", 0) == 0) {
                state |= kAccessStateLmSystemStore;
            }
        }
        value = {static_cast<u8>(state), static_cast<u8>(state >> 8),
                 static_cast<u8>(state >> 16), static_cast<u8>(state >> 24)};
        have = true;
    } else if (auto stored = entry.props->blobs.find(id);
               stored != entry.props->blobs.end()) {
        value = stored->second;
        have = true;
    } else {
        auto parsed = x509::parse_certificate(entry.der_data);
        if (parsed) {
            Computed computed = compute_property(id, entry.der_data, *parsed);
            if (computed.kind == Computed::Kind::Error) {
                set_last_error(computed.error);
                return 0;
            }
            if (computed.kind == Computed::Kind::Value) {
                value = std::move(computed.value);
                have = true;
                if (computed.clears_last_error) error_after = 0;
                // Windows caches a computed property on the context.
                entry.props->blobs[id] = value;
            }
        }
    }
    if (!have) return not_found();

    const u32 needed = static_cast<u32>(value.size());
    const u32 capacity = m.read_u32(size_ptr).value_or(0);
    (void)m.write_u32(size_ptr, needed);
    if (error_after) set_last_error(*error_after);
    if (data == 0) return 1;
    if (capacity < needed) {
        set_last_error(error_codes::ERROR_MORE_DATA);
        return 0;
    }
    if (!value.empty() &&
        m.memory().write_bytes(data, reinterpret_cast<const Byte*>(value.data()),
                               value.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return 0;
    }
    return 1;
}

// BOOL CertSetCertificateContextProperty(PCCERT_CONTEXT pCertContext,
//     DWORD dwPropId, DWORD dwFlags, const void* pvData)
u32 Crypt32Manager::cert_set_certificate_context_property(Marshaler& m) {
    const GuestAddr context = m.arg_ptr(0);
    const u32 id = m.arg32(1);
    const u32 flags = m.arg32(2);
    const GuestAddr data = m.arg_ptr(3);
    const bool is64 = m.cpu_is_64bit();
    constexpr const char* kApi = "crypt32!CertSetCertificateContextProperty";

    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_cert_contexts.find(context);
    if (found == m_cert_contexts.end() || id == 0 || id > kLastUserProp) {
        set_last_error(kEInvalidArg);
        return 0;
    }
    CertEntry& entry = found->second;

    // A property on a context in a system store is persisted with the store
    // unless the caller inhibits that. Vitreo's capsule stores keep only the
    // encoded certificates, so the write fails the way a provider-write error
    // does, unless the caller asked for such errors to be ignored.
    if (m_system_store_names.contains(entry.store_handle) &&
        (flags & (kSetInhibitPersistFlag | kSetIgnorePersistErrorFlag)) == 0) {
        crypt32::report_unimplemented(kApi, "persisting a property to a system store");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    if (id == kKeyProvHandleProp || id == kNcryptKeyHandleProp ||
        id == kHcryptprovOrNcryptKeyHandleProp || id == kNcryptKeyHandleTransferProp ||
        id == kHcryptprovTransferProp || id == kSerializableKeyContextProp) {
        if (data == 0) return 1;  // deleting a key handle that was never kept
        crypt32::report_unimplemented(kApi, "private-key handle properties");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    if (data == 0) {
        entry.props->blobs.erase(id);
        if (id == kKeyProvInfoProp) entry.props->key_prov_info.reset();
        if (id == kKeyContextProp || id == kKeySpecProp) entry.props->key_context_spec.reset();
        return 1;
    }
    if (id == kAccessStateProp) return 1;  // computed; a set is accepted and ignored

    if (id == kKeySpecProp) {
        const auto spec = m.read_u32(data);
        if (spec.is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return 0;
        }
        entry.props->key_context_spec = spec.value();
        return 1;
    }
    if (id == kKeyContextProp) {
        const u32 expected = is64 ? 24u : 12u;
        const u32 size = m.read_u32(data).value_or(0);
        if (size != expected) {
            set_last_error(kEInvalidArg);
            return 0;
        }
        const u64 handle = m.read_ptr(data + (is64 ? 8u : 4u)).value_or(0);
        if (handle != 0) {
            crypt32::report_unimplemented(kApi, "CERT_KEY_CONTEXT with a key handle");
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            return 0;
        }
        entry.props->key_context_spec = m.read_u32(data + (is64 ? 16u : 8u)).value_or(0);
        return 1;
    }
    if (id == kKeyProvInfoProp) {
        // CRYPT_KEY_PROV_INFO (x86 28 bytes, x64 48).
        const u32 ps = is64 ? 8u : 4u;
        KeyProvInfo info;
        auto read_wide = [&](GuestAddr at, bool& present, std::u16string& out) {
            const GuestAddr text = static_cast<GuestAddr>(m.read_ptr(at).value_or(0));
            present = text != 0;
            for (GuestAddr p = text; present; p += 2) {
                const auto unit = m.read_u16(p);
                if (unit.is_err() || unit.value() == 0) break;
                out.push_back(static_cast<char16_t>(unit.value()));
            }
        };
        read_wide(data, info.has_container, info.container);
        read_wide(data + ps, info.has_provider, info.provider);
        info.prov_type = m.read_u32(data + 2 * ps).value_or(0);
        info.flags = m.read_u32(data + 2 * ps + 4).value_or(0);
        const u32 count = m.read_u32(data + 2 * ps + 8).value_or(0);
        const GuestAddr params =
            static_cast<GuestAddr>(m.read_ptr(data + (is64 ? 32u : 20u)).value_or(0));
        info.key_spec = m.read_u32(data + (is64 ? 40u : 24u)).value_or(0);
        const u32 param_size = is64 ? 24u : 16u;
        for (u32 i = 0; i < count && params != 0; ++i) {
            const GuestAddr at = params + static_cast<GuestAddr>(i) * param_size;
            KeyProvInfo::Param param;
            param.param = m.read_u32(at).value_or(0);
            const GuestAddr bytes = static_cast<GuestAddr>(m.read_ptr(at + ps).value_or(0));
            const u32 size = m.read_u32(at + 2 * ps).value_or(0);
            param.flags = m.read_u32(at + 2 * ps + 4).value_or(0);
            param.data.resize(size);
            if (size != 0 && m.memory().read_bytes(
                    bytes, reinterpret_cast<Byte*>(param.data.data()), size).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return 0;
            }
            info.params.push_back(std::move(param));
        }
        entry.props->key_prov_info = std::move(info);
        return 1;
    }

    // Every other id — including CERT_DATE_STAMP_PROP_ID, which Learn lists
    // as a FILETIME but Windows reads as a blob (a FILETIME faults there) —
    // is a CRYPT_DATA_BLOB.
    const u32 size = m.read_u32(data).value_or(0);
    const GuestAddr bytes =
        static_cast<GuestAddr>(m.read_ptr(data + (is64 ? 8u : 4u)).value_or(0));
    std::vector<u8> value(size);
    if (size != 0 && m.memory().read_bytes(
            bytes, reinterpret_cast<Byte*>(value.data()), size).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return 0;
    }
    entry.props->blobs[id] = std::move(value);
    return 1;
}

namespace {

/// CERT_ENHKEY_USAGE sizes as Windows reports them: `decoded_size` is what
/// CryptDecodeObject(X509_ENHANCED_KEY_USAGE) needs (pointer array and each
/// string rounded to 8); `packed_size` is the combined-mode result size.
u32 usage_header(bool is64) { return is64 ? 16u : 8u; }
u32 decoded_size(const std::vector<std::string>& oids, bool is64) {
    const u32 ps = is64 ? 8u : 4u;
    u32 size = usage_header(is64) + ((static_cast<u32>(oids.size()) * ps + 7u) & ~7u);
    for (const auto& oid : oids) size += (static_cast<u32>(oid.size()) + 1u + 7u) & ~7u;
    return size;
}
u32 packed_size(const std::vector<std::string>& oids, bool is64) {
    u32 size = usage_header(is64) + static_cast<u32>(oids.size()) * (is64 ? 8u : 4u);
    for (const auto& oid : oids) size += static_cast<u32>(oid.size()) + 1u;
    return size;
}

bool write_usage(Marshaler& m, GuestAddr out, const std::vector<std::string>& oids) {
    crypt32::GuestBlock block(m.cpu_is_64bit());
    const u32 ps = block.pointer_size();
    const u32 header = block.reserve(usage_header(block.is64()), 1);
    block.put_u32(header, static_cast<u32>(oids.size()));
    if (!oids.empty()) {
        const u32 array = block.reserve(static_cast<u32>(oids.size()) * ps, 1);
        block.put_ptr(header + ps, array);
        for (usize i = 0; i < oids.size(); ++i) {
            block.put_ptr(array + static_cast<u32>(i) * ps, block.add_cstring(oids[i]));
        }
    }
    const auto image = block.resolve(out);
    return m.memory().write_bytes(out, reinterpret_cast<const Byte*>(image.data()),
                                  image.size()).is_ok();
}

} // namespace

// BOOL CertGetEnhancedKeyUsage(PCCERT_CONTEXT pCertContext, DWORD dwFlags,
//     PCERT_ENHKEY_USAGE pUsage, DWORD* pcbUsage)
u32 Crypt32Manager::cert_get_enhanced_key_usage(Marshaler& m) {
    constexpr u32 kExtOnly = 0x2;   // CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG
    constexpr u32 kPropOnly = 0x4;  // CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG
    const GuestAddr context = m.arg_ptr(0);
    const u32 flags = m.arg32(1);
    const GuestAddr usage = m.arg_ptr(2);
    const GuestAddr size_ptr = m.arg_ptr(3);
    const bool is64 = m.cpu_is_64bit();

    std::vector<u8> property;
    bool have_property = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_cert_contexts.find(context);
        if (found == m_cert_contexts.end() || size_ptr == 0) {
            set_last_error(kEInvalidArg);
            return 0;
        }
        auto stored = found->second.props->blobs.find(kEnhKeyUsageProp);
        if (stored != found->second.props->blobs.end()) {
            property = stored->second;
            have_property = true;
        }
    }
    set_last_error(0);

    // The extension is read from the context's CERT_INFO as the caller sees
    // it (the probe edits cExtension/rgExtension in place).
    const crypt32::CertLayout l(is64);
    const GuestAddr info =
        static_cast<GuestAddr>(m.read_ptr(context + 3 * l.ptr).value_or(0));
    const u32 count = m.read_u32(info + l.c_ext).value_or(0);
    const GuestAddr extensions =
        static_cast<GuestAddr>(m.read_ptr(info + l.rg_ext).value_or(0));
    std::vector<u8> extension;
    bool have_extension = false;
    for (u32 i = 0; i < count && extensions != 0 && !have_extension; ++i) {
        const GuestAddr entry = extensions + static_cast<GuestAddr>(i) * l.ext_size;
        const auto oid = m.read_ansi_string(
            static_cast<GuestAddr>(m.read_ptr(entry).value_or(0)), 64);
        if (oid.is_err() || oid.value() != kEnhancedKeyUsageOid) continue;
        const u32 size = m.read_u32(entry + 2 * l.ptr).value_or(0);
        const GuestAddr bytes =
            static_cast<GuestAddr>(m.read_ptr(entry + 3 * l.ptr).value_or(0));
        extension.resize(size);
        if (size != 0 && m.memory().read_bytes(
                bytes, reinterpret_cast<Byte*>(extension.data()), size).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return 0;
        }
        have_extension = true;
    }

    std::vector<std::string> ext_oids;
    std::vector<std::string> prop_oids;
    const bool ext_wanted = (flags & kPropOnly) == 0 || (flags & kExtOnly) != 0;
    const bool prop_wanted = (flags & kExtOnly) == 0 || (flags & kPropOnly) != 0;
    if (have_extension && ext_wanted) {
        if (const u32 error = x509::decode_enhanced_key_usage(extension, ext_oids)) {
            set_last_error(error);
            return 0;
        }
    }
    if (have_property && prop_wanted) {
        if (const u32 error = x509::decode_enhanced_key_usage(property, prop_oids)) {
            set_last_error(error);
            return 0;
        }
    }

    const u32 capacity = m.read_u32(size_ptr).value_or(0);
    auto emit = [&](const std::vector<std::string>& oids, u32 needed,
                    bool update_size_on_success) -> u32 {
        if (usage == 0) {
            (void)m.write_u32(size_ptr, needed);
            return 1;
        }
        if (capacity < needed) {
            (void)m.write_u32(size_ptr, needed);
            set_last_error(error_codes::ERROR_MORE_DATA);
            return 0;
        }
        if (!write_usage(m, usage, oids)) {
            set_last_error(error_codes::ERROR_NOACCESS);
            return 0;
        }
        if (update_size_on_success) (void)m.write_u32(size_ptr, needed);
        return 1;
    };

    const bool ext_only = (flags & kExtOnly) != 0;
    const bool prop_only = (flags & kPropOnly) != 0;
    if (ext_only || prop_only) {
        // One source (or, with both flags, each source that exists): the
        // decoded CERT_ENHKEY_USAGE, sized as CryptDecodeObject sizes it.
        const bool use_ext = ext_only && have_extension;
        const bool use_prop = prop_only && have_property;
        if (!use_ext && !use_prop) {
            set_last_error(kCryptENotFound);
            return 0;
        }
        if (use_ext && use_prop) {
            std::vector<std::string> all = ext_oids;
            all.insert(all.end(), prop_oids.begin(), prop_oids.end());
            return emit(all, decoded_size(ext_oids, is64) + decoded_size(prop_oids, is64),
                        false);
        }
        const auto& oids = use_ext ? ext_oids : prop_oids;
        return emit(oids, decoded_size(oids, is64), true);
    }

    // Default: the uses valid under both the extension and the property.
    // A missing property leaves CRYPT_E_NOT_FOUND from its lookup; an empty
    // result then reports CRYPT_E_NOT_FOUND for "valid for all uses" (neither
    // source present) and 0 for "valid for no use".
    if (!have_property) set_last_error(kCryptENotFound);
    std::vector<std::string> result;
    const bool all_uses = !have_extension && !have_property;
    if (have_extension && have_property) {
        for (const auto& oid : ext_oids) {
            if (std::find(prop_oids.begin(), prop_oids.end(), oid) != prop_oids.end()) {
                result.push_back(oid);
            }
        }
    } else if (have_extension) {
        result = ext_oids;
    } else if (have_property) {
        result = prop_oids;
    }
    const u32 ok = emit(result, packed_size(result, is64), false);
    if (ok && usage != 0 && result.empty()) {
        set_last_error(all_uses ? kCryptENotFound : 0u);
    }
    return ok;
}

// BOOL CertSetEnhancedKeyUsage(PCCERT_CONTEXT pCertContext,
//     PCERT_ENHKEY_USAGE pUsage)
u32 Crypt32Manager::cert_set_enhanced_key_usage(Marshaler& m) {
    const GuestAddr context = m.arg_ptr(0);
    const GuestAddr usage = m.arg_ptr(1);
    const bool is64 = m.cpu_is_64bit();
    std::vector<u8> encoded;
    if (usage != 0) {
        const u32 count = m.read_u32(usage).value_or(0);
        const GuestAddr array =
            static_cast<GuestAddr>(m.read_ptr(usage + (is64 ? 8u : 4u)).value_or(0));
        std::vector<std::string> oids;
        for (u32 i = 0; i < count; ++i) {
            const auto text = m.read_ansi_string(static_cast<GuestAddr>(
                m.read_ptr(array + static_cast<GuestAddr>(i) * (is64 ? 8u : 4u))
                    .value_or(0)));
            if (text.is_err() || x509::encode_oid(text.value()).empty()) {
                set_last_error(x509::kCryptEBadEncode);
                return 0;
            }
            oids.push_back(text.value());
        }
        encoded = x509::encode_enhanced_key_usage(oids);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_cert_contexts.find(context);
    if (found == m_cert_contexts.end()) {
        set_last_error(kEInvalidArg);
        return 0;
    }
    if (m_system_store_names.contains(found->second.store_handle)) {
        crypt32::report_unimplemented("crypt32!CertSetEnhancedKeyUsage",
                                      "persisting a property to a system store");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    if (usage == 0) {
        found->second.props->blobs.erase(kEnhKeyUsageProp);
    } else {
        found->second.props->blobs[kEnhKeyUsageProp] = std::move(encoded);
    }
    return 1;
}

} // namespace vitreo::winapi
