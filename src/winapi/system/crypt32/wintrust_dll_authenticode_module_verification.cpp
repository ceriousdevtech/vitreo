// SPDX-License-Identifier: LGPL-2.1-only
/// @file wintrust_dll_authenticode_module_verification.cpp
/// @brief wintrust.dll WinVerifyTrust (Authenticode PE / cabinet / MSI verification) and the
///        WTHelper* provider-data accessors.
///
/// Contracts:
///  https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust
///  https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_data
///  wintrust.h / softpub.h (action GUIDs, CRYPT_PROVIDER_* layouts) and the
///  Authenticode PE signature format (image digest ranges).
/// Measured Windows behaviour, x86 and x64: tools/oracle_probe/crypt_wvt_probe.c
/// (+ .expected / .x86.expected):
///  - the result is also the last error; CLOSE leaves the last error alone;
///  - VERIFY always creates state, even for a missing or unsigned file;
///  - an unreadable file is CRYPT_E_FILE_ERROR, a non-PE file
///    TRUST_E_SUBJECT_FORM_UNKNOWN, a PE without signature (or with a
///    signature whose PKCS#7 signer signature or messageDigest does not
///    verify) TRUST_E_NOSIGNATURE, a modified image TRUST_E_BAD_DIGEST, then
///    the chain decides: CERT_E_CHAINING / CERT_E_UNTRUSTEDROOT / ...;
///  - an unknown action is TRUST_E_PROVIDER_UNKNOWN with last error 0; a
///    WINTRUST_DATA too short to hold dwUIChoice returns 1.
/// Subjects: PE images, cabinets (crypt_cab_wvt.expected) and structured
/// storage (Windows Installer .msi/.msp/.mst, [MS-CFB]); the structured-storage
/// digest rules are in authenticode.hpp (storage_digest_input). Not measured
/// on winoracle yet: the result for a file with the compound-file signature
/// that is not a readable compound file (TRUST_E_SUBJECT_FORM_UNKNOWN here).

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/guest_block.hpp"
#include "authenticode.hpp"
#include "x509_parse.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "ipc/ipc_gate.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>

namespace vitreo::winapi {

namespace {

struct Guid {
    u32 d1;
    u16 d2, d3;
    std::array<u8, 8> d4;
    bool operator==(const Guid&) const = default;
};
// softpub.h / wintrust.h
constexpr Guid kGenericVerifyV2{0x00aac56b, 0xcd44, 0x11d0, {0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}};
constexpr Guid kGenericChainVerify{0xfc451c16, 0xac75, 0x11d1, {0xb4, 0xb8, 0x00, 0xc0, 0x4f, 0xb6, 0x6e, 0xa0}};
constexpr Guid kPublishedSoftware{0x64b9d180, 0x8da2, 0x11cf, {0x87, 0x36, 0x00, 0xaa, 0x00, 0xa4, 0x85, 0xeb}};
constexpr Guid kKnownUnimplemented[] = {
    {0x573e31f8, 0xddba, 0x11d0, {0x8c, 0xcb, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},  // TRUSTPROVIDER_TEST
    {0x189a3842, 0x3041, 0x11d1, {0x85, 0xe1, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},  // GENERIC_CERT_VERIFY
    {0x573e31f8, 0xaaba, 0x11d0, {0x8c, 0xcb, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},  // HTTPSPROV_ACTION
    {0x5555c2cd, 0x17fb, 0x11d1, {0x85, 0xc4, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},  // OFFICESIGN_ACTION_VERIFY
    {0xf750e6c3, 0x38ee, 0x11d1, {0x85, 0xe5, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee}},  // DRIVER_ACTION_VERIFY
    {0x66426730, 0x8da1, 0x11cf, {0x87, 0x36, 0x00, 0xaa, 0x00, 0xa4, 0x85, 0xeb}},  // TRUSTED_PUBLISHER
    {0x8bc96b00, 0x8da1, 0x11cf, {0x87, 0x36, 0x00, 0xaa, 0x00, 0xa4, 0x85, 0xeb}},  // NT_ACTIVATE_IMAGE
};

// winerror.h
constexpr u32 kTrustEProviderUnknown = 0x800B0001u;
constexpr u32 kTrustESubjectFormUnknown = 0x800B0003u;
constexpr u32 kTrustENoSignature = 0x800B0100u;
constexpr u32 kTrustEBadDigest = 0x80096010u;
constexpr u32 kTrustECertSignature = 0x80096004u;
constexpr u32 kTrustENoSignerCert = 0x800B010Cu;  // also CERT_E_REVOKED's value
constexpr u32 kCertEExpired = 0x800B0101u;
constexpr u32 kCertERevoked = 0x800B010Cu;
constexpr u32 kCertEUntrustedRoot = 0x800B0109u;
constexpr u32 kCertEChaining = 0x800B010Au;
constexpr u32 kCertEWrongUsage = 0x800B0110u;
constexpr u32 kCryptEFileError = 0x80092003u;
constexpr u32 kCryptEHashValue = 0x80091007u;
constexpr u32 kStatusInvalidSignature = 0xC000A000u;

// WINTRUST_DATA
constexpr u32 kStateActionIgnore = 0;
constexpr u32 kStateActionVerify = 1;
constexpr u32 kStateActionClose = 2;
constexpr u32 kStateActionAutoCache = 3;
constexpr u32 kStateActionAutoCacheFlush = 4;
constexpr u32 kChoiceFile = 1;
constexpr u32 kRevokeWholeChain = 1;
constexpr u32 kSgnrTypeTimestamp = 0x10;
constexpr u32 kConfidenceSig = 0x10000000u;
constexpr u32 kConfidenceTime = 0x01000000u;
constexpr u32 kConfidenceTimeNest = 0x00100000u;
constexpr u32 kCpdUseNt5ChainFlag = 0x80000000u;

// CERT_TRUST_STATUS error bits used by the Authenticode policy.
constexpr u32 kNotTimeValid = 0x1;
constexpr u32 kIsRevoked = 0x4;
constexpr u32 kNotSignatureValid = 0x8;
constexpr u32 kNotValidForUsage = 0x10;
constexpr u32 kUntrustedRoot = 0x20;
constexpr u32 kPartialChain = 0x10000;

constexpr const char* kCodeSigning = "1.3.6.1.5.5.7.3.3";
constexpr const char* kTimeStamping = "1.3.6.1.5.5.7.3.8";

u64 current_filetime() {
    return (static_cast<u64>(std::time(nullptr)) + 11644473600ULL) * 10000000ULL;
}

const char* hash_name(const std::string& oid) {
    if (oid == "1.3.14.3.2.26") return "SHA1";
    if (oid == "2.16.840.1.101.3.4.2.1") return "SHA256";
    if (oid == "2.16.840.1.101.3.4.2.2") return "SHA384";
    if (oid == "2.16.840.1.101.3.4.2.3") return "SHA512";
    return nullptr;
}

std::vector<u8> digest(std::string_view name, std::span<const std::span<const u8>> parts) {
    std::vector<u8> out;
    if (name == "SHA1") {
        CC_SHA1_CTX c; CC_SHA1_Init(&c);
        for (auto p : parts) CC_SHA1_Update(&c, p.data(), static_cast<CC_LONG>(p.size()));
        out.resize(CC_SHA1_DIGEST_LENGTH); CC_SHA1_Final(out.data(), &c);
    } else if (name == "SHA256") {
        CC_SHA256_CTX c; CC_SHA256_Init(&c);
        for (auto p : parts) CC_SHA256_Update(&c, p.data(), static_cast<CC_LONG>(p.size()));
        out.resize(CC_SHA256_DIGEST_LENGTH); CC_SHA256_Final(out.data(), &c);
    } else if (name == "SHA384") {
        CC_SHA512_CTX c; CC_SHA384_Init(&c);
        for (auto p : parts) CC_SHA384_Update(&c, p.data(), static_cast<CC_LONG>(p.size()));
        out.resize(CC_SHA384_DIGEST_LENGTH); CC_SHA384_Final(out.data(), &c);
    } else if (name == "SHA512") {
        CC_SHA512_CTX c; CC_SHA512_Init(&c);
        for (auto p : parts) CC_SHA512_Update(&c, p.data(), static_cast<CC_LONG>(p.size()));
        out.resize(CC_SHA512_DIGEST_LENGTH); CC_SHA512_Final(out.data(), &c);
    }
    return out;
}

std::vector<u8> digest_one(std::string_view name, std::span<const u8> data) {
    const std::span<const u8> parts[] = {data};
    return digest(name, parts);
}

/// Minimal DER reader for the few Authenticode structures parsed here.
struct Der {
    std::span<const u8> data;
    usize pos{0};
    bool next(u8& tag, std::span<const u8>& value) {
        if (data.size() - pos < 2) return false;
        tag = data[pos++];
        usize len = data[pos++];
        if (len & 0x80) {
            const usize n = len & 0x7f;
            if (n == 0 || n > 4 || data.size() - pos < n) return false;
            len = 0;
            for (usize i = 0; i < n; ++i) len = (len << 8) | data[pos++];
        }
        if (data.size() - pos < len) return false;
        value = data.subspan(pos, len);
        pos += len;
        return true;
    }
};

std::string oid_of(std::span<const u8> value) { return x509::oid_to_string(value); }

/// UTCTime / GeneralizedTime (optionally with fractional seconds) -> FILETIME.
bool asn1_time(u8 tag, std::span<const u8> text, u64& out) {
    auto digits = [&](usize at, usize n, int& v) {
        if (at + n > text.size()) return false;
        v = 0;
        for (usize i = 0; i < n; ++i) {
            if (text[at + i] < '0' || text[at + i] > '9') return false;
            v = v * 10 + (text[at + i] - '0');
        }
        return true;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    usize at = 0;
    if (tag == 0x17) {
        if (!digits(0, 2, year)) return false;
        year += year < 50 ? 2000 : 1900;
        at = 2;
    } else if (tag == 0x18) {
        if (!digits(0, 4, year)) return false;
        at = 4;
    } else {
        return false;
    }
    if (!digits(at, 2, month) || !digits(at + 2, 2, day) || !digits(at + 4, 2, hour) ||
        !digits(at + 6, 2, minute) || !digits(at + 8, 2, second)) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900; tm.tm_mon = month - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = second;
    const time_t unix_time = timegm(&tm);
    if (unix_time == static_cast<time_t>(-1)) return false;
    out = (static_cast<u64>(unix_time) + 11644473600ULL) * 10000000ULL;
    return true;
}

const authenticode::SignerInfo::Attribute* find_attribute(
    const std::vector<authenticode::SignerInfo::Attribute>& list, const char* oid) {
    for (const auto& a : list) if (a.oid == oid) return &a;
    return nullptr;
}

/// The single OCTET STRING value of a messageDigest attribute.
std::vector<u8> message_digest_of(const authenticode::SignerInfo& signer) {
    const auto* a = find_attribute(signer.signed_attribute_list, "1.2.840.113549.1.9.4");
    if (a == nullptr || a->values.size() != 1) return {};
    Der d{a->values[0]};
    u8 tag = 0;
    std::span<const u8> value;
    if (!d.next(tag, value) || tag != 0x04) return {};
    return {value.begin(), value.end()};
}

const authenticode::Certificate* signer_certificate(
    const std::vector<authenticode::Certificate>& certificates,
    const authenticode::SignerInfo& signer) {
    std::vector<u8> serial = signer.serial;
    while (serial.size() > 1 && serial.front() == 0) serial.erase(serial.begin());
    for (const auto& c : certificates) {
        if (c.issuer == signer.issuer && c.serial == serial) return &c;
    }
    return nullptr;
}

/// Verify a SignerInfo's signed attributes: 0 or the signer dwError
/// (measured: STATUS_INVALID_SIGNATURE / CRYPT_E_HASH_VALUE).
u32 check_signer(const authenticode::SignerInfo& signer, const x509::ParsedCertificate& cert,
                 std::span<const u8> signed_content) {
    const char* hash = hash_name(signer.digest_oid);
    if (hash == nullptr || !signer.has_signed_attributes) return kStatusInvalidSignature;
    std::vector<u8> attributes{0x31};
    const usize n = signer.signed_attributes.size();
    if (n < 0x80) {
        attributes.push_back(static_cast<u8>(n));
    } else {
        std::vector<u8> len;
        for (usize v = n; v; v >>= 8) len.insert(len.begin(), static_cast<u8>(v));
        attributes.push_back(static_cast<u8>(0x80 | len.size()));
        attributes.insert(attributes.end(), len.begin(), len.end());
    }
    attributes.insert(attributes.end(), signer.signed_attributes.begin(),
                      signer.signed_attributes.end());
    if (!verify_message_signature(cert.spki, hash, attributes, signer.signature)) {
        return kStatusInvalidSignature;
    }
    if (message_digest_of(signer) != digest_one(hash, signed_content)) return kCryptEHashValue;
    return 0;
}

struct Timestamp {
    bool present{false};
    bool valid{false};
    u64 time{0};
    authenticode::SignerInfo signer;
    std::vector<std::vector<u8>> certificates;
    std::vector<u8> signer_certificate;
};

/// SpcIndirectDataContent -> digest algorithm and value.
bool indirect_digest(std::span<const u8> content, std::string& algorithm, std::vector<u8>& value) {
    Der top{content};
    u8 tag = 0;
    std::span<const u8> body;
    if (!top.next(tag, body) || tag != 0x30) return false;
    Der fields{body};
    std::span<const u8> part;
    if (!fields.next(tag, part) || tag != 0x30) return false;          // data
    if (!fields.next(tag, part) || tag != 0x30) return false;          // DigestInfo
    Der info{part};
    std::span<const u8> alg;
    std::span<const u8> hash;
    if (!info.next(tag, alg) || tag != 0x30 || !info.next(tag, hash) || tag != 0x04) return false;
    Der a{alg};
    std::span<const u8> oid;
    if (!a.next(tag, oid) || tag != 0x06) return false;
    algorithm = oid_of(oid);
    value.assign(hash.begin(), hash.end());
    return true;
}

} // namespace

namespace crypt32 {

u32 verify_signer_attributes(const authenticode::SignerInfo& signer, const x509::ParsedCertificate& cert,
                             std::span<const u8> signed_content) {
    return check_signer(signer, cert, signed_content);
}

const authenticode::Certificate* find_signer_certificate(
    const std::vector<authenticode::Certificate>& certificates, const authenticode::SignerInfo& signer) {
    return signer_certificate(certificates, signer);
}

// CMSG_SIGNER_INFO { dwVersion; Issuer; SerialNumber (little-endian);
// HashAlgorithm; HashEncryptionAlgorithm; EncryptedHash; AuthAttrs;
// UnauthAttrs } with every blob it points at, appended to `block`.
u32 put_cmsg_signer_info(GuestBlock& block, const authenticode::SignerInfo& signer) {
    // Every piece -- the structure, each array and each string or blob --
    // takes a multiple of 8 bytes, as Windows sizes it at both pointer
    // widths (crypt_signer_trio_probe "siparam": 0x270 x86 / 0x2e8 x64 for
    // the cab_signed signer, whose raw pieces sum to less).
    constexpr u32 kAlign = 8;
    const u32 ps = block.pointer_size();
    const u32 at = block.reserve(17 * ps);
    block.put_u32(at + 0, signer.version);
    block.put_blob(at + ps, signer.issuer, kAlign);
    const std::vector<u8> serial(signer.serial.rbegin(), signer.serial.rend());
    block.put_blob(at + 3 * ps, serial, kAlign);
    // The PKCS#7 decoder drops encoded NULL (05 00) algorithm parameters
    // (crypt_msg_probe "signer0" row reports cbData 0 for both).
    auto put_algorithm = [&](u32 field, const std::string& oid, const std::vector<u8>& params) {
        if (!oid.empty()) block.put_ptr(field, block.add_cstring(oid, kAlign));
        const bool null_params = params.size() == 2 && params[0] == 0x05 && params[1] == 0x00;
        if (!null_params) block.put_blob(field + ps, params, kAlign);
    };
    put_algorithm(at + 5 * ps, signer.digest_oid, signer.digest_parameters);
    put_algorithm(at + 8 * ps, signer.signature_oid, signer.signature_parameters);
    block.put_blob(at + 11 * ps, signer.signature, kAlign);
    auto put_attributes = [&](u32 field, const std::vector<authenticode::SignerInfo::Attribute>& list) {
        block.put_u32(field, static_cast<u32>(list.size()));
        if (list.empty()) return;
        const u32 array = block.reserve(static_cast<u32>(list.size()) * 3 * ps);
        block.put_ptr(field + ps, array);
        for (usize i = 0; i < list.size(); ++i) {
            const u32 attribute = array + static_cast<u32>(i) * 3 * ps;
            block.put_ptr(attribute, block.add_cstring(list[i].oid, kAlign));
            block.put_u32(attribute + ps, static_cast<u32>(list[i].values.size()));
            if (list[i].values.empty()) continue;
            const u32 values = block.reserve(static_cast<u32>(list[i].values.size()) * 2 * ps);
            block.put_ptr(attribute + 2 * ps, values);
            for (usize v = 0; v < list[i].values.size(); ++v) {
                block.put_blob(values + static_cast<u32>(v) * 2 * ps, list[i].values[v], kAlign);
            }
        }
    };
    put_attributes(at + 13 * ps, signer.signed_attribute_list);
    put_attributes(at + 15 * ps, signer.unsigned_attribute_list);
    (void)block.reserve(0, kAlign);  // the last piece's padding
    return at;
}

} // namespace crypt32

// =============================================================================

u32 Crypt32Manager::win_verify_trust(Marshaler& m) {
    // LONG WinVerifyTrust(HWND hwnd, GUID* pgActionID, LPVOID pWVTData)
    const u64 hwnd = m.arg(0);
    const GuestAddr action_ptr = m.arg_ptr(1);
    const GuestAddr wd = m.arg_ptr(2);
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;
    constexpr const char* kApi = "wintrust!WinVerifyTrust";

    Guid action{};
    action.d1 = m.read_u32(action_ptr).value_or(0);
    action.d2 = m.read_u16(action_ptr + 4).value_or(0);
    action.d3 = m.read_u16(action_ptr + 6).value_or(0);
    for (u32 i = 0; i < 8; ++i) action.d4[i] = m.read_u8(action_ptr + 8 + i).value_or(0);
    const bool authenticode_policy =
        action == kGenericVerifyV2 || action == kGenericChainVerify || action == kPublishedSoftware;
    if (!authenticode_policy) {
        if (std::find(std::begin(kKnownUnimplemented), std::end(kKnownUnimplemented), action) !=
            std::end(kKnownUnimplemented)) {
            crypt32::report_unimplemented(kApi, "trust provider other than Authenticode (GENERIC_VERIFY_V2)");
        }
        set_last_error(0);
        return kTrustEProviderUnknown;
    }
    if (wd == 0) {
        crypt32::raise_access_violation(m, 0, false);
        return 0;
    }

    // WINTRUST_DATA field offsets (x64 / x86).
    const u32 off_policy_data = ps;
    const u32 off_ui_choice = is64 ? 24u : 12u;
    const u32 off_revocation = off_ui_choice + 4;
    const u32 off_choice = off_ui_choice + 8;
    const u32 off_union = is64 ? 40u : 24u;
    const u32 off_action = off_union + ps;
    const u32 off_state = is64 ? 56u : 32u;
    const u32 off_prov_flags = off_state + 2 * ps;
    const u32 off_signature_settings = off_prov_flags + 8;
    const u32 cb_struct = m.read_u32(wd).value_or(0);
    if (cb_struct < off_ui_choice + 4) {
        set_last_error(1);
        return 1;
    }
    const u32 state_action = m.read_u32(wd + off_action).value_or(0);

    if (state_action == kStateActionClose) {
        const u32 handle = static_cast<u32>(m.read_ptr(wd + off_state).value_or(0));
        WvtState state;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_wvt_states.find(handle);
            if (it != m_wvt_states.end()) {
                state = std::move(it->second);
                m_wvt_states.erase(it);
                m_query_messages.erase(state.message);
                if (state.store != 0) erase_store_locked(state.store);
            }
        }
        if (state.chain != 0 || state.timestamp_chain != 0) {
            // Release the provider's chains and context references.
            std::vector<GuestAddr> blocks;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const GuestAddr chain_arg : {state.chain, state.timestamp_chain}) {
                    if (chain_arg == 0) continue;
                    auto found = m_chains.find(chain_arg);
                    if (found != m_chains.end() && --found->second.references == 0) {
                        for (const GuestAddr c : found->second.contexts) {
                            auto released = release_cert_context_locked(c);
                            blocks.insert(blocks.end(), released.begin(), released.end());
                        }
                        blocks.push_back(found->second.block);
                        m_chains.erase(found);
                    }
                }
                for (const GuestAddr c : state.contexts) {
                    auto released = release_cert_context_locked(c);
                    blocks.insert(blocks.end(), released.begin(), released.end());
                }
            }
            for (const GuestAddr b : blocks) free_guest(b);
        }
        if (state.block != 0) free_guest(state.block);
        (void)m.write_ptr(wd + off_state, 0);
        return 0;
    }
    if (state_action == kStateActionAutoCacheFlush) return 0;  // nothing is cached
    if (state_action != kStateActionIgnore && state_action != kStateActionVerify &&
        state_action != kStateActionAutoCache) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return error_codes::ERROR_INVALID_PARAMETER;
    }

    if (m.read_ptr(wd + off_policy_data).value_or(0) != 0 && action == kGenericChainVerify) {
        crypt32::report_unimplemented(kApi, "WINTRUST_ACTION_GENERIC_CHAIN_VERIFY policy callbacks");
        set_last_error(kTrustEProviderUnknown);
        return kTrustEProviderUnknown;
    }
    const u32 choice = m.read_u32(wd + off_choice).value_or(0);
    if (choice != kChoiceFile) {
        crypt32::report_unimplemented(kApi, "WTD_CHOICE other than WTD_CHOICE_FILE");
        set_last_error(kTrustESubjectFormUnknown);
        return kTrustESubjectFormUnknown;
    }
    const u32 revocation_checks = m.read_u32(wd + off_revocation).value_or(0);
    const u32 prov_flags = m.read_u32(wd + off_prov_flags).value_or(0);
    const GuestAddr signature_settings =
        cb_struct >= off_signature_settings + ps
            ? static_cast<GuestAddr>(m.read_ptr(wd + off_signature_settings).value_or(0))
            : 0;

    // --- read the subject ----------------------------------------------------
    u32 result = 0;
    std::vector<u8> image;
    std::string traced_path;
    {
        const GuestAddr file_info = static_cast<GuestAddr>(m.read_ptr(wd + off_union).value_or(0));
        const GuestAddr path_ptr =
            file_info ? static_cast<GuestAddr>(m.read_ptr(file_info + ps).value_or(0)) : 0;
        if (path_ptr == 0) {
            crypt32::report_unimplemented(kApi, "WINTRUST_FILE_INFO with only hFile");
            result = kCryptEFileError;
        } else {
            auto path = m.read_wide_string(path_ptr, 32768);
            if (path.is_ok()) traced_path = path.value();
            if (!path.is_ok() || m_vfs == nullptr) {
                result = kCryptEFileError;
            } else if (auto info = m_vfs->stat(path.value());
                       info.is_err() || capsule::has_attribute(info.value().attributes,
                                                               capsule::FileAttributes::Directory)) {
                result = kCryptEFileError;
            } else {
                auto opened = m_vfs->open(path.value(), capsule::AccessMode::Read,
                                          capsule::ShareMode::ReadWrite,
                                          capsule::CreationDisposition::OpenExisting);
                if (opened.is_err()) {
                    result = kCryptEFileError;
                } else {
                    image.resize(static_cast<usize>(info.value().size));
                    usize total = 0;
                    while (total < image.size()) {
                        auto r = m_vfs->read(opened.value(), std::span<u8>(image).subspan(total));
                        if (r.is_err() || r.value() == 0) break;
                        total += r.value();
                    }
                    (void)m_vfs->close(opened.value());
                    if (total != image.size()) result = kCryptEFileError;
                }
            }
        }
    }

    // --- signature ------------------------------------------------------------
    std::optional<authenticode::SignedData> signed_data;
    const authenticode::Certificate* signer_cert = nullptr;
    u32 signer_error = 0;
    bool image_digest_ok = false;
    Timestamp timestamp;
    u32 nested_signatures = 0;
    // Subject interface packages: PE images, cabinets and structured storage
    // (Windows Installer packages, patches and transforms). A file with the
    // compound-file header that the CFB reader rejects is not a structured
    // storage the SIP can open, so it is a subject form Windows does not know.
    const bool cab_subject = result == 0 && authenticode::is_cab_image(image);
    const bool storage_subject = result == 0 && authenticode::is_compound_file_image(image);
    std::optional<authenticode::StorageSignature> storage;
    if (storage_subject) {
        auto located = authenticode::locate_storage_signature(image);
        if (located.is_ok()) {
            storage = std::move(located.value());
        } else if (located.error() == authenticode::ParseError::InvalidPe) {
            result = kTrustESubjectFormUnknown;
        }
    }
    if (result == 0 && !cab_subject && !storage_subject && !authenticode::is_pe_image(image)) {
        result = kTrustESubjectFormUnknown;
    }
    if (result == 0) {
        std::vector<u8> pkcs7;
        bool located = false;
        if (storage_subject) {
            if (storage) { pkcs7 = storage->pkcs7; located = true; }
        } else if (cab_subject) {
            auto cab = authenticode::locate_cab_signature(image);
            if (cab.is_ok()) { pkcs7 = std::move(cab.value().pkcs7); located = true; }
        } else {
            auto pe = authenticode::locate_pe_signature(image);
            if (pe.is_ok()) { pkcs7 = std::move(pe.value().pkcs7); located = true; }
        }
        auto parsed = located
            ? authenticode::parse_signed_data(pkcs7)
            : Result<authenticode::SignedData, authenticode::ParseError>::err(
                  authenticode::ParseError::NoSignature);
        std::string algorithm;
        std::vector<u8> expected;
        if (parsed.is_err() || parsed.value().content_type != "1.3.6.1.4.1.311.2.1.4" ||
            !indirect_digest(parsed.value().content, algorithm, expected)) {
            result = kTrustENoSignature;
        } else {
            signed_data = std::move(parsed.value());
            const auto& signer = signed_data->signers.front();
            signer_cert = signer_certificate(signed_data->certificates, signer);
            auto signer_parsed = signer_cert ? x509::parse_certificate(signer_cert->der) : std::nullopt;
            if (!signer_parsed) {
                signer_error = kTrustENoSignerCert;
                result = kTrustENoSignature;
            } else {
                signer_error = check_signer(signer, *signer_parsed, signed_data->content_value);
                if (signer_error != 0) result = kTrustENoSignature;
            }
            const char* hash = hash_name(algorithm);
            if (storage_subject && hash != nullptr) {
                const auto input = authenticode::storage_digest_input(*storage);
                std::vector<u8> metadata_digest;
                std::vector<std::span<const u8>> parts;
                if (input.extended) {
                    metadata_digest = digest_one(hash, input.metadata);
                    parts.emplace_back(metadata_digest);
                }
                parts.insert(parts.end(), input.content.begin(), input.content.end());
                // The stored extended stream must be the metadata digest it
                // stands for (true of every real package checked; how Windows
                // treats a stale one is not measured, so it is not trusted).
                image_digest_ok = digest(hash, parts) == expected &&
                                  (!input.extended || storage->extended == metadata_digest);
            }
            const auto ranges = storage_subject ? std::vector<std::pair<usize, usize>>{}
                              : cab_subject     ? authenticode::cab_digest_ranges(image)
                                                : authenticode::pe_digest_ranges(image);
            if (hash != nullptr && !ranges.empty()) {
                std::vector<std::span<const u8>> parts;
                for (const auto& [begin, end] : ranges) {
                    parts.emplace_back(image.data() + begin, end - begin);
                }
                image_digest_ok = digest(hash, parts) == expected;
            }
            for (const auto& a : signer.unsigned_attribute_list) {
                if (a.oid == "1.3.6.1.4.1.311.2.4.1") nested_signatures += static_cast<u32>(a.values.size());
            }
            // Timestamp: RFC 3161 token or legacy PKCS#9 countersignature.
            for (const auto& a : signer.unsigned_attribute_list) {
                if (a.values.empty()) continue;
                if (a.oid == "1.3.6.1.4.1.311.3.3.1") {
                    timestamp.present = true;
                    auto token = authenticode::parse_signed_data(a.values.front());
                    if (token.is_err() || token.value().content_type != "1.2.840.113549.1.9.16.1.4") break;
                    const auto& ts = token.value();
                    // eContent is an OCTET STRING holding the whole TSTInfo
                    // SEQUENCE { version, policy, messageImprint, serial,
                    // genTime, ... } (RFC 3161 section 2.4.2); the token's
                    // messageDigest covers that full encoding.
                    Der token_content{ts.content_value};
                    u8 tag = 0;
                    std::span<const u8> tst_info;
                    std::span<const u8> v;
                    std::span<const u8> imprint;
                    std::span<const u8> gen_time;
                    bool ok = token_content.next(tag, tst_info) && tag == 0x30;
                    Der tst{tst_info};
                    ok = ok && tst.next(tag, v) && tag == 0x02 && tst.next(tag, v) && tag == 0x06 &&
                         tst.next(tag, imprint) && tag == 0x30 && tst.next(tag, v) &&
                         tag == 0x02 && tst.next(tag, gen_time) && tag == 0x18;
                    u64 when = 0;
                    ok = ok && asn1_time(0x18, gen_time, when);
                    std::string imprint_alg;
                    std::vector<u8> imprint_hash;
                    if (ok) {
                        Der mi{imprint};
                        std::span<const u8> alg;
                        std::span<const u8> hashed;
                        ok = mi.next(tag, alg) && mi.next(tag, hashed) && tag == 0x04;
                        Der ai{alg};
                        std::span<const u8> oid;
                        ok = ok && ai.next(tag, oid) && tag == 0x06;
                        if (ok) {
                            imprint_alg = oid_of(oid);
                            imprint_hash.assign(hashed.begin(), hashed.end());
                        }
                    }
                    const char* imprint_hash_name = hash_name(imprint_alg);
                    ok = ok && imprint_hash_name != nullptr &&
                         imprint_hash == digest_one(imprint_hash_name, signer.signature);
                    const auto* tsa = ok ? signer_certificate(ts.certificates, ts.signers.front()) : nullptr;
                    auto tsa_parsed = tsa ? x509::parse_certificate(tsa->der) : std::nullopt;
                    ok = ok && tsa_parsed &&
                         check_signer(ts.signers.front(), *tsa_parsed, ts.content_value) == 0;
                    if (ok) {
                        timestamp.valid = true;
                        timestamp.time = when;
                        timestamp.signer = ts.signers.front();
                        timestamp.signer_certificate = tsa->der;
                        for (const auto& c : ts.certificates) timestamp.certificates.push_back(c.der);
                    }
                    break;
                }
                if (a.oid == "1.2.840.113549.1.9.6") {
                    timestamp.present = true;
                    auto counter = authenticode::parse_signer_info(a.values.front());
                    if (counter.is_err()) break;
                    const auto& cs = counter.value();
                    const auto* tsa = signer_certificate(signed_data->certificates, cs);
                    auto tsa_parsed = tsa ? x509::parse_certificate(tsa->der) : std::nullopt;
                    u64 when = 0;
                    bool ok = false;
                    if (const auto* st = find_attribute(cs.signed_attribute_list, "1.2.840.113549.1.9.5");
                        st && st->values.size() == 1) {
                        Der d{st->values.front()};
                        u8 tag = 0;
                        std::span<const u8> v;
                        ok = d.next(tag, v) && asn1_time(tag, v, when);
                    }
                    ok = ok && tsa_parsed && check_signer(cs, *tsa_parsed, signer.signature) == 0;
                    if (ok) {
                        timestamp.valid = true;
                        timestamp.time = when;
                        timestamp.signer = cs;
                        timestamp.signer_certificate = tsa->der;
                        for (const auto& c : signed_data->certificates) timestamp.certificates.push_back(c.der);
                    }
                    break;
                }
            }
        }
    }

    // WINTRUST_SIGNATURE_SETTINGS: only the primary signature is modelled.
    if (signature_settings != 0) {
        const u32 index = m.read_u32(signature_settings + 4).value_or(0);
        const u32 flags = m.read_u32(signature_settings + 8).value_or(0);
        const u64 crypto_policy = m.read_ptr(signature_settings + 20 + (is64 ? 4u : 0u)).value_or(0);
        constexpr u32 kVerifySpecific = 0x1;
        constexpr u32 kGetSecondarySigCount = 0x2;
        if (((flags & kVerifySpecific) != 0 && index != 0) || crypto_policy != 0 ||
            (flags & ~(kVerifySpecific | kGetSecondarySigCount)) != 0) {
            crypt32::report_unimplemented(kApi, "WINTRUST_SIGNATURE_SETTINGS beyond the primary signature");
            set_last_error(kTrustEProviderUnknown);
            return kTrustEProviderUnknown;
        }
        if ((flags & kGetSecondarySigCount) != 0) {
            (void)m.write_u32(signature_settings + 12, nested_signatures);
        }
        (void)m.write_u32(signature_settings + 16, 0);  // dwVerifiedSigIndex
    }

    // --- chain ----------------------------------------------------------------
    const u64 now = current_filetime();
    const u64 verify_time = timestamp.valid ? timestamp.time : now;
    ChainResult chain;
    bool have_chain = false;
    // The provider's message store holds every certificate the signature
    // carries, the timestamp token's included: Windows completes
    // socialclub.dll's signer chain through the G4 cross-certificate that
    // only its RFC 3161 token contains (wintrust_signer_name_probe.expected).
    auto signature_certificates = [&] {
        std::vector<std::vector<u8>> certificates;
        for (const auto& c : signed_data->certificates) certificates.push_back(c.der);
        for (const auto& der : timestamp.certificates) certificates.push_back(der);
        return certificates;
    };
    if (signed_data && signer_cert != nullptr && result != kTrustENoSignature) {
        ChainRequest request;
        request.extra_certificates = signature_certificates();
        request.when = verify_time;
        request.requested_usage = {kCodeSigning};
        request.flags = revocation_checks == kRevokeWholeChain ? 0x40000000u : 0u;
        std::lock_guard<std::mutex> lock(m_mutex);
        have_chain = build_chain_locked(request, signer_cert->der, std::nullopt, chain) == 0;
    } else if (signed_data && signer_cert != nullptr) {
        // The signature failed; Windows still reports the signer's chain.
        ChainRequest request;
        request.extra_certificates = signature_certificates();
        request.when = verify_time;
        request.requested_usage = {kCodeSigning};
        std::lock_guard<std::mutex> lock(m_mutex);
        have_chain = build_chain_locked(request, signer_cert->der, std::nullopt, chain) == 0;
    }
    // The counter-signer's own chain: the provider data reports it in the
    // counter-signer's CRYPT_PROVIDER_SGNR as it reports the signer's
    // (csCertChain / pasCertChain / pChainContext); callers walk it with
    // WTHelperGetProvCertFromChain.
    ChainResult ts_chain;
    bool have_ts_chain = false;
    if (timestamp.valid) {
        ChainRequest request;
        request.extra_certificates = timestamp.certificates;
        request.when = timestamp.time;
        request.requested_usage = {kTimeStamping};
        std::lock_guard<std::mutex> lock(m_mutex);
        have_ts_chain = build_chain_locked(request, timestamp.signer_certificate, std::nullopt, ts_chain) == 0;
        if (!have_ts_chain ||
            (ts_chain.error & (kPartialChain | kUntrustedRoot | kNotSignatureValid |
                               kNotTimeValid | kNotValidForUsage | kIsRevoked)) != 0) {
            timestamp.valid = false;  // an unverifiable timestamp does not move the time
            have_ts_chain = false;
        }
    }
    if (timestamp.present && !timestamp.valid && have_chain && verify_time != now) {
        ChainRequest request;
        request.extra_certificates = signature_certificates();
        request.when = now;
        request.requested_usage = {kCodeSigning};
        std::lock_guard<std::mutex> lock(m_mutex);
        have_chain = build_chain_locked(request, signer_cert->der, std::nullopt, chain) == 0;
    }

    // --- policy (Authenticode) -----------------------------------------------
    std::vector<u32> cert_errors(have_chain ? chain.elements.size() : 0, 0);
    if (result == 0 && !image_digest_ok) result = kTrustEBadDigest;
    if (result == 0 && have_chain) {
        auto first = [&](u32 bit, u32 code) {
            if (result != 0) return;
            for (usize i = 0; i < chain.elements.size(); ++i) {
                if ((chain.elements[i].error & bit) != 0) {
                    result = code;
                    cert_errors[i] = code;
                    return;
                }
            }
        };
        first(kNotSignatureValid, kTrustECertSignature);
        if (result == 0 && (chain.error & kPartialChain) != 0) {
            result = kCertEChaining;
            cert_errors.back() = kCertEChaining;
        }
        first(kUntrustedRoot, kCertEUntrustedRoot);
        first(kNotTimeValid, kCertEExpired);
        first(kNotValidForUsage, kCertEWrongUsage);
        first(kIsRevoked, kCertERevoked);
        // Revocation that could not be determined does not fail Authenticode
        // (crypt_wvt_probe "revoke" rows).
    }

    // --- provider data ----------------------------------------------------------
    if (state_action == kStateActionVerify) {
        MaterializedChain made;
        if (have_chain) made = materialize_chain(m, chain, 0);
        MaterializedChain made_ts;
        if (have_ts_chain) made_ts = materialize_chain(m, ts_chain, 0);
        crypt32::GuestBlock block(is64);
        const u32 data_size = is64 ? 0xf0u : 0x88u;
        const u32 sgnr_size = is64 ? 0x40u : 0x2cu;
        const u32 cert_size = is64 ? 0x58u : 0x3cu;
        const u32 pd = block.reserve(data_size);
        auto put_filetime = [&](u32 at, u64 ft) {
            block.put_u32(at, static_cast<u32>(ft));
            block.put_u32(at + 4, static_cast<u32>(ft >> 32));
        };
        block.put_u32(pd + 0, data_size);
        block.put_value(pd + ps, wd);                                   // pWintrustData
        block.put_value(pd + 3 * ps, hwnd);                             // hWndParent
        block.put_value(pd + 4 * ps, action_ptr);                       // pgActionID
        const u32 cs_signers = signed_data ? 1u : 0u;
        const u32 message_handle = signed_data ? m_next_message_handle++ : 0u;
        const u32 store_handle = signed_data ? m_next_store_handle++ : 0u;
        const u32 off_encoding = is64 ? 104u : 56u;
        block.put_u32(pd + off_encoding, signed_data ? 0x00010001u : 0u);
        block.put_value(pd + off_encoding + ps, message_handle);        // hMsg
        const u32 off_signers = is64 ? 120u : 64u;
        block.put_u32(pd + off_signers, cs_signers);
        block.put_u32(pd + (is64 ? 152u : 80u), 1);                     // dwSubjectChoice = SIP
        block.put_ptr(pd + (is64 ? 168u : 88u), block.add_cstring(kCodeSigning));
        put_filetime(pd + (is64 ? 180u : 96u), now);                    // sftSystemTime
        block.put_u32(pd + (is64 ? 200u : 108u), prov_flags | kCpdUseNt5ChainFlag);
        block.put_u32(pd + (is64 ? 204u : 112u), result);               // dwFinalError
        block.put_value(pd + (is64 ? 232u : 132u), signature_settings);
        auto put_certs = [&](u32 sgnr, const MaterializedChain& c, const std::vector<u32>& errors,
                             const ChainResult& r) {
            const u32 count = static_cast<u32>(c.contexts.size());
            block.put_u32(sgnr + 12, count);
            if (count == 0) return;
            const u32 array = block.reserve(count * cert_size);
            block.put_ptr(sgnr + 16, array);
            for (u32 i = 0; i < count; ++i) {
                const u32 at = array + i * cert_size;
                const auto& element = r.elements[i];
                const bool self_signed = (element.info & 0x8) != 0;
                const bool top = i + 1 == count;
                block.put_u32(at + 0, cert_size);
                block.put_value(at + ps, c.contexts[i]);
                block.put_u32(at + 2 * ps + 4, top && r.trusted ? 1u : 0u);  // fTrustedRoot
                block.put_u32(at + 2 * ps + 8, self_signed ? 1u : 0u);        // fSelfSigned
                u32 confidence = kConfidenceTimeNest;
                if ((element.error & kNotSignatureValid) == 0) confidence |= kConfidenceSig;
                if ((element.error & kNotTimeValid) == 0) confidence |= kConfidenceTime;
                block.put_u32(at + 2 * ps + 20, confidence);
                block.put_u32(at + 2 * ps + 24, i < errors.size() ? errors[i] : 0);
                block.put_value(at + (is64 ? 80u : 56u), c.elements[i]);    // pChainElement
            }
        };
        if (signed_data) {
            const u32 sgnr = block.reserve(sgnr_size);
            block.put_ptr(pd + off_signers + ps, sgnr);
            block.put_u32(sgnr + 0, sgnr_size);
            put_filetime(sgnr + 4, verify_time);
            put_certs(sgnr, made, cert_errors, chain);
            const u32 signer_info = crypt32::put_cmsg_signer_info(block, signed_data->signers.front());
            block.put_ptr(sgnr + (is64 ? 32u : 24u), signer_info);
            block.put_u32(sgnr + (is64 ? 40u : 28u), signer_error);
            if (timestamp.present) {
                block.put_u32(sgnr + (is64 ? 44u : 32u), 1);
                const u32 counter = block.reserve(sgnr_size);
                block.put_ptr(sgnr + (is64 ? 48u : 36u), counter);
                block.put_u32(counter + 0, sgnr_size);
                put_filetime(counter + 4, timestamp.valid ? timestamp.time : now);
                block.put_u32(counter + (is64 ? 24u : 20u), kSgnrTypeTimestamp);
                if (timestamp.valid) {
                    block.put_ptr(counter + (is64 ? 32u : 24u),
                                  crypt32::put_cmsg_signer_info(block, timestamp.signer));
                    if (have_ts_chain) {
                        put_certs(counter, made_ts, {}, ts_chain);
                        block.put_value(counter + (is64 ? 56u : 40u), made_ts.chain);
                    }
                } else {
                    block.put_u32(counter + (is64 ? 40u : 28u), kStatusInvalidSignature);
                }
            }
            block.put_value(sgnr + (is64 ? 56u : 40u), made.chain);
        }
        const GuestAddr base = alloc_guest(block.size());
        if (base == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return error_codes::ERROR_NOT_ENOUGH_MEMORY;
        }
        const auto bytes = block.resolve(base);
        (void)m.memory().write_bytes(base, reinterpret_cast<const Byte*>(bytes.data()), bytes.size());
        u32 handle = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            handle = m_next_wvt_state++;
            WvtState state;
            state.block = base;
            state.prov_data = base + pd;
            state.chain = made.chain;
            state.timestamp_chain = made_ts.chain;
            state.message = message_handle;
            state.store = store_handle;
            if (signed_data) {
                fill_message_store_locked(store_handle, signed_data->certificates);
                QueryMessage message;
                message.certificates = signed_data->certificates;
                message.signers = signed_data->signers;
                message.content_type = signed_data->content_type;
                message.content = signed_data->content;
                message.content_value = signed_data->content_value;
                m_query_messages.emplace(message_handle, std::move(message));
            }
            m_wvt_states[handle] = std::move(state);
        }
        (void)m.write_ptr(wd + off_state, handle);
    }
    ipc::ipc_log("WinVerifyTrust action=%u ui=%u revocation=%u prov_flags=0x%x "
                 "settings=%s path='%s' -> 0x%x",
                 state_action, m.read_u32(wd + off_ui_choice).value_or(0), revocation_checks,
                 prov_flags, signature_settings ? "set" : "none", traced_path.c_str(), result);
    set_last_error(result);
    return result;
}

// CRYPT_PROVIDER_DATA* WTHelperProvDataFromStateData(HANDLE hStateData)
GuestAddr Crypt32Manager::wt_helper_prov_data_from_state_data(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_wvt_states.find(handle);
    const GuestAddr result = it == m_wvt_states.end() ? 0 : it->second.prov_data;
    ipc::ipc_log("WTHelperProvDataFromStateData(0x%x) -> 0x%llx", handle,
                 static_cast<unsigned long long>(result));
    return result;
}

// CRYPT_PROVIDER_SGNR* WTHelperGetProvSignerFromChain(CRYPT_PROVIDER_DATA*,
//     DWORD idxSigner, BOOL fCounterSigner, DWORD idxCounterSigner)
// Indexes the provider data's own arrays (NULL when out of range).
GuestAddr Crypt32Manager::wt_helper_get_prov_signer_from_chain(Marshaler& m) {
    const GuestAddr pd = m.arg_ptr(0);
    const u32 index = m.arg32(1);
    const u32 counter = m.arg32(2);
    const u32 counter_index = m.arg32(3);
    const bool is64 = m.cpu_is_64bit();
    const u32 sgnr_size = is64 ? 0x40u : 0x2cu;
    if (pd == 0) return 0;
    const u32 count = m.read_u32(pd + (is64 ? 120u : 64u)).value_or(0);
    const GuestAddr signers = static_cast<GuestAddr>(m.read_ptr(pd + (is64 ? 128u : 68u)).value_or(0));
    ipc::ipc_log("WTHelperGetProvSignerFromChain(pd=0x%llx, %u, counter=%u, %u) signers=%u",
                 static_cast<unsigned long long>(pd), index, counter, counter_index, count);
    if (index >= count || signers == 0) return 0;
    const GuestAddr signer = signers + static_cast<GuestAddr>(index) * sgnr_size;
    if (!counter) return signer;
    const u32 counters = m.read_u32(signer + (is64 ? 44u : 32u)).value_or(0);
    const GuestAddr list = static_cast<GuestAddr>(m.read_ptr(signer + (is64 ? 48u : 36u)).value_or(0));
    if (counter_index >= counters || list == 0) return 0;
    return list + static_cast<GuestAddr>(counter_index) * sgnr_size;
}

// CRYPT_PROVIDER_CERT* WTHelperGetProvCertFromChain(CRYPT_PROVIDER_SGNR*, DWORD idxCert)
GuestAddr Crypt32Manager::wt_helper_get_prov_cert_from_chain(Marshaler& m) {
    const GuestAddr signer = m.arg_ptr(0);
    const u32 index = m.arg32(1);
    const bool is64 = m.cpu_is_64bit();
    if (signer == 0) return 0;
    const u32 count = m.read_u32(signer + 12).value_or(0);
    const GuestAddr certs = static_cast<GuestAddr>(m.read_ptr(signer + 16).value_or(0));
    const GuestAddr result = index >= count || certs == 0
        ? 0 : certs + static_cast<GuestAddr>(index) * (is64 ? 0x58u : 0x3cu);
    ipc::ipc_log("WTHelperGetProvCertFromChain(signer=0x%llx, %u) count=%u -> 0x%llx",
                 static_cast<unsigned long long>(signer), index, count,
                 static_cast<unsigned long long>(result));
    return result;
}

} // namespace vitreo::winapi
