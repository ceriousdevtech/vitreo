// SPDX-License-Identifier: LGPL-2.1-only
/// @file crypt32.cpp
/// @brief CRYPT32.dll — "always-trust" certificate verification
///
/// Implements the Crypt32 surface a statically linked OpenSSL verify
/// callback needs.  Guest-side
/// structures (CERT_CONTEXT, CERT_CHAIN_CONTEXT, etc.) are allocated from
/// a dedicated scratch region at CRYPT32_SCRATCH_BASE.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "guest_block.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#endif

/// @file cryptstringtobinarya_cryptbinarytostringa_base64.cpp
/// @brief CryptStringToBinaryA / CryptBinaryToStringA — Base64/HEX encode/decode
///
/// Closed cut from crypt32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// CryptStringToBinaryA / CryptBinaryToStringA — Base64/HEX encode/decode
// =============================================================================
//
// Constants (from wincrypt.h):
//   CRYPT_STRING_BASE64HEADER  = 0x00000000
//   CRYPT_STRING_BASE64        = 0x00000001
//   CRYPT_STRING_BINARY        = 0x00000002
//   CRYPT_STRING_BASE64REQUESTHEADER = 0x00000003
//   CRYPT_STRING_HEX           = 0x00000004
//   CRYPT_STRING_HEXASCII      = 0x00000005
//   CRYPT_STRING_BASE64_ANY    = 0x00000006
//   CRYPT_STRING_ANY           = 0x00000007
//   CRYPT_STRING_HEX_ANY       = 0x00000008
//   CRYPT_STRING_HEXADDR       = 0x0000000a
//   CRYPT_STRING_HEXASCIIADDR  = 0x0000000b
//   CRYPT_STRING_HEXRAW        = 0x0000000c
//   CRYPT_STRING_NOCRLF        = 0x40000000
//   CRYPT_STRING_NOCR          = 0x80000000

namespace {

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::vector<u8> decode_base64(const std::string& src) {
    std::vector<u8> out;
    out.reserve(src.size() * 3 / 4);
    int buf = 0, bits = 0;
    for (char c : src) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_value(c);
        if (v < 0) continue; // tolerate junk
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

static std::string strip_pem_armor(const std::string& s) {
    // Skip lines starting with "-----"
    std::string r;
    r.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t eol = s.find('\n', i);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(i, eol - i);
        // trim trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 5 || line.compare(0, 5, "-----") != 0) {
            r += line;
        }
        i = eol + 1;
    }
    return r;
}

static std::vector<u8> decode_hex(const std::string& src) {
    std::vector<u8> out;
    int hi = -1;
    for (char c : src) {
        int v = hex_value(c);
        if (v < 0) continue; // skip whitespace, separators
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<u8>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

static std::string encode_base64(const u8* data, usize n, bool crlf) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4 + (crlf ? n / 48 * 2 : 0));
    usize i = 0;
    usize line_chars = 0;
    while (i + 2 < n) {
        u32 v = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | u32(data[i + 2]);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
        line_chars += 4;
        if (crlf && line_chars >= 64) {
            out += "\r\n";
            line_chars = 0;
        }
    }
    if (i < n) {
        u32 v = u32(data[i]) << 16;
        if (i + 1 < n) v |= u32(data[i + 1]) << 8;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        if (i + 1 < n) {
            out.push_back(tbl[(v >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back('=');
            out.push_back('=');
        }
    }
    if (crlf) out += "\r\n";
    return out;
}

static std::string encode_hexraw(const u8* data, usize n) {
    static const char* tbl = "0123456789abcdef";
    std::string out;
    out.resize(n * 2);
    for (usize i = 0; i < n; ++i) {
        out[i * 2] = tbl[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = tbl[data[i] & 0xF];
    }
    return out;
}

static u32 crypt_string_to_binary_from_narrow(
    Marshaler& m,
    const std::string& src,
    u32 flags,
    GuestAddr out_buf,
    GuestAddr cb_ptr,
    GuestAddr skip_ptr,
    GuestAddr out_flags_ptr,
    bool wide) {
    u32 fmt = flags & 0xF;
    std::vector<u8> bin;
    switch (fmt) {
        case 0x0: // BASE64HEADER — strip "-----..." armor lines
        case 0x3: // BASE64REQUESTHEADER
        case 0x6: // BASE64_ANY
        case 0x7: // ANY
            bin = decode_base64(strip_pem_armor(src));
            break;
        case 0x1: // BASE64
            bin = decode_base64(src);
            break;
        case 0x2: // BINARY
            bin.assign(src.begin(), src.end());
            break;
        case 0x4: // HEX
        case 0x5: // HEXASCII
        case 0x8: // HEX_ANY
        case 0xa: // HEXADDR
        case 0xb: // HEXASCIIADDR
        case 0xc: // HEXRAW
            bin = decode_hex(src);
            break;
        default:
            return 0;
    }

    u32 cb = m.read_u32(cb_ptr).value_or(0);
    (void)m.write_u32(cb_ptr, static_cast<u32>(bin.size()));
    if (skip_ptr != 0) (void)m.write_u32(skip_ptr, 0);
    if (out_flags_ptr != 0) (void)m.write_u32(out_flags_ptr, fmt);

    if (out_buf == 0) {
        return 1;
    }
    if (cb < bin.size()) {
        return 0;
    }
    if (!bin.empty()) {
        (void)m.memory().write_bytes(out_buf,
            reinterpret_cast<const Byte*>(bin.data()), bin.size());
    }
    VITREO_TRACE("[crypt32] CryptStringToBinary%s fmt=0x%x in=%zu out=%zu\n",
                 wide ? "W" : "A", fmt, src.size(), bin.size());
    return 1;
}

} // namespace

u32 Crypt32Manager::crypt_string_to_binary_a(Marshaler& m) {
    // BOOL CryptStringToBinaryA(LPCSTR pszString, DWORD cchString, DWORD dwFlags,
    //   BYTE *pbBinary, DWORD *pcbBinary, DWORD *pdwSkip, DWORD *pdwFlags)
    GuestAddr str_addr = m.arg_ptr(0);
    u32 cch = static_cast<u32>(m.arg(1));
    u32 flags = static_cast<u32>(m.arg(2));
    GuestAddr out_buf = m.arg_ptr(3);
    GuestAddr cb_ptr = m.arg_ptr(4);
    GuestAddr skip_ptr = m.arg_ptr(5);
    GuestAddr out_flags_ptr = m.arg_ptr(6);

    if (str_addr == 0 || cb_ptr == 0) return 0;

    // Read source
    std::string src;
    if (cch == 0) {
        // NUL-terminated
        for (u32 i = 0; i < (1u << 24); ++i) {
            u8 c = m.read_u8(str_addr + i).value_or(0);
            if (c == 0) break;
            src.push_back(static_cast<char>(c));
        }
    } else {
        src.resize(cch);
        for (u32 i = 0; i < cch; ++i) {
            src[i] = static_cast<char>(m.read_u8(str_addr + i).value_or(0));
        }
    }

    return crypt_string_to_binary_from_narrow(
        m, src, flags, out_buf, cb_ptr, skip_ptr, out_flags_ptr, /*wide=*/false);
}

u32 Crypt32Manager::crypt_string_to_binary_w(Marshaler& m) {
    // MS Learn: BOOL CryptStringToBinaryW(LPCWSTR, DWORD, DWORD, BYTE*, DWORD*,
    //   DWORD*, DWORD*). Same formats as A; input is UTF-16. Base64/hex/PEM are
    //   ASCII, so each wchar maps to a single narrow byte.
    GuestAddr str_addr = m.arg_ptr(0);
    u32 cch = static_cast<u32>(m.arg(1));
    u32 flags = static_cast<u32>(m.arg(2));
    GuestAddr out_buf = m.arg_ptr(3);
    GuestAddr cb_ptr = m.arg_ptr(4);
    GuestAddr skip_ptr = m.arg_ptr(5);
    GuestAddr out_flags_ptr = m.arg_ptr(6);

    if (str_addr == 0 || cb_ptr == 0) return 0;

    std::string src;
    if (cch == 0) {
        for (u32 i = 0; i < (1u << 24); ++i) {
            const u16 w = m.read_u16(str_addr + static_cast<GuestAddr>(i) * 2)
                              .value_or(0);
            if (w == 0) break;
            src.push_back(static_cast<char>(w & 0xff));
        }
    } else {
        src.resize(cch);
        for (u32 i = 0; i < cch; ++i) {
            const u16 w = m.read_u16(str_addr + static_cast<GuestAddr>(i) * 2)
                              .value_or(0);
            src[i] = static_cast<char>(w & 0xff);
        }
    }

    return crypt_string_to_binary_from_narrow(
        m, src, flags, out_buf, cb_ptr, skip_ptr, out_flags_ptr, /*wide=*/true);
}

u32 Crypt32Manager::crypt_binary_to_string_a(Marshaler& m) {
    // BOOL CryptBinaryToStringA(const BYTE *pbBinary, DWORD cbBinary, DWORD dwFlags,
    //   LPSTR pszString, DWORD *pcchString)
    GuestAddr bin_addr = m.arg_ptr(0);
    u32 cb = static_cast<u32>(m.arg(1));
    u32 flags = static_cast<u32>(m.arg(2));
    GuestAddr out_str = m.arg_ptr(3);
    GuestAddr cch_ptr = m.arg_ptr(4);

    if (cch_ptr == 0) return 0;

    std::vector<u8> bin(cb);
    if (cb > 0 && bin_addr != 0) {
        const Byte* host = m.memory().host_ptr(bin_addr, cb);
        if (host) {
            std::memcpy(bin.data(), host, cb);
        } else {
            (void)m.memory().read_bytes(bin_addr, bin.data(), cb);
        }
    }

    u32 fmt = flags & 0xF;
    bool no_crlf = (flags & 0x40000000) != 0;

    std::string out;
    switch (fmt) {
        case 0x0: // BASE64HEADER
            out  = "-----BEGIN CERTIFICATE-----\r\n";
            out += encode_base64(bin.data(), bin.size(), !no_crlf);
            out += "-----END CERTIFICATE-----\r\n";
            break;
        case 0x1: // BASE64
        case 0x6: // BASE64_ANY
            out = encode_base64(bin.data(), bin.size(), !no_crlf);
            break;
        case 0x2: // BINARY
            out.assign(bin.begin(), bin.end());
            break;
        case 0x4: // HEX
        case 0x5: // HEXASCII
        case 0xc: // HEXRAW
            out = encode_hexraw(bin.data(), bin.size());
            break;
        default:
            return 0;
    }

    u32 cch = m.read_u32(cch_ptr).value_or(0);
    u32 needed = static_cast<u32>(out.size() + 1); // +NUL
    if (out_str == 0) {
        (void)m.write_u32(cch_ptr, needed);
        return 1;
    }
    if (cch < needed) {
        (void)m.write_u32(cch_ptr, needed);
        return 0;
    }
    for (usize i = 0; i < out.size(); ++i) {
        (void)m.write_u8(out_str + static_cast<GuestAddr>(i), static_cast<u8>(out[i]));
    }
    (void)m.write_u8(out_str + static_cast<GuestAddr>(out.size()), 0);
    (void)m.write_u32(cch_ptr, static_cast<u32>(out.size())); // excludes NUL on success
    VITREO_TRACE("[crypt32] CryptBinaryToStringA fmt=0x%x in=%u out=%zu\n",
                 fmt, cb, out.size());
    return 1;
}

// CryptEncodeObjectEx: no encoders are implemented yet.
u32 Crypt32Manager::crypt_encode_object_ex() {
    crypt32::report_unimplemented("crypt32!CryptEncodeObjectEx", "ASN.1 encoders");
    set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

// PFXImportCertStore: PKCS#12 import needs private-key storage.
u32 Crypt32Manager::pfx_import_cert_store() {
    crypt32::report_unimplemented("crypt32!PFXImportCertStore", "PKCS#12 import");
    set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

// BOOL CryptHashCertificate2(LPCWSTR pwszCNGHashAlgid, DWORD dwFlags,
//     void* pvReserved, const BYTE* pbEncoded, DWORD cbEncoded,
//     BYTE* pbComputedHash, DWORD* pcbComputedHash)
// Measured (crypt_misc_probe "hash" rows): CNG algorithm names are
// case-insensitive; an unknown one is STATUS_NOT_FOUND with *pcb = 0; a
// short buffer is ERROR_MORE_DATA; dwFlags is ignored; only the first
// successful call in a process sets the last error (to 0).
u32 Crypt32Manager::crypt_hash_certificate2(Marshaler& m) {
    constexpr u32 kStatusNotFound = 0xC0000225u;
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr data = m.arg_ptr(3);
    const u32 size = m.arg32(4);
    const GuestAddr out = m.arg_ptr(5);
    const GuestAddr out_size = m.arg_ptr(6);
    auto name = m.read_wide_string(name_ptr, 64);
    std::string algorithm = name.is_ok() ? name.value() : std::string{};
    for (char& c : algorithm) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::vector<u8> bytes(size);
    if (size != 0 && m.memory().read_bytes(data, reinterpret_cast<Byte*>(bytes.data()), size).is_err()) {
        crypt32::raise_access_violation(m, data, false);
        return 0;
    }
    std::vector<u8> digest;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto run = [&](usize length, unsigned char* (*fn)(const void*, CC_LONG, unsigned char*)) {
        digest.resize(length);
        fn(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    };
    if (algorithm == "SHA1") run(CC_SHA1_DIGEST_LENGTH, CC_SHA1);
    else if (algorithm == "SHA256") run(CC_SHA256_DIGEST_LENGTH, CC_SHA256);
    else if (algorithm == "SHA384") run(CC_SHA384_DIGEST_LENGTH, CC_SHA384);
    else if (algorithm == "SHA512") run(CC_SHA512_DIGEST_LENGTH, CC_SHA512);
    else if (algorithm == "MD5") run(CC_MD5_DIGEST_LENGTH, CC_MD5);
    else if (algorithm == "MD4") run(CC_MD4_DIGEST_LENGTH, CC_MD4);
    else if (algorithm == "MD2") run(CC_MD2_DIGEST_LENGTH, CC_MD2);
#pragma clang diagnostic pop
    if (digest.empty()) {
        (void)m.write_u32(out_size, 0);
        set_last_error(kStatusNotFound);
        return 0;
    }
    const u32 capacity = m.read_u32(out_size).value_or(0);
    (void)m.write_u32(out_size, static_cast<u32>(digest.size()));
    if (out != 0) {
        if (capacity < digest.size()) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            return 0;
        }
        (void)m.memory().write_bytes(out, reinterpret_cast<const Byte*>(digest.data()), digest.size());
    }
    // Only the first successful call in the process clears the last error
    // (crypt_misc_probe: "hash SHA1" 0, every later row 0xdead).
    std::call_once(m_hash_certificate2_once, [] { set_last_error(0); });
    return 1;
}

// BOOL CertControlStore(HCERTSTORE, DWORD dwFlags, DWORD dwCtrlType, void const*)
// A memory store supports no control (crypt_misc_probe "control" rows:
// FALSE, ERROR_CALL_NOT_IMPLEMENTED). A capsule system store is read from
// and written to its backing files on every call, so RESYNC, AUTO_RESYNC
// and COMMIT have nothing left to do; change notification is not modelled.
u32 Crypt32Manager::cert_control_store(Marshaler& m) {
    constexpr u32 kResync = 1, kNotifyChange = 2, kCommit = 3, kAutoResync = 4;
    const u32 store = static_cast<u32>(m.arg(0));
    const u32 control = m.arg32(2);
    bool system = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        system = m_system_store_names.contains(store);
    }
    if (system && (control == kResync || control == kCommit || control == kAutoResync)) return 1;
    if (system && control == kNotifyChange) {
        crypt32::report_unimplemented("crypt32!CertControlStore", "CERT_STORE_CTRL_NOTIFY_CHANGE");
    }
    set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

// BOOL CryptAcquireCertificatePrivateKey(PCCERT_CONTEXT, DWORD, void*,
//     HCRYPTPROV_OR_NCRYPT_KEY_HANDLE*, DWORD* pdwKeySpec, BOOL* pfCallerFree)
u32 Crypt32Manager::crypt_acquire_certificate_private_key(Marshaler& m) {
    constexpr u32 kCryptENoKeyProperty = 0x8009200Bu;
    const GuestAddr key = m.arg_ptr(3);
    const GuestAddr spec = m.arg_ptr(4);
    const GuestAddr caller_free = m.arg_ptr(5);
    if (key != 0) (void)m.write_ptr(key, 0);   // HCRYPTPROV_OR_NCRYPT_KEY_HANDLE is pointer-sized
    if (spec != 0) (void)m.write_u32(spec, 0);
    if (caller_free != 0) (void)m.write_u32(caller_free, 0);
    set_last_error(kCryptENoKeyProperty);
    return 0;
}

// Microsoft Learn (wincrypt!CertFindExtension): scan CERT_EXTENSION array by OID.
GuestAddr Crypt32Manager::cert_find_extension(Marshaler& m) {
    const GuestAddr oid_ptr = m.arg_ptr(0);
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr extensions = m.arg_ptr(2);
    if (oid_ptr == 0 || extensions == 0 || count == 0) {
        return 0;
    }
    auto oid_r = m.read_ansi_string(oid_ptr);
    if (oid_r.is_err()) return 0;
    const std::string& want = oid_r.value();
    const bool is64 = m.cpu_is_64bit();
    const u32 ext_size = is64 ? 32u : 16u;
    const u32 oid_off = 0;
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr ext = extensions + i * ext_size;
        GuestAddr psz = 0;
        if (is64) {
            psz = m.read_u64(ext + oid_off).value_or(0);
        } else {
            psz = m.read_u32(ext + oid_off).value_or(0);
        }
        if (psz == 0) continue;
        auto got = m.read_ansi_string(psz);
        if (got.is_ok() && got.value() == want) {
            return ext;
        }
    }
    return 0;
}

// BOOL CryptDecodeObjectEx(DWORD dwCertEncodingType, LPCSTR lpszStructType,
//     const BYTE* pbEncoded, DWORD cbEncoded, DWORD dwFlags,
//     PCRYPT_DECODE_PARA pDecodePara, void* pvStructInfo, DWORD* pcbStructInfo)
// Microsoft Learn (wincrypt!CryptDecodeObjectEx). Implemented structure
// types: X509_ALTERNATE_NAME (12) / szOID_SUBJECT_ALT_NAME(2) /
// szOID_ISSUER_ALT_NAME(2), and X509_ENHANCED_KEY_USAGE (36) /
// szOID_ENHANCED_KEY_USAGE. Layout measured by crypt_misc_probe on Windows:
// the struct area and every out-of-line item are rounded to 8 bytes (x64
// CERT_ALT_NAME_INFO for 5 entries = 0xe8, x86 = 0xa8); every success,
// including the size query, sets last error 0; a short buffer reports
// ERROR_MORE_DATA with the needed size; a non-SEQUENCE is
// CRYPT_E_ASN1_BADTAG.
namespace {

/// PKCS_UTC_TIME / szOID_RSA_signingTime: a DER UTCTime -> FILETIME (UTC).
/// Measured (crypt_signer_trio_probe "time" rows): "YYMMDDhhmm[ss]" then 'Z'
/// or a "+hhmm"/"-hhmm" offset that is added to (or subtracted from) the
/// time as written, its hours read in decimal and its minutes -- as Windows
/// reads them -- in base 16 ("+0030" adds 0x30 = 48 minutes); YY below 50 is
/// 20YY. Content that ends before 'Z' or an offset (or a TLV longer than the
/// data) is CRYPT_E_ASN1_EOD; a date or time out of range is
/// CRYPT_E_BAD_ENCODE; bytes after the TLV are ignored. A non-digit where a
/// digit belongs was not measured and is reported as CRYPT_E_ASN1_CORRUPT.
u32 decode_utc_time(const std::vector<u8>& der, u64& out) {
    if (der.size() < 2) return x509::kCryptEAsn1Eod;
    const usize len = der[1];
    if (len >= 0x80 || 2 + len > der.size()) return x509::kCryptEAsn1Eod;
    const u8* t = der.data() + 2;
    usize at = 0;
    auto two = [&](int& v, int base) -> u32 {
        if (at + 2 > len) return x509::kCryptEAsn1Eod;
        if (t[at] < '0' || t[at] > '9' || t[at + 1] < '0' || t[at + 1] > '9') return x509::kCryptEAsn1Corrupt;
        v = (t[at] - '0') * base + (t[at + 1] - '0');
        at += 2;
        return 0;
    };
    int yy = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    for (int* f : {&yy, &month, &day, &hour, &minute})
        if (const u32 e = two(*f, 10)) return e;
    if (at < len && t[at] >= '0' && t[at] <= '9') {
        if (const u32 e = two(second, 10)) return e;
    }
    if (at >= len) return x509::kCryptEAsn1Eod;
    i64 offset_minutes = 0;
    if (t[at] == 'Z') {
        ++at;
    } else if (t[at] == '+' || t[at] == '-') {
        const bool minus = t[at] == '-';
        ++at;
        int oh = 0, om = 0;
        if (const u32 e = two(oh, 10)) return e;
        if (const u32 e = two(om, 16)) return e;
        offset_minutes = (minus ? -1 : 1) * (static_cast<i64>(oh) * 60 + om);
    } else {
        return x509::kCryptEAsn1Corrupt;
    }
    const int year = yy < 50 ? 2000 + yy : 1900 + yy;
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month < 1 || month > 12 || day < 1 ||
        day > kDays[month - 1] + ((month == 2 && leap) ? 1 : 0) || hour > 23 || minute > 59 || second > 59) {
        return x509::kCryptEBadEncode;
    }
    // Days from 1601-01-01 to the date (proleptic Gregorian).
    auto days_from_civil = [](i64 y, i64 m, i64 d) {
        y -= m <= 2;
        const i64 era = (y >= 0 ? y : y - 399) / 400;
        const i64 yoe = y - era * 400;
        const i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe;
    };
    const i64 days = days_from_civil(year, month, day) - days_from_civil(1601, 1, 1);
    const i64 seconds = days * 86400 + hour * 3600 + minute * 60 + second + offset_minutes * 60;
    out = static_cast<u64>(seconds) * 10000000ULL;
    return 0;
}

} // namespace

u32 Crypt32Manager::crypt_decode_object_ex(Marshaler& m) {
    return crypt_decode_object_core(m, m.arg32(0), m.arg_ptr(1), m.arg_ptr(2), m.arg32(3),
                                    m.arg32(4), m.arg_ptr(5), m.arg_ptr(6), m.arg_ptr(7));
}

// BOOL CryptDecodeObject(dwCertEncodingType, lpszStructType, pbEncoded,
//     cbEncoded, dwFlags, pvStructInfo, pcbStructInfo)
// https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptdecodeobject
// Measured (crypt_signer_trio_probe "dec"/"decex" rows, both bitnesses): every
// result, size, last error and output equals CryptDecodeObjectEx with a NULL
// pDecodePara on the same arguments -- including CRYPT_DECODE_ALLOC_FLAG,
// which makes pvStructInfo receive the allocated pointer.
u32 Crypt32Manager::crypt_decode_object(Marshaler& m) {
    return crypt_decode_object_core(m, m.arg32(0), m.arg_ptr(1), m.arg_ptr(2), m.arg32(3),
                                    m.arg32(4), 0, m.arg_ptr(5), m.arg_ptr(6));
}

// Measured (crypt_signer_trio_probe "dec"/"decex" rows): every failure but
// ERROR_MORE_DATA leaves *pcbStructInfo 0.
u32 Crypt32Manager::crypt_decode_object_core(Marshaler& m, u32 encoding, GuestAddr struct_type,
                                             GuestAddr encoded, u32 encoded_len, u32 flags,
                                             GuestAddr decode_para, GuestAddr struct_info,
                                             GuestAddr size_ptr) {
    const u32 ok = crypt_decode_object_body(m, encoding, struct_type, encoded, encoded_len, flags,
                                            decode_para, struct_info, size_ptr);
    auto* cpu = m.cpu();
    const bool faulted = cpu != nullptr && cpu->pending_raise.active;
    if (ok == 0 && size_ptr != 0 && !faulted && get_last_error() != error_codes::ERROR_MORE_DATA) {
        (void)m.write_u32(size_ptr, 0);
    }
    return ok;
}

u32 Crypt32Manager::crypt_decode_object_body(Marshaler& m, u32 encoding, GuestAddr struct_type,
                                             GuestAddr encoded, u32 encoded_len, u32 flags,
                                             GuestAddr decode_para, GuestAddr struct_info,
                                             GuestAddr size_ptr) {
    constexpr u32 X509_ASN_ENCODING = 0x00000001;
    constexpr u32 PKCS_7_ASN_ENCODING = 0x00010000;
    constexpr u32 CRYPT_DECODE_ALLOC_FLAG = 0x8000;
    constexpr u32 kAlternateName = 12;      // X509_ALTERNATE_NAME
    constexpr u32 kEnhancedKeyUsage = 36;   // X509_ENHANCED_KEY_USAGE
    constexpr u32 kPkcs7SignerInfo = 500;   // PKCS7_SIGNER_INFO
    constexpr u32 kPkcsUtcTime = 17;        // PKCS_UTC_TIME

    if (size_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((encoding & (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING)) == 0) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }

    enum class Kind { None, AltName, Eku, SignerInfo, UtcTime };
    Kind kind = Kind::None;
    std::string type_name;
    if ((struct_type >> 16) == 0) {
        const u32 id = static_cast<u32>(struct_type);
        if (id == kAlternateName) kind = Kind::AltName;
        else if (id == kEnhancedKeyUsage) kind = Kind::Eku;
        else if (id == kPkcs7SignerInfo) kind = Kind::SignerInfo;
        else if (id == kPkcsUtcTime) kind = Kind::UtcTime;
        type_name = "#" + std::to_string(id);
    } else {
        auto oid = m.read_ansi_string(struct_type, 128);
        if (oid.is_ok()) {
            type_name = oid.value();
            if (type_name == "2.5.29.17" || type_name == "2.5.29.18" ||
                type_name == "2.5.29.7" || type_name == "2.5.29.8") {
                kind = Kind::AltName;
            } else if (type_name == "2.5.29.37") {
                kind = Kind::Eku;
            } else if (type_name == "1.2.840.113549.1.9.5") {  // szOID_RSA_signingTime
                kind = Kind::UtcTime;
            }
        }
    }
    if (kind == Kind::None) {
        const std::string detail = "lpszStructType " + type_name;
        crypt32::report_unimplemented("CryptDecodeObjectEx", detail.c_str());
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if ((flags & CRYPT_DECODE_ALLOC_FLAG) != 0 && decode_para != 0) {
        crypt32::report_unimplemented("CryptDecodeObjectEx",
                                      "CRYPT_DECODE_PARA pfnAlloc/pfnFree");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    std::vector<u8> bytes(encoded_len);
    if (encoded_len != 0 &&
        m.memory().read_bytes(encoded, bytes.data(), encoded_len).is_err()) {
        crypt32::raise_access_violation(m, encoded, false);
        return 0;
    }
    // Outer SEQUENCE: tag then a length that fits the input.
    if (bytes.empty()) {
        set_last_error(x509::kCryptEAsn1Eod);
        return 0;
    }
    if (bytes[0] != (kind == Kind::UtcTime ? 0x17 : 0x30)) {
        set_last_error(x509::kCryptEAsn1BadTag);
        return 0;
    }
    if (kind == Kind::AltName) {
        // Every GeneralName must be a context-specific [0]..[8] TLV that
        // fits inside the SEQUENCE.
        const auto tlv_length = [&](usize at, usize& header, usize& length) {
            if (at + 2 > bytes.size()) return false;
            const u8 first = bytes[at + 1];
            if (first < 0x80) { header = 2; length = first; }
            else {
                const usize n = first & 0x7f;
                if (n == 0 || n > 4 || at + 2 + n > bytes.size()) return false;
                length = 0;
                for (usize k = 0; k < n; ++k) length = (length << 8) | bytes[at + 2 + k];
                header = 2 + n;
            }
            return at + header + length <= bytes.size();
        };
        usize header = 0, length = 0;
        if (!tlv_length(0, header, length)) {
            set_last_error(x509::kCryptEAsn1Eod);
            return 0;
        }
        for (usize at = header; at < header + length;) {
            usize h = 0, l = 0;
            if (!tlv_length(at, h, l) || at + h + l > header + length) {
                set_last_error(x509::kCryptEAsn1Eod);
                return 0;
            }
            if ((bytes[at] & 0xc0) != 0x80 || (bytes[at] & 0x1f) > 8) {
                set_last_error(x509::kCryptEAsn1BadTag);
                return 0;
            }
            at += h + l;
        }
    }

    crypt32::GuestBlock block(m.cpu_is_64bit());
    const bool is64 = block.is64();
    const u32 ps = block.pointer_size();
    if (kind == Kind::UtcTime) {
        u64 filetime = 0;
        if (const u32 error = decode_utc_time(bytes, filetime); error != 0) {
            set_last_error(error);
            return 0;
        }
        const u32 ft = block.reserve(8);
        block.put_u32(ft, static_cast<u32>(filetime));
        block.put_u32(ft + 4, static_cast<u32>(filetime >> 32));
    } else if (kind == Kind::SignerInfo) {
        // PKCS7_SIGNER_INFO -> CMSG_SIGNER_INFO, laid out exactly as
        // CryptMsgGetParam(CMSG_SIGNER_INFO_PARAM) lays out the same signer
        // (crypt_signer_trio_probe "siparam"/"si" rows: equal sizes). Bad
        // input (the "sibad" rows): an outer length past the data or an empty
        // SEQUENCE is CRYPT_E_ASN1_EOD, any other malformed SignerInfo
        // CRYPT_E_ASN1_CORRUPT.
        usize length = 0, header = 0;
        bool fits = bytes.size() >= 2;
        if (fits) {
            const u8 first = bytes[1];
            if (first < 0x80) {
                header = 2;
                length = first;
            } else {
                const usize n = first & 0x7f;
                fits = n != 0 && n <= 4 && 2 + n <= bytes.size();
                for (usize k = 0; fits && k < n; ++k) length = (length << 8) | bytes[2 + k];
                header = 2 + n;
            }
            fits = fits && header + length <= bytes.size();
        }
        if (!fits || length == 0) {
            set_last_error(x509::kCryptEAsn1Eod);
            return 0;
        }
        auto signer = authenticode::parse_signer_info(std::span<const u8>(bytes).first(header + length));
        if (signer.is_err()) {
            set_last_error(x509::kCryptEAsn1Corrupt);
            return 0;
        }
        (void)crypt32::put_cmsg_signer_info(block, signer.value());
    } else if (kind == Kind::AltName) {
        const auto names = x509::decode_general_names(bytes);
        const u32 entry_size = is64 ? 24u : 12u;    // CERT_ALT_NAME_ENTRY
        const u32 header = block.reserve(is64 ? 16u : 8u, 1);
        block.put_u32(header, static_cast<u32>(names.size()));
        const u32 entries =
            names.empty() ? crypt32::GuestBlock::kNull
                          : block.reserve(static_cast<u32>(names.size()) * entry_size, 1);
        if (entries != crypt32::GuestBlock::kNull) block.put_ptr(header + ps, entries);
        // Union member sits after the DWORD choice, pointer-aligned.
        const u32 member = ps;
        for (usize i = 0; i < names.size(); ++i) {
            const auto& name = names[i];
            const u32 entry = entries + static_cast<u32>(i) * entry_size;
            const u32 choice = static_cast<u32>(name.choice) + 1u;
            block.put_u32(entry, choice);
            switch (choice) {
            case 2:    // CERT_ALT_NAME_RFC822_NAME  (IA5String)
            case 3:    // CERT_ALT_NAME_DNS_NAME
            case 7: {  // CERT_ALT_NAME_URL
                std::vector<u16> wide(name.value.begin(), name.value.end());
                block.put_ptr(entry + member, block.add_wstring(wide, 8));
                break;
            }
            case 5:    // CERT_ALT_NAME_DIRECTORY_NAME: encoded Name blob
            case 8:    // CERT_ALT_NAME_IP_ADDRESS: octets
                block.put_u32(entry + member, static_cast<u32>(name.value.size()));
                if (!name.value.empty()) {
                    block.put_ptr(entry + member + ps, block.add_bytes(name.value, 8));
                }
                break;
            case 9: {  // CERT_ALT_NAME_REGISTERED_ID: dotted OID
                block.put_ptr(entry + member,
                              block.add_cstring(x509::oid_to_string(name.value), 8));
                break;
            }
            default: {
                const std::string detail =
                    "CERT_ALT_NAME choice " + std::to_string(choice);
                crypt32::report_unimplemented("CryptDecodeObjectEx", detail.c_str());
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                return 0;
            }
            }
        }
    } else {
        std::vector<std::string> oids;
        if (const u32 error = x509::decode_enhanced_key_usage(bytes, oids); error != 0) {
            set_last_error(error);
            return 0;
        }
        const u32 header = block.reserve(is64 ? 16u : 8u, 1);
        block.put_u32(header, static_cast<u32>(oids.size()));
        if (!oids.empty()) {
            const u32 array = block.reserve(static_cast<u32>(oids.size()) * ps, 8);
            block.put_ptr(header + ps, array);
            for (usize i = 0; i < oids.size(); ++i) {
                block.put_ptr(array + static_cast<u32>(i) * ps,
                              block.add_cstring(oids[i], 8));
            }
        }
    }
    const u32 total = (block.size() + 7u) & ~7u;

    GuestAddr out = 0;
    if ((flags & CRYPT_DECODE_ALLOC_FLAG) != 0) {
        if (struct_info == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        out = alloc_guest(total);
        if (out == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        (void)m.write_ptr(struct_info, out);
    } else {
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        (void)m.write_u32(size_ptr, total);
        if (struct_info == 0) {
            set_last_error(error_codes::ERROR_SUCCESS);
            return 1;
        }
        if (capacity < total) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            return 0;
        }
        out = struct_info;
    }
    auto image = block.resolve(out);
    image.resize(total, 0);
    if (m.memory().write_bytes(out, reinterpret_cast<const Byte*>(image.data()),
                               image.size()).is_err()) {
        crypt32::raise_access_violation(m, out, true);
        return 0;
    }
    (void)m.write_u32(size_ptr, total);
    set_last_error(error_codes::ERROR_SUCCESS);
    return 1;
}

GuestAddr Crypt32Manager::create_remote_cert_context(
    Marshaler& m, std::span<const std::vector<u8>> peer_chain_der) {
    if (peer_chain_der.empty() || peer_chain_der.front().empty()) return 0;

    // Microsoft Learn (QueryContextAttributes, Schannel): the returned
    // CERT_CONTEXT hCertStore contains the intermediate certificates, if any.
    const u32 store_handle = cert_open_store();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto store = m_store_certs.find(store_handle);
        if (store == m_store_certs.end()) return 0;
        store->second.assign(peer_chain_der.begin() + 1,
                             peer_chain_der.end());
    }

    const GuestAddr context = create_cert_context_from_der(
        m, peer_chain_der.front(), store_handle);
    if (context == 0) {
        (void)cert_close_store(store_handle, 0);
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto entry = m_cert_contexts.find(context);
        if (entry == m_cert_contexts.end()) {
            m_store_certs.erase(store_handle);
            return 0;
        }
        entry->second.owns_store_handle = true;
        entry->second.originating_store_certificates.assign(
            peer_chain_der.begin() + 1, peer_chain_der.end());
    }
    return context;
}

} // namespace vitreo::winapi
