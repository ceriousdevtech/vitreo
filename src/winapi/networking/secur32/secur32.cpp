// SPDX-License-Identifier: LGPL-2.1-only
/// @file secur32.cpp
/// @brief Secur32/Security.dll SSPI — real Schannel over Secure Transport.
///
/// libcurl's schannel backend drives TLS through this package: it owns the TCP
/// socket via
/// ws2_32 and exchanges raw handshake tokens and records through SecBuffers.
/// AcquireCredentialsHandle mints a client credential; InitializeSecurityContext
/// produces the ClientHello and folds in each server flight; EncryptMessage /
/// DecryptMessage are the record layer. The bytes are transformed by
/// schannel_tls (macOS Secure Transport).
///
/// Package dispatch: only the Schannel family ("Schannel", UNISP_NAME,
/// "Default TLS SSP") exists. Every other package name (Negotiate, NTLM,
/// Kerberos, ...) fails with SEC_E_SECPKG_NOT_FOUND, which is what Windows
/// returns for a package it does not have; Windows packages Vitreo lacks also
/// log one "[vitreo] UNIMPLEMENTED" line.
///
/// Every status, attribute mask and buffer layout below is measured on
/// Windows 11 (tools/oracle_probe/sspi_secur32_probe.c and
/// sspi_schannel_stream_probe.c, x86 and x64) unless a Learn page is cited.

#include "secur32.hpp"
#include "schannel_tls.hpp"
#include "winapi/networking/sspicli/sspicli.hpp"
#include "winapi/system/crypt32/crypt32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "ipc/ipc_gate.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

// SECURITY_STATUS values (winerror.h).
constexpr u64 SEC_E_OK                   = 0x00000000ULL;
constexpr u64 SEC_I_CONTINUE_NEEDED      = 0x00090312ULL;
constexpr u64 SEC_I_CONTEXT_EXPIRED      = 0x00090317ULL;
constexpr u64 SEC_E_INSUFFICIENT_MEMORY  = 0x80090300ULL;
constexpr u64 SEC_E_INVALID_HANDLE       = 0x80090301ULL;
constexpr u64 SEC_E_UNSUPPORTED_FUNCTION = 0x80090302ULL;
constexpr u64 SEC_E_INTERNAL_ERROR       = 0x80090304ULL;
constexpr u64 SEC_E_SECPKG_NOT_FOUND     = 0x80090305ULL;
constexpr u64 SEC_E_INVALID_TOKEN        = 0x80090308ULL;
constexpr u64 SEC_E_QOP_NOT_SUPPORTED    = 0x8009030AULL;
constexpr u64 SEC_E_UNKNOWN_CREDENTIALS  = 0x8009030DULL;
constexpr u64 SEC_E_NO_CREDENTIALS       = 0x8009030EULL;
constexpr u64 SEC_E_INCOMPLETE_MESSAGE   = 0x80090318ULL;
constexpr u64 SEC_E_BUFFER_TOO_SMALL     = 0x80090321ULL;
constexpr u64 SEC_E_WRONG_PRINCIPAL      = 0x80090322ULL;
constexpr u64 SEC_E_UNTRUSTED_ROOT       = 0x80090325ULL;
constexpr u64 SEC_E_ILLEGAL_MESSAGE      = 0x80090326ULL;
constexpr u64 SEC_E_CERT_EXPIRED         = 0x80090328ULL;
constexpr u64 SEC_E_ENCRYPT_FAILURE      = 0x80090329ULL;
constexpr u64 SEC_E_DECRYPT_FAILURE      = 0x80090330ULL;
constexpr u64 SEC_E_ALGORITHM_MISMATCH   = 0x80090331ULL;

// SecBuffer.BufferType values (sspi.h).
constexpr u32 SECBUFFER_EMPTY          = 0;
constexpr u32 SECBUFFER_DATA           = 1;
constexpr u32 SECBUFFER_TOKEN          = 2;
constexpr u32 SECBUFFER_MISSING        = 4;
constexpr u32 SECBUFFER_EXTRA          = 5;
constexpr u32 SECBUFFER_STREAM_TRAILER = 6;
constexpr u32 SECBUFFER_STREAM_HEADER  = 7;

// fCredentialUse (sspi.h).
constexpr u32 SECPKG_CRED_INBOUND  = 0x1;
constexpr u32 SECPKG_CRED_OUTBOUND = 0x2;

// ISC_REQ_* / ISC_RET_* (sspi.h). The RET bits Schannel echoes share the
// REQ bit values.
constexpr u32 ISC_REQ_MUTUAL_AUTH            = 0x00000002;
constexpr u32 ISC_REQ_USE_SUPPLIED_CREDS     = 0x00000080;
constexpr u32 ISC_REQ_ALLOCATE_MEMORY        = 0x00000100;
constexpr u32 ISC_REQ_DATAGRAM               = 0x00000400;
constexpr u32 ISC_REQ_CONNECTION             = 0x00000800;
constexpr u32 ISC_REQ_EXTENDED_ERROR         = 0x00004000;
constexpr u32 ISC_REQ_STREAM                 = 0x00008000;
constexpr u32 ISC_REQ_MANUAL_CRED_VALIDATION = 0x00080000;
constexpr u32 ISC_RET_REPLAY_DETECT          = 0x00000004;
constexpr u32 ISC_RET_SEQUENCE_DETECT        = 0x00000008;
constexpr u32 ISC_RET_CONFIDENTIALITY        = 0x00000010;
constexpr u32 ISC_RET_ALLOCATED_MEMORY       = 0x00000100;
constexpr u32 ISC_RET_CONNECTION             = 0x00000800;
constexpr u32 ISC_RET_STREAM                 = 0x00008000;

// EncryptMessage fQOP (sspi.h).
constexpr u32 SECQOP_WRAP_OOB_DATA = 0x40000000;

// schannel.h.
constexpr u32 SCH_CRED_V3                        = 0x00000003;
constexpr u32 SCHANNEL_CRED_VERSION              = 0x00000004;
constexpr u32 SCH_CREDENTIALS_VERSION            = 0x00000005;
constexpr u32 SCH_CRED_NO_SERVERNAME_CHECK       = 0x00000004;
constexpr u32 SCH_CRED_MANUAL_CRED_VALIDATION    = 0x00000008;
constexpr u32 SCH_CRED_REVOCATION_CHECK_END_CERT = 0x00000100;
constexpr u32 SCH_CRED_REVOCATION_CHECK_CHAIN    = 0x00000200;
constexpr u32 SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT = 0x00000400;
constexpr u32 SP_PROT_TLS1_2_CLIENT              = 0x00000800;
constexpr u32 SCHANNEL_SHUTDOWN                  = 1;
constexpr u16 UNISP_RPC_ID                       = 14;

// QueryContextAttributes attribute ids.
constexpr u32 SECPKG_ATTR_STREAM_SIZES         = 4;
constexpr u32 SECPKG_ATTR_APPLICATION_PROTOCOL = 35;
constexpr u32 SECPKG_ATTR_REMOTE_CERT_CONTEXT  = 0x53; // Schannel peer leaf

// The Schannel package as Windows 11 reports it (QuerySecurityPackageInfo and
// EnumerateSecurityPackages, identical for all three names, x86 and x64).
constexpr u32 kSchannelCapabilities = 0x004107B3;
constexpr u16 kSchannelVersion      = 1;
constexpr u32 kSchannelMaxToken     = 0x6000;
constexpr char kSchannelComment[]   = "Schannel Security Package";
// EnumerateSecurityPackages order.
constexpr const char* kSchannelNames[] = {
    "Schannel", "Microsoft Unified Security Protocol Provider", "Default TLS SSP",
};
constexpr usize kSchannelNameCount = sizeof(kSchannelNames) / sizeof(kSchannelNames[0]);

// AcquireCredentialsHandle expiry for a credential without a certificate:
// "a maximum time value" (Learn), measured as 0x7FFFFF36D5969FFF.
constexpr u64 kCredentialNeverExpires = 0x7FFFFF36D5969FFFULL;

// Security packages Windows ships that Vitreo does not implement.
bool is_windows_package(const std::string& name) {
    static const char* kNames[] = {
        "Negotiate", "NTLM", "Kerberos", "Negotiate Extender", "pku2u",
        "WDigest", "Digest", "TSSSP", "CREDSSP", "CloudAP",
    };
    for (const char* n : kNames) {
        if (name.size() != std::char_traits<char>::length(n)) continue;
        bool eq = true;
        for (usize i = 0; i < name.size() && eq; ++i)
            eq = std::tolower(static_cast<unsigned char>(name[i])) ==
                 std::tolower(static_cast<unsigned char>(n[i]));
        if (eq) return true;
    }
    return false;
}

/// Index into kSchannelNames (case-insensitive, exact length), or -1.
int find_package(const std::string& name) {
    for (usize p = 0; p < kSchannelNameCount; ++p) {
        const char* n = kSchannelNames[p];
        if (name.size() != std::char_traits<char>::length(n)) continue;
        bool eq = true;
        for (usize i = 0; i < name.size() && eq; ++i)
            eq = std::tolower(static_cast<unsigned char>(name[i])) ==
                 std::tolower(static_cast<unsigned char>(n[i]));
        if (eq) return static_cast<int>(p);
    }
    return -1;
}

void report_missing_package(const char* api, const std::string& name) {
    if (is_windows_package(name)) {
        sspi_unimplemented_once(std::string("secur32!") + api + ": package " + name +
                                " (Vitreo has no " + name + " security package)");
    }
}

inline void unsupported(Marshaler& m, u32 nargs) {
    for (u32 i = 0; i < nargs; ++i) (void)m.arg(i);
    m.set_return(SEC_E_UNSUPPORTED_FUNCTION);
}

// -- Guest struct accessors (Windows x86/x64 layouts) --------------------

// SecHandle / CredHandle / CtxtHandle = { ULONG_PTR dwLower; ULONG_PTR dwUpper; }.
// The engine id lives in dwLower; dwUpper stays 0.
inline u32 read_handle_id(Marshaler& m, GuestAddr h) {
    if (!h) return 0;
    const u64 lower = m.read_ptr(h).value_or(0);
    return lower > 0xFFFFFFFFull ? 0 : static_cast<u32>(lower);
}
inline bool handle_is_zero(Marshaler& m, GuestAddr h) {
    const u32 ps = m.cpu_is_64bit() ? 8 : 4;
    return m.read_ptr(h).value_or(1) == 0 && m.read_ptr(h + ps).value_or(1) == 0;
}
inline void write_handle_id(Marshaler& m, GuestAddr h, u32 id) {
    if (!h) return;
    (void)m.write_ptr(h, id);
    (void)m.write_ptr(h + (m.cpu_is_64bit() ? 8 : 4), 0);
}
// TimeStamp = LARGE_INTEGER (8 bytes on both bitnesses).
inline void write_timestamp(Marshaler& m, GuestAddr ts, u64 value) {
    if (ts) (void)m.write_u64(ts, value);
}

// A SecBuffer is { ULONG cbBuffer; ULONG BufferType; PVOID pvBuffer; }.
struct SecBufferView {
    GuestAddr entry;   // address of this SecBuffer within the array
    u32 cb;
    u32 type;
    GuestAddr pv;
};

// SecBufferDesc = { ULONG ulVersion; ULONG cBuffers; PSecBuffer pBuffers; }.
struct SecBufferDescView {
    u32 count{0};
    GuestAddr array{0};
    SecBufferView at(Marshaler& m, u32 i) const {
        GuestAddr e = array + static_cast<GuestAddr>(i) * (m.cpu_is_64bit() ? 16 : 12);
        return SecBufferView{e, m.read_u32(e + 0).value_or(0),
                             m.read_u32(e + 4).value_or(0),
                             m.read_ptr(e + 8).value_or(0)};
    }
    void set(Marshaler& m, const SecBufferView& b) const {
        (void)m.write_u32(b.entry + 0, b.cb);
        (void)m.write_u32(b.entry + 4, b.type);
        (void)m.write_ptr(b.entry + 8, b.pv);
    }
};

SecBufferDescView read_desc(Marshaler& m, GuestAddr desc) {
    SecBufferDescView v;
    if (!desc) return v;
    v.count = m.read_u32(desc + 4).value_or(0);
    v.array = m.read_ptr(desc + 8).value_or(0);
    return v;
}

// First buffer of `type`.
bool find_buffer(Marshaler& m, const SecBufferDescView& d, u32 type,
                 SecBufferView& out) {
    for (u32 i = 0; i < d.count; ++i) {
        SecBufferView b = d.at(m, i);
        if (b.type == type) { out = b; return true; }
    }
    return false;
}

std::vector<u8> read_guest(Marshaler& m, GuestAddr p, u32 n) {
    std::vector<u8> v(n);
    if (n) (void)m.memory().read_bytes(p, reinterpret_cast<Byte*>(v.data()), n);
    return v;
}
bool write_guest(Marshaler& m, GuestAddr p, const u8* data, usize n) {
    if (!n) return true;
    return m.memory().write_bytes(p, reinterpret_cast<const Byte*>(data), n).is_ok();
}

// Allocate guest memory the caller will release via FreeContextBuffer.
GuestAddr alloc_context_buffer(Secur32Manager& mgr, usize n) {
    HeapManager* heap = mgr.heap_manager();
    if (!heap || n == 0) return 0;
    return static_cast<GuestAddr>(heap->heap_alloc(heap->get_process_heap(), 0, n));
}

// -- SecPkgInfoA/W --------------------------------------------------------
//
// typedef struct _SecPkgInfoW { unsigned long fCapabilities; unsigned short
//   wVersion; unsigned short wRPCID; unsigned long cbMaxToken; SEC_WCHAR* Name;
//   SEC_WCHAR* Comment; } -- 20 bytes on x86, 32 on x64 (pointers 8-aligned).
//
// One heap block holds the array and every string, so a single
// FreeContextBuffer releases it (Learn: QuerySecurityPackageInfo /
// EnumerateSecurityPackages Remarks).
GuestAddr build_package_infos(Marshaler& m, Secur32Manager& mgr, bool wide,
                              const std::vector<int>& packages) {
    const bool x64 = m.cpu_is_64bit();
    const u32 entry = x64 ? 32 : 20;
    const u32 cs = wide ? 2 : 1;
    usize total = entry * packages.size();
    for (int p : packages) {
        total += (std::char_traits<char>::length(kSchannelNames[p]) + 1) * cs;
        total += (sizeof(kSchannelComment)) * cs;
    }
    const GuestAddr block = alloc_context_buffer(mgr, total);
    if (!block) return 0;
    GuestAddr str = block + entry * packages.size();
    auto put_string = [&](const char* s) {
        const GuestAddr at = str;
        (void)(wide ? m.write_wide_string(at, s) : m.write_ansi_string(at, s));
        str += (std::char_traits<char>::length(s) + 1) * cs;
        return at;
    };
    for (usize i = 0; i < packages.size(); ++i) {
        const GuestAddr e = block + entry * i;
        (void)m.write_u32(e + 0, kSchannelCapabilities);
        (void)m.write_u16(e + 4, kSchannelVersion);
        (void)m.write_u16(e + 6, UNISP_RPC_ID);
        (void)m.write_u32(e + 8, kSchannelMaxToken);
        if (x64) (void)m.write_u32(e + 12, 0);   // alignment padding
        (void)m.write_ptr(e + (x64 ? 16 : 12), put_string(kSchannelNames[packages[i]]));
        (void)m.write_ptr(e + (x64 ? 24 : 16), put_string(kSchannelComment));
    }
    return block;
}

std::string read_text(Marshaler& m, GuestAddr p, bool wide) {
    if (!p) return {};
    auto r = wide ? m.read_wide_string(p) : m.read_ansi_string(p);
    return r.is_ok() ? r.value() : std::string{};
}

// SECURITY_STATUS QuerySecurityPackageInfo(LPTSTR pszPackageName,
//                                          PSecPkgInfo* ppPackageInfo)
template <bool Wide>
void query_security_package_info(Marshaler& m, Secur32Manager& mgr) {
    const std::string name = read_text(m, m.arg_ptr(0), Wide);
    const GuestAddr ppInfo = m.arg_ptr(1);
    const int pkg = find_package(name);
    if (pkg < 0) {
        // Windows leaves *ppPackageInfo untouched.
        report_missing_package(Wide ? "QuerySecurityPackageInfoW"
                                    : "QuerySecurityPackageInfoA", name);
        m.set_return(SEC_E_SECPKG_NOT_FOUND);
        return;
    }
    if (!ppInfo) { m.set_return(SEC_E_INSUFFICIENT_MEMORY); return; }
    const GuestAddr info = build_package_infos(m, mgr, Wide, {pkg});
    if (!info) { m.set_return(SEC_E_INSUFFICIENT_MEMORY); return; }
    (void)m.write_ptr(ppInfo, info);
    m.set_return(SEC_E_OK);
}

// SECURITY_STATUS EnumerateSecurityPackages(unsigned long* pcPackages,
//                                           PSecPkgInfo* ppPackageInfo)
template <bool Wide>
void enumerate_security_packages(Marshaler& m, Secur32Manager& mgr) {
    const GuestAddr pcount = m.arg_ptr(0);
    const GuestAddr ppInfo = m.arg_ptr(1);
    std::vector<int> all;
    for (usize p = 0; p < kSchannelNameCount; ++p) all.push_back(static_cast<int>(p));
    const GuestAddr infos = build_package_infos(m, mgr, Wide, all);
    if (!infos) { m.set_return(SEC_E_INSUFFICIENT_MEMORY); return; }
    if (pcount) (void)m.write_u32(pcount, static_cast<u32>(all.size()));
    if (ppInfo) (void)m.write_ptr(ppInfo, infos);
    m.set_return(SEC_E_OK);
}

// -- AcquireCredentialsHandle ---------------------------------------------

// SECURITY_STATUS AcquireCredentialsHandle(LPTSTR pszPrincipal, LPTSTR
//   pszPackage, unsigned long fCredentialUse, void* pvLogonId, void* pAuthData,
//   SEC_GET_KEY_FN, void*, PCredHandle phCredential, PTimeStamp ptsExpiry)
template <bool Wide>
void acquire_credentials_handle(Marshaler& m) {
    const GuestAddr pkg_ptr = m.arg_ptr(1);
    const u32 use = static_cast<u32>(m.arg(2));
    (void)m.arg(3);
    const GuestAddr auth = m.arg_ptr(4);
    (void)m.arg(5); (void)m.arg(6);
    const GuestAddr phCred = m.arg_ptr(7);
    const GuestAddr expiry = m.arg_ptr(8);
    const char* api = Wide ? "AcquireCredentialsHandleW" : "AcquireCredentialsHandleA";

    if (pkg_ptr == 0) { m.set_return(SEC_E_SECPKG_NOT_FOUND); return; }
    const std::string name = read_text(m, pkg_ptr, Wide);
    if (find_package(name) < 0) {
        // Unknown package: *phCredential and *ptsExpiry stay untouched.
        report_missing_package(api, name);
        m.set_return(SEC_E_SECPKG_NOT_FOUND);
        return;
    }
    // Every Schannel failure below writes a zero expiry and no handle.
    auto fail = [&](u64 status) {
        write_timestamp(m, expiry, 0);
        m.set_return(status);
    };

    // Neither INBOUND nor OUTBOUND, or INBOUND without SCHANNEL_CRED, has no
    // credential to hand out (measured for 0, INBOUND, BOTH, DEFAULT).
    if ((use & (SECPKG_CRED_INBOUND | SECPKG_CRED_OUTBOUND)) == 0 ||
        ((use & SECPKG_CRED_INBOUND) != 0 && auth == 0)) {
        fail(SEC_E_NO_CREDENTIALS);
        return;
    }

    schannel::CredentialConfig cfg;
    cfg.outbound = (use & SECPKG_CRED_OUTBOUND) != 0;
    cfg.inbound = (use & SECPKG_CRED_INBOUND) != 0;
    if (auth) {
        const bool x64 = m.cpu_is_64bit();
        const u32 version = m.read_u32(auth).value_or(0);
        u32 creds = 0, flags = 0;
        if (version == SCH_CRED_V3 || version == SCHANNEL_CRED_VERSION) {
            // SCHANNEL_CRED: dwVersion, cCreds@4, paCred, hRootStore, cMappers,
            // aphMappers, cSupportedAlgs, palgSupportedAlgs,
            // grbitEnabledProtocols@32/56, dwMin/MaxCipherStrength,
            // dwSessionLifespan, dwFlags@48/72, dwCredFormat.
            creds = m.read_u32(auth + 4).value_or(0);
            flags = m.read_u32(auth + (x64 ? 72 : 48)).value_or(0);
            const u32 protocols = m.read_u32(auth + (x64 ? 56 : 32)).value_or(0);
            if (cfg.outbound && protocols != 0 && (protocols & SP_PROT_TLS1_2_CLIENT) == 0) {
                sspi_unimplemented_once(std::string("secur32!") + api +
                    ": grbitEnabledProtocols without TLS 1.2 (Vitreo's Schannel speaks TLS 1.2 only)");
                fail(SEC_E_INTERNAL_ERROR);
                return;
            }
        } else if (version == SCH_CREDENTIALS_VERSION) {
            // SCH_CREDENTIALS: dwVersion, dwCredFormat, cCreds@8, paCred,
            // hRootStore, cMappers, aphMappers, dwSessionLifespan,
            // dwFlags@32/52, cTlsParameters@36/56, pTlsParameters@40/64.
            creds = m.read_u32(auth + 8).value_or(0);
            flags = m.read_u32(auth + (x64 ? 52 : 32)).value_or(0);
            const u32 ntls = m.read_u32(auth + (x64 ? 56 : 36)).value_or(0);
            const GuestAddr tls = m.read_ptr(auth + (x64 ? 64 : 40)).value_or(0);
            // TLS_PARAMETERS: cAlpnIds, rgstrAlpnIds, grbitDisabledProtocols
            // @8/16, cDisabledCrypto@12/20, pDisabledCrypto, dwFlags;
            // 24 bytes on x86, 40 on x64.
            for (u32 i = 0; i < ntls && tls != 0; ++i) {
                const GuestAddr t = tls + i * (x64 ? 40 : 24);
                const u32 disabled = m.read_u32(t + (x64 ? 16 : 8)).value_or(0);
                const u32 crypto = m.read_u32(t + (x64 ? 20 : 12)).value_or(0);
                if (cfg.outbound && (disabled & SP_PROT_TLS1_2_CLIENT) != 0) {
                    sspi_unimplemented_once(std::string("secur32!") + api +
                        ": TLS_PARAMETERS disables TLS 1.2 (Vitreo's Schannel speaks TLS 1.2 only)");
                    fail(SEC_E_INTERNAL_ERROR);
                    return;
                }
                if (crypto != 0) {
                    sspi_unimplemented_once(std::string("secur32!") + api +
                        ": TLS_PARAMETERS.pDisabledCrypto");
                    fail(SEC_E_INTERNAL_ERROR);
                    return;
                }
            }
        } else {
            fail(SEC_E_UNKNOWN_CREDENTIALS);
            return;
        }
        if (cfg.outbound && creds != 0) {
            sspi_unimplemented_once(std::string("secur32!") + api +
                                    ": client certificate (cCreds != 0)");
            fail(SEC_E_INTERNAL_ERROR);
            return;
        }
        cfg.verify_peer = (flags & SCH_CRED_MANUAL_CRED_VALIDATION) == 0;
        cfg.check_server_name = (flags & SCH_CRED_NO_SERVERNAME_CHECK) == 0;
        if (cfg.verify_peer &&
            (flags & (SCH_CRED_REVOCATION_CHECK_END_CERT | SCH_CRED_REVOCATION_CHECK_CHAIN |
                      SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT)) != 0) {
            // Logged, not failed: curl requests chain revocation checks by
            // default, and Secure Transport's evaluation here performs none.
            sspi_unimplemented_once(std::string("secur32!") + api +
                ": SCH_CRED_REVOCATION_CHECK_* (no revocation checking is performed)");
        }
    }

    const u32 cred = schannel::create_credential(cfg);
    write_handle_id(m, phCred, cred);
    write_timestamp(m, expiry, kCredentialNeverExpires);
    VITREO_TRACE("[secur32] AcquireCredentialsHandle(\"%s\") -> cred=0x%x verify=%d\n",
                 name.c_str(), cred, cfg.verify_peer);
    ipc::ipc_log("AcquireCredentialsHandle cred=0x%x verify=%d", cred, cfg.verify_peer ? 1 : 0);
    m.set_return(SEC_E_OK);
}

// -- InitializeSecurityContext --------------------------------------------

/// pfContextAttr Schannel reports for a request (measured one ISC_REQ bit at a
/// time): replay/sequence/confidentiality always; STREAM unless CONNECTION was
/// requested; ALLOCATED_MEMORY when it allocated; and the echoed request bits.
u32 context_attributes(u32 req) {
    u32 a = ISC_RET_REPLAY_DETECT | ISC_RET_SEQUENCE_DETECT | ISC_RET_CONFIDENTIALITY;
    if ((req & ISC_REQ_CONNECTION) != 0) a |= ISC_RET_CONNECTION;
    if ((req & ISC_REQ_CONNECTION) == 0 || (req & ISC_REQ_STREAM) != 0) a |= ISC_RET_STREAM;
    if ((req & ISC_REQ_ALLOCATE_MEMORY) != 0) a |= ISC_RET_ALLOCATED_MEMORY;
    a |= req & (ISC_REQ_MUTUAL_AUTH | ISC_REQ_USE_SUPPLIED_CREDS | ISC_REQ_DATAGRAM |
                ISC_REQ_EXTENDED_ERROR | ISC_REQ_MANUAL_CRED_VALIDATION);
    return a;
}
/// Attributes Schannel writes when it rejects the call before looking at the
/// request (no output buffer, NULL credential on a later leg, ...).
constexpr u32 kEarlyFailureAttributes =
    ISC_RET_REPLAY_DETECT | ISC_RET_SEQUENCE_DETECT | ISC_RET_CONFIDENTIALITY;

u64 handshake_failure_status(schannel::TlsSession::Failure f) {
    using F = schannel::TlsSession::Failure;
    switch (f) {
    case F::PeerAlert:      return SEC_E_ILLEGAL_MESSAGE;   // measured: fatal alert
    case F::Protocol:       return SEC_E_ILLEGAL_MESSAGE;
    case F::Negotiation:    return SEC_E_ALGORITHM_MISMATCH;
    case F::UntrustedRoot:  return SEC_E_UNTRUSTED_ROOT;
    case F::CertExpired:    return SEC_E_CERT_EXPIRED;
    case F::WrongPrincipal: return SEC_E_WRONG_PRINCIPAL;
    default:                return SEC_E_INVALID_TOKEN;
    }
}

/// Bytes Schannel reports as missing for an incomplete handshake record: the
/// rest of the 5-byte record header, else the rest of the record.
u32 handshake_missing_bytes(const std::vector<u8>& in) {
    if (in.size() < 5) return static_cast<u32>(5 - in.size());
    const usize rec = 5 + (static_cast<usize>(in[3]) << 8 | in[4]);
    return rec > in.size() ? static_cast<u32>(rec - in.size()) : 0;
}

template <bool Wide>
void initialize_security_context(Marshaler& m, Secur32Manager& mgr) {
    GuestAddr phCred    = m.arg_ptr(0);
    GuestAddr phContext = m.arg_ptr(1);
    GuestAddr target    = m.arg_ptr(2);
    u32       reqFlags  = static_cast<u32>(m.arg(3));
    GuestAddr pInput    = m.arg_ptr(6);
    GuestAddr phNew     = m.arg_ptr(8);
    GuestAddr pOutput   = m.arg_ptr(9);
    GuestAddr pfAttr    = m.arg_ptr(10);
    GuestAddr expiry    = m.arg_ptr(11);

    // 1. An existing context must be live (a zeroed handle is not "no
    //    context": Windows rejects it). Nothing is written.
    u32 ctx_id = 0;
    schannel::TlsSession* sess = nullptr;
    if (phContext) {
        ctx_id = read_handle_id(m, phContext);
        sess = schannel::get_context(ctx_id);
        if (!sess) { m.set_return(SEC_E_INVALID_HANDLE); return; }
    }

    // 2. The credential is required on every leg (a NULL credential on a
    //    later leg is rejected too, with the early attribute mask).
    auto early_fail = [&](u64 status) {
        if (pfAttr) (void)m.write_u32(pfAttr, kEarlyFailureAttributes);
        write_timestamp(m, expiry, 0);
        m.set_return(status);
    };
    schannel::CredentialConfig cfg;
    if (!phCred || !schannel::get_credential(read_handle_id(m, phCred), cfg)) {
        if (sess) early_fail(SEC_E_INVALID_HANDLE);
        else m.set_return(SEC_E_INVALID_HANDLE);
        return;
    }

    // 3. The output descriptor must offer a token buffer: a TOKEN buffer, or
    //    an EMPTY one that Schannel may allocate into.
    const bool alloc = (reqFlags & ISC_REQ_ALLOCATE_MEMORY) != 0;
    SecBufferDescView out_desc = read_desc(m, pOutput);
    SecBufferView tok{};
    bool have_tok = pOutput != 0 && find_buffer(m, out_desc, SECBUFFER_TOKEN, tok);
    if (!have_tok && pOutput != 0 && alloc)
        have_tok = find_buffer(m, out_desc, SECBUFFER_EMPTY, tok);
    if (!have_tok) { early_fail(SEC_E_INVALID_TOKEN); return; }
    if (!alloc && tok.pv == 0) { early_fail(SEC_E_INSUFFICIENT_MEMORY); return; }

    const u32 attr = context_attributes(reqFlags);
    auto finish_attr = [&](u64 exp) {
        if (pfAttr) (void)m.write_u32(pfAttr, attr);
        write_timestamp(m, expiry, exp);
    };

    // 4. A credential without an outbound half has no client protocol.
    if (!cfg.outbound) {
        finish_attr(0);
        m.set_return(SEC_E_ALGORITHM_MISMATCH);
        return;
    }
    if ((reqFlags & ISC_REQ_DATAGRAM) != 0) {
        sspi_unimplemented_once(std::string("secur32!InitializeSecurityContext") +
                                (Wide ? "W" : "A") + ": ISC_REQ_DATAGRAM (DTLS)");
        finish_attr(0);
        m.set_return(SEC_E_UNSUPPORTED_FUNCTION);
        return;
    }

    // 5. First leg: create the context bound to the target host.
    bool created = false;
    if (!sess) {
        const std::string host = read_text(m, target, Wide);
        if ((reqFlags & ISC_REQ_MANUAL_CRED_VALIDATION) != 0) cfg.verify_peer = false;
        ctx_id = schannel::create_context(cfg, host);
        sess = schannel::get_context(ctx_id);
        if (!sess) { finish_attr(0); m.set_return(SEC_E_INSUFFICIENT_MEMORY); return; }
        created = true;
        VITREO_TRACE("[secur32] InitializeSecurityContext: new ctx=0x%x host=\"%s\"\n",
                     ctx_id, host.c_str());
        ipc::ipc_log("InitializeSecurityContext NEW ctx=0x%x host=\"%s\"",
                     ctx_id, host.c_str());
    }
    auto discard = [&]() { if (created) (void)schannel::free_context(ctx_id); };

    // 6. Deliver `out` through the token buffer. Without ISC_REQ_ALLOCATE_
    //    MEMORY nothing is written unless the whole token fits.
    auto deliver = [&](const std::vector<u8>& out) -> u64 {
        if (out.empty()) {
            tok.cb = 0;
            tok.type = SECBUFFER_TOKEN;
            out_desc.set(m, tok);
            return SEC_E_OK;
        }
        GuestAddr buf = tok.pv;
        if (alloc) {
            buf = alloc_context_buffer(mgr, out.size());
            if (!buf) return SEC_E_INSUFFICIENT_MEMORY;
        } else if (out.size() > tok.cb) {
            return SEC_E_BUFFER_TOO_SMALL;
        }
        if (!write_guest(m, buf, out.data(), out.size())) return SEC_E_INSUFFICIENT_MEMORY;
        tok.pv = buf;
        tok.cb = static_cast<u32>(out.size());
        tok.type = SECBUFFER_TOKEN;
        out_desc.set(m, tok);
        return SEC_E_OK;
    };

    if (sess->shutdown_pending()) {
        std::vector<u8> out;
        if (!sess->emit_shutdown(out)) {
            finish_attr(sess->peer_not_after_filetime());
            m.set_return(SEC_E_INTERNAL_ERROR);
            return;
        }
        const u64 st = deliver(out);
        finish_attr(sess->peer_not_after_filetime());
        if (st != SEC_E_OK) { m.set_return(st); return; }
        write_handle_id(m, phNew, ctx_id);
        VITREO_TRACE("[secur32] InitializeSecurityContext ctx=0x%x shutdown out=%zu\n",
                     ctx_id, out.size());
        m.set_return(SEC_E_OK);
        return;
    }

    // 7. Gather the server token, if any.
    std::vector<u8> in;
    SecBufferView in_tok{};
    bool have_in_tok = false;
    SecBufferDescView in_desc = read_desc(m, pInput);
    if (pInput && find_buffer(m, in_desc, SECBUFFER_TOKEN, in_tok)) {
        have_in_tok = true;
        in = read_guest(m, in_tok.pv, in_tok.cb);
    }

    if (std::getenv("VITREO_SSPI_DIAG")) {
        auto dump = [&](const char* tag, GuestAddr desc) {
            if (!desc) { VITREO_TRACE("[secur32]   %s=null\n", tag); return; }
            SecBufferDescView d = read_desc(m, desc);
            VITREO_TRACE("[secur32]   %s: count=%u\n", tag, d.count);
            for (u32 i = 0; i < d.count && i < 6; ++i) {
                SecBufferView b = d.at(m, i);
                VITREO_TRACE("[secur32]     [%u] type=%u cb=%u pv=0x%llx\n", i, b.type, b.cb,
                             static_cast<unsigned long long>(b.pv));
            }
        };
        dump("pInput", pInput);
        dump("pOutput", pOutput);
    }

    size_t consumed = 0;
    std::vector<u8> out;
    const schannel::TlsSession::Step step =
        sess->handshake(in.empty() ? nullptr : in.data(), in.size(), consumed, out);

    if (step == schannel::TlsSession::Step::NeedMoreInput) {
        // SECBUFFER_MISSING in the second input buffer; the context handle and
        // the output buffers are left as passed.
        if (have_in_tok && in_desc.count >= 2) {
            SecBufferView miss = in_desc.at(m, 1);
            miss.type = SECBUFFER_MISSING;
            miss.cb = handshake_missing_bytes(in);
            miss.pv = 0;
            in_desc.set(m, miss);
        }
        finish_attr(sess->peer_not_after_filetime());
        m.set_return(SEC_E_INCOMPLETE_MESSAGE);
        return;
    }
    if (step == schannel::TlsSession::Step::Fatal) {
        const u64 st = handshake_failure_status(sess->last_failure());
        finish_attr(sess->peer_not_after_filetime());
        discard();
        VITREO_TRACE("[secur32] InitializeSecurityContext ctx=0x%x failed -> 0x%llx\n",
                     ctx_id, static_cast<unsigned long long>(st));
        m.set_return(st);
        return;
    }

    const u64 status = step == schannel::TlsSession::Step::Complete ? SEC_E_OK
                                                                     : SEC_I_CONTINUE_NEEDED;
    const u64 delivered = deliver(out);
    if (delivered != SEC_E_OK) {
        finish_attr(0);
        discard();
        m.set_return(delivered);
        return;
    }

    // Leftover input goes back as SECBUFFER_EXTRA in the second input buffer
    // so the caller re-presents it (a TLS record split across socket reads).
    if (have_in_tok && in_desc.count >= 2) {
        SecBufferView extra = in_desc.at(m, 1);
        const size_t leftover = in.size() - consumed;
        if (leftover > 0) {
            extra.type = SECBUFFER_EXTRA;
            extra.pv = in_tok.pv + static_cast<GuestAddr>(consumed);
            extra.cb = static_cast<u32>(leftover);
        } else {
            extra.type = SECBUFFER_EMPTY;
            extra.cb = 0;
        }
        in_desc.set(m, extra);
    }

    write_handle_id(m, phNew, ctx_id);
    // Expiry: 0 until the server certificate arrives, then its NotAfter (UTC).
    finish_attr(sess->peer_not_after_filetime());

    VITREO_TRACE("[secur32] InitializeSecurityContext ctx=0x%x in=%zu consumed=%zu out=%zu -> 0x%llx\n",
                 ctx_id, in.size(), consumed, out.size(),
                 static_cast<unsigned long long>(status));
    ipc::ipc_log(
        "InitializeSecurityContext ctx=0x%x in=%zu out=%zu -> 0x%llx",
        ctx_id, in.size(), out.size(),
        static_cast<unsigned long long>(status));
    m.set_return(status);
}

// -- QueryContextAttributes -----------------------------------------------

void query_context_attributes(Marshaler& m, Secur32Manager& mgr) {
    // (0)phContext (1)ulAttribute (2)pBuffer
    u32 ctx_id = read_handle_id(m, m.arg_ptr(0));
    u32 attr   = static_cast<u32>(m.arg(1));
    GuestAddr buf = m.arg_ptr(2);
    schannel::TlsSession* sess = schannel::get_context(ctx_id);
    if (!sess) { m.set_return(SEC_E_INVALID_HANDLE); return; }

    if (attr == SECPKG_ATTR_STREAM_SIZES && buf) {
        // Windows rejects STREAM_SIZES until the handshake completes.
        if (!sess->complete()) { m.set_return(SEC_E_INVALID_HANDLE); return; }
        // SecPkgContext_StreamSizes { cbHeader; cbTrailer; cbMaximumMessage;
        //   cBuffers; cbBlockSize; }. TLS 1.2 AES-GCM: 13/16/16384/4/16.
        (void)m.write_u32(buf + 0,  static_cast<u32>(sess->header_size()));
        (void)m.write_u32(buf + 4,  static_cast<u32>(sess->trailer_size()));
        (void)m.write_u32(buf + 8,  static_cast<u32>(sess->max_message()));
        (void)m.write_u32(buf + 12, 4);   // cBuffers used by EncryptMessage
        (void)m.write_u32(buf + 16, 16);  // cbBlockSize: AES block
        m.set_return(SEC_E_OK);
        return;
    }
    if (attr == SECPKG_ATTR_APPLICATION_PROTOCOL && buf) {
        // SecPkgContext_ApplicationProtocol { ULONG ProtoNegoStatus; ULONG
        //   ProtoNegoExt; UCHAR ProtocolIdSize; UCHAR ProtocolId[255]; }.
        // No ALPN is offered in the ClientHello, so "not negotiated" (0) is
        // the true answer; curl then uses HTTP/1.1.
        (void)m.write_u32(buf + 0, 0);   // ProtoNegoStatus = None
        (void)m.write_u32(buf + 4, 0);   // ProtoNegoExt = None
        (void)m.write_u8(buf + 8, 0);    // ProtocolIdSize = 0
        m.set_return(SEC_E_OK);
        return;
    }
    // Microsoft Learn (Schannel QueryContextAttributes):
    // SECPKG_ATTR_REMOTE_CERT_CONTEXT → *pBuffer = PCCERT_CONTEXT leaf.
    // Caller frees with CertFreeCertificateContext.
    if (attr == SECPKG_ATTR_REMOTE_CERT_CONTEXT && buf) {
        Crypt32Manager* crypt32 = mgr.crypt32_manager();
        const auto& der = sess->peer_certificate_der();
        if (!crypt32 || der.empty()) {
            VITREO_TRACE(
                "[secur32] QueryContextAttributes REMOTE_CERT_CONTEXT "
                "unavailable (crypt32=%p der=%zu)\n",
                static_cast<void*>(crypt32), der.size());
            m.set_return(SEC_E_UNSUPPORTED_FUNCTION);
            return;
        }
        const GuestAddr ctx = crypt32->create_remote_cert_context(
            m, sess->peer_certificate_chain_der());
        if (ctx == 0) {
            m.set_return(SEC_E_INSUFFICIENT_MEMORY);
            return;
        }
        (void)m.write_ptr(buf, ctx);
        ipc::ipc_log(
            "QueryContextAttributes REMOTE_CERT_CONTEXT -> ctx=0x%llx "
            "der=%zu chain=%zu",
            static_cast<unsigned long long>(ctx), der.size(),
            sess->peer_certificate_chain_der().size());
        m.set_return(SEC_E_OK);
        return;
    }
    VITREO_TRACE("[secur32] QueryContextAttributes attr=%u -> unsupported\n", attr);
    m.set_return(SEC_E_UNSUPPORTED_FUNCTION);
}

// -- EncryptMessage (Schannel) --------------------------------------------
//
// Buffers are found by type (order does not matter) and each receives its own
// part of the record: header -> STREAM_HEADER, ciphertext in place in DATA,
// tag -> STREAM_TRAILER; they need not be contiguous. Measured failures:
// no DATA -> SEC_E_INVALID_TOKEN; no header/trailer, header size other than
// cbHeader, trailer below cbTrailer, or data above cbMaximumMessage ->
// SEC_E_ENCRYPT_FAILURE; handshake not finished -> SEC_E_INVALID_HANDLE.
void encrypt_message(Marshaler& m) {
    // (0)phContext (1)fQOP (2)pMessage (3)MessageSeqNo
    const u32 ctx_id = read_handle_id(m, m.arg_ptr(0));
    const u32 qop = static_cast<u32>(m.arg(1));
    const GuestAddr pMsg = m.arg_ptr(2);
    (void)m.arg(3);  // Schannel does not use sequence numbers (Learn)
    schannel::TlsSession* sess = schannel::get_context(ctx_id);
    if (!sess || !sess->complete()) { m.set_return(SEC_E_INVALID_HANDLE); return; }

    const SecBufferDescView d = read_desc(m, pMsg);
    SecBufferView hdr{}, data{}, trl{};
    if (!find_buffer(m, d, SECBUFFER_DATA, data)) { m.set_return(SEC_E_INVALID_TOKEN); return; }
    if (qop != 0) {
        // SECQOP_WRAP_OOB_DATA sends a TLS alert built by the caller.
        sspi_unimplemented_once(qop == SECQOP_WRAP_OOB_DATA
            ? "secur32!EncryptMessage: SECQOP_WRAP_OOB_DATA"
            : "secur32!EncryptMessage: fQOP != 0");
        m.set_return(SEC_E_QOP_NOT_SUPPORTED);
        return;
    }
    const u32 header = static_cast<u32>(sess->header_size());
    const u32 trailer = static_cast<u32>(sess->trailer_size());
    if (!find_buffer(m, d, SECBUFFER_STREAM_HEADER, hdr) ||
        !find_buffer(m, d, SECBUFFER_STREAM_TRAILER, trl) ||
        hdr.cb != header || hdr.pv == 0 || trl.cb < trailer || trl.pv == 0 ||
        data.cb > sess->max_message() || (data.cb != 0 && data.pv == 0)) {
        m.set_return(SEC_E_ENCRYPT_FAILURE);
        return;
    }
    if (data.cb == 0) {
        // Windows emits a record with an empty payload; Secure Transport's
        // SSLWrite produces no record for zero bytes.
        sspi_unimplemented_once("secur32!EncryptMessage: zero-length message");
        m.set_return(SEC_E_ENCRYPT_FAILURE);
        return;
    }

    const std::vector<u8> plain = read_guest(m, data.pv, data.cb);
    std::vector<u8> record;
    if (!sess->encrypt(plain.data(), plain.size(), record)) {
        m.set_return(SEC_E_ENCRYPT_FAILURE);
        return;
    }
    // TLS 1.2 AES-GCM: header(13) | ciphertext(len) | tag(16).
    if (record.size() != static_cast<usize>(header) + plain.size() + trailer) {
        sspi_unimplemented_once("secur32!EncryptMessage: record shape other than one AES-GCM record");
        m.set_return(SEC_E_INTERNAL_ERROR);
        return;
    }
    const bool ok = write_guest(m, hdr.pv, record.data(), header) &&
                    write_guest(m, data.pv, record.data() + header, plain.size()) &&
                    write_guest(m, trl.pv, record.data() + header + plain.size(), trailer);
    if (!ok) { m.set_return(SEC_E_ENCRYPT_FAILURE); return; }
    hdr.cb = header;
    trl.cb = trailer;
    d.set(m, hdr); d.set(m, data); d.set(m, trl);
    VITREO_TRACE("[secur32] EncryptMessage ctx=0x%x plaintext=%zu -> record=%zu\n",
                 ctx_id, plain.size(), record.size());
    ipc::ipc_log("EncryptMessage ctx=0x%x plaintext=%zu -> record=%zu",
                 ctx_id, plain.size(), record.size());
    m.set_return(SEC_E_OK);
}

// -- DecryptMessage (Schannel) --------------------------------------------
//
// One record per call, decrypted in place. Measured layout on success:
// [0] STREAM_HEADER {base, cbHeader}, [1] DATA {base+cbHeader, plaintext},
// [2] STREAM_TRAILER {after the plaintext, rest of the input},
// [3] EXTRA {next record, bytes} or EMPTY {NULL, 0}. Fewer than four buffers
// or no DATA buffer -> SEC_E_INVALID_TOKEN; incomplete record ->
// SEC_E_INCOMPLETE_MESSAGE with the DATA buffer and the first EMPTY buffer
// both turned into SECBUFFER_MISSING{needed}; close_notify ->
// SEC_I_CONTEXT_EXPIRED; bad record -> SEC_E_DECRYPT_FAILURE. Buffers are
// untouched on every failure. pfQOP is never written.
void decrypt_message(Marshaler& m) {
    // (0)phContext (1)pMessage (2)MessageSeqNo (3)pfQOP
    const u32 ctx_id = read_handle_id(m, m.arg_ptr(0));
    const GuestAddr pMsg = m.arg_ptr(1);
    (void)m.arg(2); (void)m.arg(3);
    schannel::TlsSession* sess = schannel::get_context(ctx_id);
    if (!sess || !sess->complete()) { m.set_return(SEC_E_INVALID_HANDLE); return; }

    const SecBufferDescView d = read_desc(m, pMsg);
    SecBufferView data{};
    if (d.count < 4 || !find_buffer(m, d, SECBUFFER_DATA, data)) {
        m.set_return(SEC_E_INVALID_TOKEN);
        return;
    }

    std::vector<u8> cipher = read_guest(m, data.pv, data.cb);
    size_t consumed = 0;
    std::vector<u8> plain;
    const schannel::TlsSession::DecStep step =
        sess->decrypt(cipher.data(), cipher.size(), consumed, plain);

    if (step == schannel::TlsSession::DecStep::NeedMoreInput) {
        const usize header = sess->header_size();
        u32 missing;
        if (cipher.size() < header) {
            missing = static_cast<u32>(header - cipher.size());
        } else {
            const usize rec = 5 + (static_cast<usize>(cipher[3]) << 8 | cipher[4]);
            missing = rec > cipher.size() ? static_cast<u32>(rec - cipher.size()) : 0;
        }
        SecBufferView miss = data;
        miss.type = SECBUFFER_MISSING;
        miss.cb = missing;
        d.set(m, miss);
        for (u32 i = 0; i < d.count; ++i) {
            SecBufferView b = d.at(m, i);
            if (b.entry == data.entry || b.type != SECBUFFER_EMPTY) continue;
            b.type = SECBUFFER_MISSING;
            b.cb = missing;
            b.pv = 0;
            d.set(m, b);
            break;
        }
        m.set_return(SEC_E_INCOMPLETE_MESSAGE);
        return;
    }
    if (step == schannel::TlsSession::DecStep::Fatal) {
        m.set_return(SEC_E_DECRYPT_FAILURE);
        return;
    }
    if (step == schannel::TlsSession::DecStep::Closed) {
        VITREO_TRACE("[secur32] DecryptMessage ctx=0x%x close_notify\n", ctx_id);
        m.set_return(SEC_I_CONTEXT_EXPIRED);
        return;
    }

    const u32 header = static_cast<u32>(sess->header_size());
    const u32 plen = static_cast<u32>(plain.size());
    if (consumed < static_cast<usize>(header) + plen ||
        !write_guest(m, data.pv + header, plain.data(), plain.size())) {
        m.set_return(SEC_E_DECRYPT_FAILURE);
        return;
    }
    const u32 total = static_cast<u32>(cipher.size());
    const u32 leftover = static_cast<u32>(cipher.size() - consumed);
    SecBufferView b0 = d.at(m, 0), b1 = d.at(m, 1), b2 = d.at(m, 2), b3 = d.at(m, 3);
    b0.type = SECBUFFER_STREAM_HEADER; b0.pv = data.pv;                   b0.cb = header;
    b1.type = SECBUFFER_DATA;          b1.pv = data.pv + header;          b1.cb = plen;
    b2.type = SECBUFFER_STREAM_TRAILER; b2.pv = data.pv + header + plen;  b2.cb = total - header - plen;
    if (leftover > 0) {
        b3.type = SECBUFFER_EXTRA; b3.pv = data.pv + static_cast<GuestAddr>(consumed); b3.cb = leftover;
    } else {
        b3.type = SECBUFFER_EMPTY; b3.pv = 0; b3.cb = 0;
    }
    d.set(m, b0); d.set(m, b1); d.set(m, b2); d.set(m, b3);

    VITREO_TRACE("[secur32] DecryptMessage ctx=0x%x cipher=%zu consumed=%zu plaintext=%zu\n",
                 ctx_id, cipher.size(), consumed, plain.size());
    ipc::ipc_log("DecryptMessage ctx=0x%x cipher=%zu plaintext=%zu",
                 ctx_id, cipher.size(), plain.size());
    if (std::getenv("VITREO_SSPI_DIAG") && !plain.empty()) {
        const size_t cap = plain.size() < 2048 ? plain.size() : 2048;
        std::string head(reinterpret_cast<const char*>(plain.data()), cap);
        for (char& c : head) if (c == '\r' || c == '\n') c = ' ';
        VITREO_TRACE("[secur32]   plaintext[0:%zu]=\"%s\"\n", cap, head.c_str());
    }
    m.set_return(SEC_E_OK);
}

// Build (once per character width) the guest SecurityFunctionTable of thunk
// addresses and return it.
template <bool Wide>
void init_security_interface(Marshaler& m, Secur32Manager& mgr) {
    GuestAddr& cached = Wide ? mgr.func_table_w : mgr.func_table_a;
    if (cached != 0) { m.set_return(cached); return; }

    ModuleManager* mm = mgr.module_manager();
    HeapManager* heap = mgr.heap_manager();
    if (!mm || !heap) { m.set_return(0); return; }

    GuestAddr base = mm->get_module_handle("security.dll");
    if (base == 0) base = mm->get_module_handle("secur32.dll");
    if (base == 0) { m.set_return(0); return; }

    // SecurityFunctionTableA/W field offsets (32-bit), from sspi.h.
    struct Slot { u32 off; const char* name; };
    static const Slot kSlotsA[] = {
        {4,   "EnumerateSecurityPackagesA"},
        {12,  "AcquireCredentialsHandleA"},
        {16,  "FreeCredentialsHandle"},
        {24,  "InitializeSecurityContextA"},
        {28,  "AcceptSecurityContext"},
        {32,  "CompleteAuthToken"},
        {36,  "DeleteSecurityContext"},
        {40,  "ApplyControlToken"},
        {44,  "QueryContextAttributesA"},
        {64,  "FreeContextBuffer"},
        {68,  "QuerySecurityPackageInfoA"},
        {100, "EncryptMessage"},
        {104, "DecryptMessage"},
        {108, "SetContextAttributesA"},
    };
    static const Slot kSlotsW[] = {
        {4,   "EnumerateSecurityPackagesW"},
        {12,  "AcquireCredentialsHandleW"},
        {16,  "FreeCredentialsHandle"},
        {24,  "InitializeSecurityContextW"},
        {28,  "AcceptSecurityContext"},
        {32,  "CompleteAuthToken"},
        {36,  "DeleteSecurityContext"},
        {40,  "ApplyControlToken"},
        {44,  "QueryContextAttributesW"},
        {64,  "FreeContextBuffer"},
        {68,  "QuerySecurityPackageInfoW"},
        {100, "EncryptMessage"},
        {104, "DecryptMessage"},
        {108, "SetContextAttributesW"},
    };
    const u32 pointer_size = m.cpu_is_64bit() ? 8 : 4;
    // dwVersion 4 (SECURITY_SUPPORT_PROVIDER_INTERFACE_VERSION_4) tables run
    // through QueryCredentialsAttributesEx: 31 function slots after dwVersion.
    const u32 kTableSize = 32 * pointer_size;
    GuestAddr table = static_cast<GuestAddr>(heap->heap_alloc(heap->get_process_heap(), 0, kTableSize));
    if (table == 0) { m.set_return(0); return; }
    for (u32 i = 0; i < kTableSize; i += 4) (void)m.write_u32(table + i, 0);
    (void)m.write_u32(table + 0, 4);  // dwVersion (SECPKG interface version)
    const Slot* slots = Wide ? kSlotsW : kSlotsA;
    const usize slot_count = Wide ? std::size(kSlotsW) : std::size(kSlotsA);
    for (usize i = 0; i < slot_count; ++i) {
        const Slot& s = slots[i];
        GuestAddr fn = mm->get_proc_address(base, s.name);
        // The DWORD version is padded to pointer alignment on x64.
        (void)m.write_ptr(table + (s.off / 4) * pointer_size, fn);
    }
    cached = table;
    VITREO_TRACE("[secur32] InitSecurityInterface%c -> table=0x%llx (base=0x%llx)\n",
                 Wide ? 'W' : 'A', static_cast<unsigned long long>(table),
                 static_cast<unsigned long long>(base));
    m.set_return(table);
}

} // namespace

DllExports create_secur32_thunks(Secur32Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(40);
    Secur32Manager* pmgr = &mgr;

    exports.entries.push_back(ExportEntry{"AcquireCredentialsHandleA", 0,
        [](Marshaler& m) { acquire_credentials_handle<false>(m); }, 36});
    exports.entries.push_back(ExportEntry{"AcquireCredentialsHandleW", 0,
        [](Marshaler& m) { acquire_credentials_handle<true>(m); }, 36});

    exports.entries.push_back(ExportEntry{"InitializeSecurityContextA", 0,
        [pmgr](Marshaler& m) { initialize_security_context<false>(m, *pmgr); }, 48});
    exports.entries.push_back(ExportEntry{"InitializeSecurityContextW", 0,
        [pmgr](Marshaler& m) { initialize_security_context<true>(m, *pmgr); }, 48});

    exports.entries.push_back(ExportEntry{"QueryContextAttributesA", 0,
        [&mgr](Marshaler& m) { query_context_attributes(m, mgr); }, 12});
    exports.entries.push_back(ExportEntry{"QueryContextAttributesW", 0,
        [&mgr](Marshaler& m) { query_context_attributes(m, mgr); }, 12});

    exports.entries.push_back(ExportEntry{"EncryptMessage", 0,
        [](Marshaler& m) { encrypt_message(m); }, 16});
    exports.entries.push_back(ExportEntry{"DecryptMessage", 0,
        [](Marshaler& m) { decrypt_message(m); }, 16});

    // A zeroed CtxtHandle is accepted (SEC_E_OK); an unknown or already
    // deleted one is SEC_E_INVALID_HANDLE.
    exports.entries.push_back(ExportEntry{"DeleteSecurityContext", 0,
        [](Marshaler& m) {
            const GuestAddr h = m.arg_ptr(0);
            if (h == 0) { m.set_return(SEC_E_INVALID_HANDLE); return; }
            if (handle_is_zero(m, h)) { m.set_return(SEC_E_OK); return; }
            m.set_return(schannel::free_context(read_handle_id(m, h)) ? SEC_E_OK
                                                                      : SEC_E_INVALID_HANDLE);
        }, 4});
    // Zeroed, unknown and already freed CredHandles are SEC_E_INVALID_HANDLE.
    exports.entries.push_back(ExportEntry{"FreeCredentialsHandle", 0,
        [](Marshaler& m) {
            const GuestAddr h = m.arg_ptr(0);
            const u32 id = read_handle_id(m, h);
            m.set_return(id != 0 && schannel::free_credential(id) ? SEC_E_OK
                                                                  : SEC_E_INVALID_HANDLE);
        }, 4});

    exports.entries.push_back(ExportEntry{"FreeContextBuffer", 0,
        [pmgr](Marshaler& m) {
            GuestAddr p = m.arg_ptr(0);
            HeapManager* heap = pmgr->heap_manager();
            if (p && heap) (void)heap->heap_free(heap->get_process_heap(), 0, p);
            m.set_return(SEC_E_OK);
        }, 4});

    // SECURITY_STATUS CompleteAuthToken(PCtxtHandle, PSecBufferDesc)
    // https://learn.microsoft.com/en-us/windows/win32/api/sspi/nf-sspi-completeauthtoken
    // Schannel does not revise tokens after transport updates.
    exports.entries.push_back(ExportEntry{"CompleteAuthToken", 0,
        [](Marshaler& m) {
            const u32 id = read_handle_id(m, m.arg_ptr(0));
            (void)m.arg_ptr(1);
            if (id == 0 || schannel::get_context(id) == nullptr) {
                m.set_return(SEC_E_INVALID_HANDLE);
                return;
            }
            m.set_return(SEC_E_UNSUPPORTED_FUNCTION);
        }, 8});

    exports.entries.push_back(ExportEntry{"ApplyControlToken", 0,
        [](Marshaler& m) {
            u32 ctx_id = read_handle_id(m, m.arg_ptr(0));
            GuestAddr pInput = m.arg_ptr(1);
            schannel::TlsSession* sess = schannel::get_context(ctx_id);
            if (!sess) { m.set_return(SEC_E_INVALID_HANDLE); return; }

            SecBufferDescView d = read_desc(m, pInput);
            SecBufferView tok{};
            if (!find_buffer(m, d, SECBUFFER_TOKEN, tok) || tok.cb < 4 || tok.pv == 0) {
                m.set_return(SEC_E_INVALID_TOKEN);
                return;
            }
            u32 token = m.read_u32(tok.pv).value_or(0);
            if (token != SCHANNEL_SHUTDOWN) {
                VITREO_TRACE("[secur32] ApplyControlToken unsupported token=%u\n", token);
                m.set_return(SEC_E_UNSUPPORTED_FUNCTION);
                return;
            }
            sess->request_shutdown();
            VITREO_TRACE("[secur32] ApplyControlToken ctx=0x%x SCHANNEL_SHUTDOWN -> OK\n",
                         ctx_id);
            m.set_return(SEC_E_OK);
        }, 8});

    // Server-side Schannel is not implemented: inbound credentials are
    // accepted by AcquireCredentialsHandle (as on Windows) and fail here.
    exports.entries.push_back(ExportEntry{"AcceptSecurityContext", 0,
        [](Marshaler& m) {
            sspi_unimplemented_once("secur32!AcceptSecurityContext: server-side Schannel");
            unsupported(m, 9);
        }, 36});
    exports.entries.push_back(ExportEntry{"SetContextAttributesA", 0,
        [](Marshaler& m) { unsupported(m, 4); }, 16});
    exports.entries.push_back(ExportEntry{"SetContextAttributesW", 0,
        [](Marshaler& m) { unsupported(m, 4); }, 16});

    exports.entries.push_back(ExportEntry{"InitSecurityInterfaceA", 0,
        [pmgr](Marshaler& m) { init_security_interface<false>(m, *pmgr); }, 0});
    exports.entries.push_back(ExportEntry{"InitSecurityInterfaceW", 0,
        [pmgr](Marshaler& m) { init_security_interface<true>(m, *pmgr); }, 0});

    exports.entries.push_back(ExportEntry{"QuerySecurityPackageInfoA", 0,
        [pmgr](Marshaler& m) { query_security_package_info<false>(m, *pmgr); }, 8});
    exports.entries.push_back(ExportEntry{"QuerySecurityPackageInfoW", 0,
        [pmgr](Marshaler& m) { query_security_package_info<true>(m, *pmgr); }, 8});
    exports.entries.push_back(ExportEntry{"EnumerateSecurityPackagesA", 0,
        [pmgr](Marshaler& m) { enumerate_security_packages<false>(m, *pmgr); }, 8});
    exports.entries.push_back(ExportEntry{"EnumerateSecurityPackagesW", 0,
        [pmgr](Marshaler& m) { enumerate_security_packages<true>(m, *pmgr); }, 8});

    // GetUserNameExA/W and the Lsa* exports forward to sspicli.dll on Windows.
    append_sspicli_forwarded_exports(exports);
    append_secur32_computer_object_name_exports(exports);

    return exports;
}

} // namespace vitreo::winapi
