// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_key_exports.cpp
/// @brief advapi32 legacy CryptoAPI keys: CryptGenKey, CryptImportKey and
/// CryptGetUserKey.
///
/// Part of the legacy CryptoAPI shim; the other slices live in the
/// crypto_*_exports.cpp files and share crypto_internal.hpp.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "winapi/advapi32/security/crypto_internal.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <mutex>
#include <stdlib.h> // arc4random_buf
#include <utility>
#include <vector>
#include <Security/Security.h>

namespace vitreo::winapi {

using namespace capi;

/// Registers CryptGenKey, CryptImportKey, CryptGetUserKey.
void add_crypto_key_exports(DllExports& exports) {
    static CryptState* s_state = &capi::crypt_state();

    // CryptGenKey(HCRYPTPROV, ALG_ID, DWORD, HCRYPTKEY*) — 4 args, stdcall=16
    //
    // A missing-export fail-thunk returns 0xFFFFFFFF (TRUE) without writing
    // *phKey. Session keys are random PLAINTEXTKEYBLOB material; RSA
    // AT_KEYEXCHANGE / AT_SIGNATURE pairs are generated with SecKey.
    exports.entries.push_back({"CryptGenKey", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_UID = 0x80090001u;
            constexpr u32 NTE_BAD_ALGID = 0x80090008u;
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_SILENT_CONTEXT = 0x80090022u;
            constexpr u32 NTE_FAIL = 0x80090020u;
            constexpr u32 CRYPT_EXPORTABLE = 0x00000001u;
            constexpr u32 CRYPT_USER_PROTECTED = 0x00000002u;
            constexpr u32 CRYPT_CREATE_SALT = 0x00000004u;
            constexpr u32 CRYPT_NO_SALT = 0x00000010u;
            constexpr u32 CRYPT_ARCHIVABLE = 0x00004000u;
            constexpr u32 CRYPT_FORCE_KEY_PROTECTION_HIGH = 0x00000400u;
            constexpr u32 AT_KEYEXCHANGE = 1;
            constexpr u32 AT_SIGNATURE = 2;
            constexpr u32 CALG_DES = 0x00006601u;
            constexpr u32 CALG_3DES = 0x00006603u;
            constexpr u32 CALG_RC2 = 0x00006602u;
            constexpr u32 CALG_AES_128 = 0x0000660eu;
            constexpr u32 CALG_AES_192 = 0x0000660fu;
            constexpr u32 CALG_AES_256 = 0x00006610u;
            constexpr u32 CALG_RC4 = 0x00006801u;
            constexpr u32 CALG_RSA_SIGN = 0x00002400u;
            constexpr u32 CALG_RSA_KEYX = 0x0000A400u;
            constexpr u8 PRIVATEKEYBLOB = 0x07;
            constexpr u8 PLAINTEXTKEYBLOB = 0x08;

            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 algid = static_cast<u32>(m.arg(1));
            const u32 flags = static_cast<u32>(m.arg(2));
            const GuestAddr ph_key = m.arg_ptr(3);
            const u32 key_bits = flags >> 16;
            const u32 flag_lo = flags & 0xffffu;
            constexpr u32 k_known_lo =
                CRYPT_EXPORTABLE | CRYPT_USER_PROTECTED | CRYPT_CREATE_SALT |
                CRYPT_NO_SALT | CRYPT_ARCHIVABLE |
                CRYPT_FORCE_KEY_PROTECTION_HIGH;

            if (ph_key == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                if (!s_state->providers.contains(provider)) {
                    VITREO_TRACE(
                        "[advapi32] CryptGenKey(hprov=0x%x) "
                        "-> FALSE NTE_BAD_UID\n",
                        provider);
                    set_last_error(NTE_BAD_UID);
                    m.set_return(0);
                    return;
                }
            }

            if ((flag_lo & ~k_known_lo) != 0) {
                set_last_error(NTE_BAD_FLAGS);
                m.set_return(0);
                return;
            }
            if ((flag_lo & (CRYPT_USER_PROTECTED |
                            CRYPT_FORCE_KEY_PROTECTION_HIGH)) != 0) {
                set_last_error(NTE_SILENT_CONTEXT);
                m.set_return(0);
                return;
            }

            auto parse_der_length = [](const u8*& p, const u8* end,
                                       usize& len) -> bool {
                if (p >= end) return false;
                const u8 first = *p++;
                if (first < 0x80) {
                    len = first;
                    return p + len <= end;
                }
                const u32 count = first & 0x7fu;
                if (count == 0 || count > 4 || p + count > end) return false;
                len = 0;
                for (u32 i = 0; i < count; ++i)
                    len = (len << 8) | *p++;
                return p + len <= end;
            };
            auto parse_der_integer = [&](const u8*& p, const u8* end,
                                         std::vector<u8>& out,
                                         u32 pad_to) -> bool {
                if (p >= end || *p++ != 0x02) return false;
                usize len = 0;
                if (!parse_der_length(p, end, len)) return false;
                const u8* body = p;
                p += len;
                while (len > 1 && *body == 0) {
                    ++body;
                    --len;
                }
                if (pad_to != 0) {
                    if (len > pad_to) return false;
                    out.assign(pad_to - static_cast<u32>(len), 0);
                    out.insert(out.end(), body, body + len);
                } else {
                    out.assign(body, body + len);
                }
                return true;
            };
            auto generate_rsa = [&](u32 bits, KeyCtx& key_ctx) -> bool {
                if (bits == 0) bits = 1024;
                if (bits < 1024 || bits > 16384 || (bits & 15u) != 0)
                    return false;
                CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
                    kCFAllocatorDefault, 0,
                    &kCFTypeDictionaryKeyCallBacks,
                    &kCFTypeDictionaryValueCallBacks);
                if (!attrs) return false;
                CFMutableDictionaryRef priv_attrs = CFDictionaryCreateMutable(
                    kCFAllocatorDefault, 0,
                    &kCFTypeDictionaryKeyCallBacks,
                    &kCFTypeDictionaryValueCallBacks);
                if (!priv_attrs) {
                    CFRelease(attrs);
                    return false;
                }
                const int bits_i = static_cast<int>(bits);
                CFNumberRef bits_num = CFNumberCreate(
                    kCFAllocatorDefault, kCFNumberIntType, &bits_i);
                CFDictionarySetValue(attrs, kSecAttrKeyType,
                                     kSecAttrKeyTypeRSA);
                if (bits_num) {
                    CFDictionarySetValue(attrs, kSecAttrKeySizeInBits,
                                         bits_num);
                    CFRelease(bits_num);
                }
                CFDictionarySetValue(priv_attrs, kSecAttrIsPermanent,
                                     kCFBooleanFalse);
                CFDictionarySetValue(attrs, kSecPrivateKeyAttrs, priv_attrs);
                CFRelease(priv_attrs);
                CFErrorRef error = nullptr;
                SecKeyRef private_key =
                    SecKeyCreateRandomKey(attrs, &error);
                CFRelease(attrs);
                if (!private_key) {
                    if (error) CFRelease(error);
                    return false;
                }
                CFDataRef exported =
                    SecKeyCopyExternalRepresentation(private_key, &error);
                CFRelease(private_key);
                if (!exported) {
                    if (error) CFRelease(error);
                    return false;
                }
                const u8* p = CFDataGetBytePtr(exported);
                const u8* end = p + CFDataGetLength(exported);
                bool ok = false;
                usize seq_len = 0;
                if (p < end && *p++ == 0x30 &&
                    parse_der_length(p, end, seq_len)) {
                    const u8* seq_end = p + seq_len;
                    std::vector<u8> version;
                    ok = parse_der_integer(p, seq_end, version, 0) &&
                         parse_der_integer(p, seq_end,
                                           key_ctx.modulus_be, 0);
                    if (ok) {
                        u32 mod_len = static_cast<u32>(
                            key_ctx.modulus_be.size());
                        if ((mod_len & 1u) != 0) ++mod_len;
                        if (mod_len != key_ctx.modulus_be.size()) {
                            std::vector<u8> padded(
                                mod_len - key_ctx.modulus_be.size(), 0);
                            padded.insert(padded.end(),
                                key_ctx.modulus_be.begin(),
                                key_ctx.modulus_be.end());
                            key_ctx.modulus_be = std::move(padded);
                        }
                        const u32 half = static_cast<u32>(
                            key_ctx.modulus_be.size() / 2);
                        std::vector<u8> pubexp;
                        ok = parse_der_integer(p, seq_end, pubexp, 0) &&
                             parse_der_integer(
                                 p, seq_end, key_ctx.private_exponent_be,
                                 static_cast<u32>(key_ctx.modulus_be.size())) &&
                             parse_der_integer(p, seq_end, key_ctx.prime1_be,
                                               half) &&
                             parse_der_integer(p, seq_end, key_ctx.prime2_be,
                                               half) &&
                             parse_der_integer(p, seq_end,
                                               key_ctx.exponent1_be, half) &&
                             parse_der_integer(p, seq_end,
                                               key_ctx.exponent2_be, half) &&
                             parse_der_integer(p, seq_end,
                                               key_ctx.coefficient_be, half);
                        key_ctx.pubexp = 0;
                        for (u8 b : pubexp)
                            key_ctx.pubexp = (key_ctx.pubexp << 8) | b;
                        if (key_ctx.pubexp == 0) key_ctx.pubexp = 65537;
                    }
                }
                CFRelease(exported);
                if (error) CFRelease(error);
                return ok && !key_ctx.modulus_be.empty() &&
                       !key_ctx.private_exponent_be.empty();
            };

            KeyCtx key_ctx;
            key_ctx.provider = provider;
            key_ctx.exportable = (flag_lo & CRYPT_EXPORTABLE) != 0;

            auto session_bytes = [&](u32 alg) -> u32 {
                u32 bits = key_bits;
                switch (alg) {
                case CALG_RC4:
                case CALG_RC2:
                    if (bits == 0) bits = 128;
                    if (bits < 40 || bits > 128 || (bits % 8u) != 0)
                        return 0;
                    return bits / 8u;
                case CALG_AES_128:
                    if (bits != 0 && bits != 128) return 0;
                    return 16;
                case CALG_AES_192:
                    if (bits != 0 && bits != 192) return 0;
                    return 24;
                case CALG_AES_256:
                    if (bits != 0 && bits != 256) return 0;
                    return 32;
                case CALG_3DES:
                    if (bits != 0 && bits != 192) return 0;
                    return 24;
                case CALG_DES:
                    if (bits != 0 && bits != 64) return 0;
                    return 8;
                default:
                    return 0;
                }
            };

            const bool rsa = algid == AT_KEYEXCHANGE ||
                             algid == AT_SIGNATURE ||
                             algid == CALG_RSA_KEYX ||
                             algid == CALG_RSA_SIGN;
            if (rsa) {
                key_ctx.alg_id = (algid == AT_SIGNATURE ||
                                  algid == CALG_RSA_SIGN)
                    ? CALG_RSA_SIGN : CALG_RSA_KEYX;
                key_ctx.key_spec = (key_ctx.alg_id == CALG_RSA_SIGN)
                    ? AT_SIGNATURE : AT_KEYEXCHANGE;
                key_ctx.blob_type = PRIVATEKEYBLOB;
                if (!generate_rsa(key_bits, key_ctx)) {
                    VITREO_TRACE(
                        "[advapi32] CryptGenKey RSA bits=%u "
                        "-> FALSE\n", key_bits);
                    set_last_error(key_bits != 0 &&
                                   (key_bits < 1024 || (key_bits & 15u) != 0)
                        ? error_codes::ERROR_INVALID_PARAMETER
                        : NTE_FAIL);
                    m.set_return(0);
                    return;
                }
            } else {
                const u32 nbytes = session_bytes(algid);
                if (nbytes == 0) {
                    VITREO_TRACE(
                        "[advapi32] CryptGenKey alg=0x%x bits=%u "
                        "-> FALSE NTE_BAD_ALGID\n",
                        algid, key_bits);
                    set_last_error(key_bits != 0
                        ? error_codes::ERROR_INVALID_PARAMETER
                        : NTE_BAD_ALGID);
                    m.set_return(0);
                    return;
                }
                key_ctx.alg_id = algid;
                key_ctx.blob_type = PLAINTEXTKEYBLOB;
                key_ctx.raw_key.resize(nbytes);
                arc4random_buf(key_ctx.raw_key.data(), nbytes);
            }

            u32 key_handle = 0;
            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                key_handle = s_state->next_key++;
                if (rsa) {
                    s_state->providers.at(provider).user_keys[
                        key_ctx.key_spec] = key_ctx;
                }
                s_state->keys.emplace(key_handle, std::move(key_ctx));
            }
            if (m.write_ptr(ph_key, key_handle).is_err()) {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                s_state->keys.erase(key_handle);
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            VITREO_TRACE(
                "[advapi32] CryptGenKey(hprov=0x%x, alg=0x%x, "
                "flags=0x%x) -> 0x%x\n",
                provider, algid, flags, key_handle);
            m.set_return(1);
        }, 16});

    // CryptImportKey(HCRYPTPROV, BYTE*, DWORD, HCRYPTKEY, DWORD, HCRYPTKEY*)
    // 6 args, stdcall=24
    //
    // Must be a real stdcall export: the missing-export stub uses
    // stdcall_bytes=0, which leaves 24 bytes on the x86 stack and corrupts
    // subsequent CryptoAPI cleanup (observed as CryptReleaseContext getting
    // a stack pointer instead of HCRYPTPROV, then AV).
    //
    // Supported base-provider blob types: public RSA keys, private RSA
    // key pairs, and plaintext symmetric keys.
    exports.entries.push_back({"CryptImportKey", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_UID = 0x80090001u;
            constexpr u32 NTE_BAD_TYPE = 0x8009000Au;
            constexpr u32 NTE_BAD_DATA = 0x80090005u;
            constexpr u32 NTE_BAD_VER = 0x80090007u;
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u8 PUBLICKEYBLOB = 0x06;
            constexpr u8 PRIVATEKEYBLOB = 0x07;
            constexpr u8 PLAINTEXTKEYBLOB = 0x08;
            constexpr u8 CUR_BLOB_VERSION = 0x02;
            constexpr u32 CRYPT_EXPORTABLE = 0x00000001u;
            constexpr u32 CALG_RSA_SIGN = 0x00002400u;
            constexpr u32 CALG_RSA_KEYX = 0x0000A400u;
            constexpr u32 AT_KEYEXCHANGE = 1;
            constexpr u32 AT_SIGNATURE = 2;

            const u32 provider = static_cast<u32>(m.arg(0));
            const GuestAddr blob_ptr = m.arg_ptr(1);
            const u32 blob_len = static_cast<u32>(m.arg(2));
            (void)m.arg(3);  // hPubKey — unused for PUBLICKEYBLOB/PLAINTEXT
            const u32 flags = static_cast<u32>(m.arg(4));
            const GuestAddr ph_key = m.arg_ptr(5);

            if (blob_ptr == 0 || ph_key == 0 || blob_len < 8) {
                VITREO_TRACE(
                    "[advapi32] CryptImportKey(hprov=0x%x, len=%u) "
                    "-> FALSE ERROR_INVALID_PARAMETER\n",
                    provider, blob_len);
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                if (!s_state->providers.contains(provider)) {
                    VITREO_TRACE(
                        "[advapi32] CryptImportKey(hprov=0x%x) "
                        "-> FALSE NTE_BAD_UID\n",
                        provider);
                    set_last_error(NTE_BAD_UID);
                    m.set_return(0);
                    return;
                }
            }

            std::vector<u8> blob(blob_len);
            if (m.memory().read_bytes(blob_ptr, blob.data(), blob_len).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            const u8 b_type = blob[0];
            const u8 b_version = blob[1];
            const u32 ai_key_alg =
                static_cast<u32>(blob[4]) |
                (static_cast<u32>(blob[5]) << 8) |
                (static_cast<u32>(blob[6]) << 16) |
                (static_cast<u32>(blob[7]) << 24);
            if (b_version != CUR_BLOB_VERSION) {
                VITREO_TRACE(
                    "[advapi32] CryptImportKey: bad blob version %u "
                    "-> FALSE NTE_BAD_VER\n",
                    static_cast<unsigned>(b_version));
                set_last_error(NTE_BAD_VER);
                m.set_return(0);
                return;
            }
            if (b_type != PUBLICKEYBLOB && b_type != PRIVATEKEYBLOB &&
                b_type != PLAINTEXTKEYBLOB) {
                VITREO_TRACE(
                    "[advapi32] CryptImportKey: blob type 0x%x "
                    "unsupported -> FALSE NTE_BAD_TYPE\n",
                    static_cast<unsigned>(b_type));
                set_last_error(NTE_BAD_TYPE);
                m.set_return(0);
                return;
            }

            KeyCtx key_ctx;
            key_ctx.provider = provider;
            key_ctx.alg_id = ai_key_alg;
            key_ctx.blob_type = b_type;
            key_ctx.exportable = (flags & CRYPT_EXPORTABLE) != 0;

            if (b_type == PUBLICKEYBLOB || b_type == PRIVATEKEYBLOB) {
                // PUBLICKEYSTRUC (8) + RSAPUBKEY (12) + modulus[bitlen/8]
                // RSAPUBKEY: magic 'RSA1', bitlen, pubexp — all little-endian.
                if (blob_len < 20) {
                    set_last_error(NTE_BAD_DATA);
                    m.set_return(0);
                    return;
                }
                const u32 magic =
                    static_cast<u32>(blob[8]) |
                    (static_cast<u32>(blob[9]) << 8) |
                    (static_cast<u32>(blob[10]) << 16) |
                    (static_cast<u32>(blob[11]) << 24);
                const u32 bitlen =
                    static_cast<u32>(blob[12]) |
                    (static_cast<u32>(blob[13]) << 8) |
                    (static_cast<u32>(blob[14]) << 16) |
                    (static_cast<u32>(blob[15]) << 24);
                key_ctx.pubexp =
                    static_cast<u32>(blob[16]) |
                    (static_cast<u32>(blob[17]) << 8) |
                    (static_cast<u32>(blob[18]) << 16) |
                    (static_cast<u32>(blob[19]) << 24);
                const u32 mod_bytes = bitlen / 8;
                const u32 expected_magic = b_type == PUBLICKEYBLOB
                    ? 0x31415352u /* 'RSA1' */
                    : 0x32415352u /* 'RSA2' */;
                if (magic != expected_magic || (bitlen & 15u) != 0 ||
                    mod_bytes == 0 || blob_len < 20 + mod_bytes) {
                    set_last_error(NTE_BAD_DATA);
                    m.set_return(0);
                    return;
                }
                key_ctx.modulus_be.assign(
                    blob.begin() + 20, blob.begin() + 20 + static_cast<std::ptrdiff_t>(mod_bytes));
                // CryptoAPI stores the modulus little-endian; SecKey wants BE.
                std::reverse(key_ctx.modulus_be.begin(), key_ctx.modulus_be.end());

                if (b_type == PRIVATEKEYBLOB) {
                    if ((flags & ~CRYPT_EXPORTABLE) != 0 ||
                        (ai_key_alg != CALG_RSA_SIGN &&
                         ai_key_alg != CALG_RSA_KEYX)) {
                        set_last_error((flags & ~CRYPT_EXPORTABLE) != 0
                            ? NTE_BAD_FLAGS : NTE_BAD_DATA);
                        m.set_return(0);
                        return;
                    }
                    const u32 half = mod_bytes / 2;
                    const u64 required = 20ull + mod_bytes +
                        5ull * half + mod_bytes;
                    if (blob_len < required) {
                        set_last_error(NTE_BAD_DATA);
                        m.set_return(0);
                        return;
                    }
                    usize offset = 20 + mod_bytes;
                    auto copy_le_integer = [&](u32 size,
                                               std::vector<u8>& out) {
                        out.assign(blob.begin() +
                                       static_cast<std::ptrdiff_t>(offset),
                                   blob.begin() +
                                       static_cast<std::ptrdiff_t>(offset + size));
                        std::reverse(out.begin(), out.end());
                        offset += size;
                    };
                    copy_le_integer(half, key_ctx.prime1_be);
                    copy_le_integer(half, key_ctx.prime2_be);
                    copy_le_integer(half, key_ctx.exponent1_be);
                    copy_le_integer(half, key_ctx.exponent2_be);
                    copy_le_integer(half, key_ctx.coefficient_be);
                    copy_le_integer(mod_bytes,
                                    key_ctx.private_exponent_be);
                    key_ctx.key_spec = ai_key_alg == CALG_RSA_SIGN
                        ? AT_SIGNATURE : AT_KEYEXCHANGE;
                }
            } else {
                if (flags != 0) {
                    set_last_error(NTE_BAD_FLAGS);
                    m.set_return(0);
                    return;
                }
                if (blob_len < 12) {
                    set_last_error(NTE_BAD_DATA);
                    m.set_return(0);
                    return;
                }
                const u32 key_len =
                    static_cast<u32>(blob[8]) |
                    (static_cast<u32>(blob[9]) << 8) |
                    (static_cast<u32>(blob[10]) << 16) |
                    (static_cast<u32>(blob[11]) << 24);
                if (blob_len < 12 + key_len) {
                    set_last_error(NTE_BAD_DATA);
                    m.set_return(0);
                    return;
                }
                key_ctx.raw_key.assign(
                    blob.begin() + 12,
                    blob.begin() + 12 + static_cast<std::ptrdiff_t>(key_len));
            }

            u32 key_handle = 0;
            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                key_handle = s_state->next_key++;
                s_state->keys.emplace(key_handle, key_ctx);
                if (b_type == PRIVATEKEYBLOB) {
                    s_state->providers.at(provider).user_keys[
                        key_ctx.key_spec] = std::move(key_ctx);
                }
            }
            if (m.write_u32(ph_key, key_handle).is_err()) {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                s_state->keys.erase(key_handle);
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            VITREO_TRACE(
                "[advapi32] CryptImportKey(hprov=0x%x, type=0x%x, "
                "len=%u) -> key=0x%x\n",
                provider, static_cast<unsigned>(b_type), blob_len,
                key_handle);
            m.set_return(1);
        }, 24});

    // CryptGetUserKey(HCRYPTPROV, DWORD, HCRYPTKEY*)
    // Returns a disposable handle for the requested key pair from the
    // provider's current container.
    exports.entries.push_back({"CryptGetUserKey", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_UID = 0x80090001u;
            constexpr u32 NTE_BAD_KEY = 0x80090003u;
            constexpr u32 NTE_NO_KEY = 0x8009000Du;
            constexpr u32 AT_KEYEXCHANGE = 1;
            constexpr u32 AT_SIGNATURE = 2;
            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 key_spec = static_cast<u32>(m.arg(1));
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (key_spec != AT_KEYEXCHANGE && key_spec != AT_SIGNATURE) {
                set_last_error(NTE_BAD_KEY);
                m.set_return(0);
                return;
            }

            u32 handle = 0;
            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                const auto provider_it = s_state->providers.find(provider);
                if (provider_it == s_state->providers.end()) {
                    set_last_error(NTE_BAD_UID);
                    m.set_return(0);
                    return;
                }
                const auto key_it = provider_it->second.user_keys.find(key_spec);
                if (key_it == provider_it->second.user_keys.end()) {
                    set_last_error(NTE_NO_KEY);
                    m.set_return(0);
                    return;
                }
                handle = s_state->next_key++;
                s_state->keys.emplace(handle, key_it->second);
            }
            if (m.write_ptr(out, handle).is_err()) {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                s_state->keys.erase(handle);
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 12});
}

} // namespace vitreo::winapi
