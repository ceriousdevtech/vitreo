// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypt32_registration.cpp
/// @brief Export table for crypt32's certificate-store and encoding surface.
///
/// Composition only: maps export names to handlers and records each one's
/// stdcall byte count.
///
/// The handlers it names live in a nested `crypt32` namespace — see the internal
/// header — because their spellings are shared vocabulary across the DLLs in
/// this family.
///
/// Split out of crypt32.cpp, which was 3,014 lines.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export. Plan
/// section 4.1 exempts these because length here tracks how many exports
/// Vitreo implements, not how much logic one file carries.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "winapi/system/crypt32/pfx_is_pfx_blob.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "ipc/ipc_gate.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonRandom.h>

namespace vitreo::winapi {

using namespace crypt32;

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_crypt32_thunks(Crypt32Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(18);

    exports.entries.push_back(ExportEntry{
        "CryptProtectData", 1,
        [&mgr](Marshaler& m) { impl_CryptProtectData(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "CryptUnprotectData", 2,
        [&mgr](Marshaler& m) { impl_CryptUnprotectData(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        // Learn (CertOpenSystemStoreA): opens a system store by name ("MY",
        // "ROOT", "CA"). It is CertOpenStore with the system-store provider,
        // so it routes to the same manager rather than being a separate store
        // implementation; the first parameter is a legacy HCRYPTPROV that
        // Windows itself ignores.
        "CertOpenSystemStoreA", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.cert_open_system_store_a(m)); },
        8
    });

    exports.entries.push_back(ExportEntry{
        "CertOpenStore", 3,
        [&mgr](Marshaler& m) {
            // NULL store provider -> ERROR_FILE_NOT_FOUND on real Windows.
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                m.set_return(0);
                return;
            }
            impl_CertOpenStore(m, mgr);
        },
        20
    });
    exports.entries.push_back(ExportEntry{
        "CertCloseStore", 4,
        [&mgr](Marshaler& m) { impl_CertCloseStore(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "CertFindCertificateInStore", 5,
        [&mgr](Marshaler& m) { impl_CertFindCertificateInStore(m, mgr); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "CertFreeCertificateContext", 6,
        [&mgr](Marshaler& m) { impl_CertFreeCertificateContext(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CertDuplicateCertificateContext", 0,
        [&mgr](Marshaler& m) { impl_CertDuplicateCertificateContext(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CertGetCertificateChain", 7,
        [&mgr](Marshaler& m) { impl_CertGetCertificateChain(m, mgr); },
        32
    });
    exports.entries.push_back(ExportEntry{
        "CertFreeCertificateChain", 8,
        [&mgr](Marshaler& m) { impl_CertFreeCertificateChain(m, mgr); },
        4
    });
    // PCCERT_CHAIN_CONTEXT CertDuplicateCertificateChain(PCCERT_CHAIN_CONTEXT)
    exports.entries.push_back(ExportEntry{
        "CertDuplicateCertificateChain", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.cert_duplicate_certificate_chain(m)); },
        4
    });
    // HCERTCHAINENGINE create/free — 2 args / 1 arg stdcall.
    exports.entries.push_back(ExportEntry{
        "CertCreateCertificateChainEngine", 0,
        [&mgr](Marshaler& m) { impl_CertCreateCertificateChainEngine(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "CertFreeCertificateChainEngine", 0,
        [&mgr](Marshaler& m) { impl_CertFreeCertificateChainEngine(m, mgr); },
        4
    });
    // 6 args × 4 = 24 (Microsoft Learn wincrypt!CryptInstallOIDFunctionAddress)
    exports.entries.push_back(ExportEntry{
        "CryptInstallOIDFunctionAddress", 0,
        [&mgr](Marshaler& m) { impl_CryptInstallOIDFunctionAddress(m, mgr); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "CertVerifyCertificateChainPolicy", 9,
        [&mgr](Marshaler& m) { impl_CertVerifyCertificateChainPolicy(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "CryptStringToBinaryA", 10,
        [&mgr](Marshaler& m) { impl_CryptStringToBinaryA(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "CryptStringToBinaryW", 0,
        [&mgr](Marshaler& m) { impl_CryptStringToBinaryW(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "CryptBinaryToStringA", 11,
        [&mgr](Marshaler& m) { impl_CryptBinaryToStringA(m, mgr); },
        20
    });
    // 8 args × 4 = 32 (Microsoft Learn wincrypt!CryptDecodeObjectEx)
    exports.entries.push_back(ExportEntry{
        "CryptDecodeObjectEx", 12,
        [&mgr](Marshaler& m) { impl_CryptDecodeObjectEx(m, mgr); },
        32
    });
    // PCERT_EXTENSION CertFindExtension(LPCSTR, DWORD, CERT_EXTENSION[])
    exports.entries.push_back(ExportEntry{
        "CertFindExtension", 0,
        [&mgr](Marshaler& m) { impl_CertFindExtension(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CryptEncodeObjectEx", 13,
        [&mgr](Marshaler& m) { impl_CryptEncodeObjectEx(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "PFXImportCertStore", 14,
        [&mgr](Marshaler& m) { impl_PFXImportCertStore(m, mgr); },
        12
    });
    // BOOL PFXIsPFXBlob(CRYPT_DATA_BLOB* pPFX)
    exports.entries.push_back(ExportEntry{
        "PFXIsPFXBlob", 0,
        [](Marshaler& m) { impl_PFXIsPFXBlob(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CertAddCertificateContextToStore", 15,
        [&mgr](Marshaler& m) { impl_CertAddCertificateContextToStore(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "CertCreateCertificateContext", 16,
        [&mgr](Marshaler& m) { impl_CertCreateCertificateContext(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CertOpenSystemStoreW", 17,
        [&mgr](Marshaler& m) { impl_CertOpenSystemStoreW(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "CertEnumCertificatesInStore", 18,
        [&mgr](Marshaler& m) { impl_CertEnumCertificatesInStore(m, mgr); },
        8
    });
    // PCCERT_CONTEXT CertGetIssuerCertificateFromStore(HCERTSTORE,
    //   PCCERT_CONTEXT pSubject, PCCERT_CONTEXT pPrevIssuer, DWORD* pdwFlags)
    exports.entries.push_back(ExportEntry{
        "CertGetIssuerCertificateFromStore", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.cert_get_issuer_certificate_from_store(m));
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "CertComparePublicKeyInfo", 0,
        [](Marshaler& m) { impl_CertComparePublicKeyInfo(m); },
        12
    });

    // HCRYPTOIDFUNCSET CryptInitOIDFunctionSet(LPCSTR pszFuncName, DWORD)
    exports.entries.push_back(ExportEntry{
        "CryptInitOIDFunctionSet", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.crypt_init_oid_function_set(m)); },
        8
    });

    // BOOL CryptGetOIDFunctionAddress(HCRYPTOIDFUNCSET, DWORD, LPCSTR, DWORD,
    //                                 void**, HCRYPTOIDFUNCADDR*)
    exports.entries.push_back(ExportEntry{
        "CryptGetOIDFunctionAddress", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.crypt_get_oid_function_address(m));
        },
        24
    });

    exports.entries.push_back(ExportEntry{
        "CertDeleteCertificateFromStore", 19,
        [&mgr](Marshaler& m) { impl_CertDeleteCertificateFromStore(m, mgr); },
        4
    });

    // -------------------------------------------------------------------------
    // Default-fail stubs — apps that exercise these paths will get a clear
    // ERROR_NOT_SUPPORTED rather than a silent NULL/garbage.
    // -------------------------------------------------------------------------

    // BOOL CertSetCertificateContextProperty(
    //     PCCERT_CONTEXT, DWORD dwPropId, DWORD dwFlags, const void* pvData)
    exports.entries.push_back(ExportEntry{
        "CertSetCertificateContextProperty", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.cert_set_certificate_context_property(m));
        },
        16
    });
    // BOOL CertSetEnhancedKeyUsage(PCCERT_CONTEXT, PCERT_ENHKEY_USAGE)
    exports.entries.push_back(ExportEntry{
        "CertSetEnhancedKeyUsage", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.cert_set_enhanced_key_usage(m));
        },
        8
    });

    // BOOL CryptHashCertificate2(LPCWSTR pwszCNGHashAlgid, DWORD dwFlags,
    //     void* pvReserved, const BYTE* pbEncoded, DWORD cbEncoded,
    //     BYTE* pbComputedHash, DWORD* pcbComputedHash) — 7 args, 28 bytes.
    exports.entries.push_back(ExportEntry{
        "CryptHashCertificate2", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.crypt_hash_certificate2(m)); },
        28
    });

    // BOOL CertStrToNameA(DWORD dwCertEncodingType, LPCSTR pszX500,
    //     DWORD dwStrType, void* pvReserved, BYTE* pbEncoded,
    //     DWORD* pcbEncoded, LPCSTR* ppszError) — 7 args, 28 bytes.
    exports.entries.push_back(ExportEntry{
        "CertStrToNameA", 0,
        [](Marshaler& m) {
            report_unimplemented("crypt32!CertStrToNameA", "X.500 string encoding");
            const GuestAddr error_out = m.arg_ptr(6);
            if (error_out != 0) (void)m.write_ptr(error_out, 0);
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            m.set_return(0);
        },
        28
    });

    // PCCERT_CONTEXT CertCreateSelfSignCertificate(
    //     HCRYPTPROV_OR_NCRYPT_KEY_HANDLE, PCERT_NAME_BLOB, DWORD,
    //     PCRYPT_KEY_PROV_INFO, PCRYPT_ALGORITHM_IDENTIFIER, PSYSTEMTIME,
    //     PSYSTEMTIME, PCERT_EXTENSIONS) — 8 args, 32 bytes. Needs key
    //     generation and signing through a CSP / CNG key.
    exports.entries.push_back(ExportEntry{
        "CertCreateSelfSignCertificate", 0,
        [](Marshaler& m) {
            report_unimplemented("crypt32!CertCreateSelfSignCertificate", "key generation and signing");
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            m.set_return(0);
        },
        32
    });

    // ------------------------------------------------------------------------
    // Wave 5: crypt32 + wintrust + cat admin stubs
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "CertAddEncodedCertificateToStore", 0,
        [&mgr](Marshaler& m) {
            impl_CertAddEncodedCertificateToStore(m, mgr);
        }, 24 });
    exports.entries.push_back(ExportEntry{ "CertAddStoreToCollection", 0,
        [&mgr](Marshaler& m) {
            impl_CertAddStoreToCollection(m, mgr);
        }, 16 });
    exports.entries.push_back(ExportEntry{ "CertCompareCertificateName", 0,
        impl_CertCompareCertificateName, 12 });
    exports.entries.push_back(ExportEntry{ "CertControlStore", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.cert_control_store(m)); }, 16 });
    exports.entries.push_back(ExportEntry{ "CertFindChainInStore", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 6; ++i) (void)m.arg(i);
            set_last_error(0x80092004u);
            m.set_return(0);
        }, 24 });
    exports.entries.push_back(ExportEntry{ "CertGetCertificateContextProperty", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.cert_get_certificate_context_property(m));
        }, 16 });
    exports.entries.push_back(ExportEntry{ "CertGetEnhancedKeyUsage", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.cert_get_enhanced_key_usage(m));
        }, 16 });
    exports.entries.push_back(ExportEntry{ "CertGetIntendedKeyUsage", 0,
        impl_CertGetIntendedKeyUsage, 16 });
    exports.entries.push_back(ExportEntry{ "CertGetNameStringA", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.cert_get_name_string(m, false));
        }, 24 });
    exports.entries.push_back(ExportEntry{ "CertGetNameStringW", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.cert_get_name_string(m, true));
        }, 24 });
    exports.entries.push_back(ExportEntry{ "CertNameToStrA", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.cert_name_to_str_a(m));
        }, 20 });
    exports.entries.push_back(ExportEntry{ "CertNameToStrW", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.cert_name_to_str_w(m));
        }, 20 });
    // VOID CryptMemFree(void *pv) — Microsoft Learn: frees CryptMemAlloc /
    // CRYPT_DECODE_ALLOC_FLAG buffers (LocalAlloc family). Missing-export stubs
    // returned 0xFFFFFFFF and left callers with a live leak/UAF risk.
    exports.entries.push_back(ExportEntry{ "CryptMemFree", 0,
        [&mgr](Marshaler& m){
            const GuestAddr ptr = m.arg_ptr(0);
            if (ptr != 0) mgr.free_guest(ptr);
        }, 4 });
    // LONG CertVerifyTimeValidity(LPFILETIME pTimeToVerify, PCERT_INFO pCertInfo)
    // Microsoft Learn (wincrypt!CertVerifyTimeValidity): -1 before NotBefore,
    // +1 after NotAfter, 0 if the comparison time is inside the validity window.
    exports.entries.push_back(ExportEntry{ "CertVerifyTimeValidity", 0,
        [](Marshaler& m){
            const GuestAddr time_ptr = m.arg_ptr(0);
            const GuestAddr info = m.arg_ptr(1);
            if (info == 0) {
                m.set_return(1);
                return;
            }
            const bool is64 = m.cpu_is_64bit();
            const u32 ptr_size = is64 ? 8u : 4u;
            const u32 blob_size = 4u + (is64 ? 4u : 0u) + ptr_size;
            const u32 serial_off = is64 ? 8u : 4u;
            const u32 issuer_off =
                serial_off + blob_size + ptr_size + blob_size;
            const u32 not_before_off = issuer_off + blob_size;
            const u32 not_after_off = not_before_off + 8u;
            auto read_filetime = [&](GuestAddr addr) -> u64 {
                const u32 lo = m.read_u32(addr).value_or(0);
                const u32 hi = m.read_u32(addr + 4).value_or(0);
                return (static_cast<u64>(hi) << 32) | lo;
            };
            const u64 not_before = read_filetime(info + not_before_off);
            const u64 not_after = read_filetime(info + not_after_off);
            u64 now = 0;
            if (time_ptr != 0) {
                now = read_filetime(time_ptr);
            } else {
                now = (static_cast<u64>(std::time(nullptr)) + 11644473600ULL) *
                      10000000ULL;
            }
            i32 result = 0;
            if (now < not_before) result = -1;
            else if (now > not_after) result = 1;
            m.set_return(static_cast<u32>(result));
        }, 8 });
    // Vitreo holds no private keys (no CSP / CNG key storage behind
    // CERT_KEY_PROV_INFO_PROP_ID), so every certificate is key-less:
    // CRYPT_E_NO_KEY_PROPERTY with the handle and outputs cleared
    // (crypt_misc_probe "acquire" rows).
    exports.entries.push_back(ExportEntry{ "CryptAcquireCertificatePrivateKey", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_acquire_certificate_private_key(m)); }, 24 });
    exports.entries.push_back(ExportEntry{ "CryptMsgClose", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_msg_close(m)); }, 4 });
    exports.entries.push_back(ExportEntry{ "CryptMsgGetParam", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_msg_get_param(m)); }, 20 });
    // BOOL CryptMsgGetAndVerifySigner(HCRYPTMSG, DWORD, HCERTSTORE*, DWORD,
    //     PCCERT_CONTEXT*, DWORD*)
    exports.entries.push_back(ExportEntry{ "CryptMsgGetAndVerifySigner", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_msg_get_and_verify_signer(m)); }, 24 });
    // BOOL CryptHashPublicKeyInfo(HCRYPTPROV_LEGACY, ALG_ID, DWORD, DWORD,
    //     PCERT_PUBLIC_KEY_INFO, BYTE*, DWORD*)
    exports.entries.push_back(ExportEntry{ "CryptHashPublicKeyInfo", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_hash_public_key_info(m)); }, 28 });
    // BOOL CryptDecodeObject(DWORD, LPCSTR, const BYTE*, DWORD, DWORD, void*, DWORD*)
    exports.entries.push_back(ExportEntry{ "CryptDecodeObject", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_decode_object(m)); }, 28 });
    exports.entries.push_back(ExportEntry{ "CryptProtectMemory", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_protect_memory(m, true)); }, 12 });
    exports.entries.push_back(ExportEntry{ "CryptUnprotectMemory", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_protect_memory(m, false)); }, 12 });
    exports.entries.push_back(ExportEntry{ "CryptQueryObject", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.crypt_query_object(m)); }, 44 });
    exports.entries.push_back(ExportEntry{ "CryptVerifyCertificateSignatureEx", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.crypt_verify_certificate_signature_ex(m));
        }, 32 });
    exports.entries.push_back(ExportEntry{ "WinVerifyTrust", 0,
        [&mgr](Marshaler& m){ m.set_return(mgr.win_verify_trust(m)); }, 12 });
    exports.entries.push_back(ExportEntry{ "WTHelperGetProvCertFromChain", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.wt_helper_get_prov_cert_from_chain(m));
        }, 8 });
    exports.entries.push_back(ExportEntry{ "WTHelperGetProvSignerFromChain", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.wt_helper_get_prov_signer_from_chain(m));
        }, 16 });
    exports.entries.push_back(ExportEntry{ "WTHelperProvDataFromStateData", 0,
        [&mgr](Marshaler& m){
            m.set_return(mgr.wt_helper_prov_data_from_state_data(m));
        }, 4 });
    exports.entries.push_back(ExportEntry{ "CryptCATAdminAcquireContext", 0,
        [](Marshaler& m){
            GuestAddr cat = m.arg_ptr(0);
            (void)m.arg(1); (void)m.arg(2);
            if (cat) (void)m.write_u32(cat, 0x434C0001u);
            m.set_return(1);
        }, 12 });
    exports.entries.push_back(ExportEntry{ "CryptCATAdminCalcHashFromFileHandle", 0,
        [](Marshaler& m){
            (void)m.arg(0);
            GuestAddr sz = m.arg_ptr(1);
            GuestAddr hash = m.arg_ptr(2);
            (void)m.arg(3);
            if (sz) {
                u32 cap = m.read_u32(sz).value_or(0);
                (void)m.write_u32(sz, 20);
                if (cap >= 20 && hash) {
                    for (u32 i = 0; i < 20; ++i) (void)m.write_u8(hash + i, 0);
                    m.set_return(1);
                    return;
                }
            }
            set_last_error(122u);
            m.set_return(0);
        }, 16 });
    exports.entries.push_back(ExportEntry{ "CryptCATAdminEnumCatalogFromHash", 0,
        [](Marshaler& m){
            for (u32 i = 0; i < 5; ++i) (void)m.arg(i);
            m.set_return(0);
        }, 20 });
    exports.entries.push_back(ExportEntry{ "CryptCATAdminReleaseCatalogContext", 0,
        [](Marshaler& m){ (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); m.set_return(1); }, 12 });
    exports.entries.push_back(ExportEntry{ "CryptCATAdminReleaseContext", 0,
        [](Marshaler& m){ (void)m.arg(0); (void)m.arg(1); m.set_return(1); }, 8 });
    exports.entries.push_back(ExportEntry{ "CryptCATCatalogInfoFromContext", 0,
        [](Marshaler& m){
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
            set_last_error(1168u);
            m.set_return(0);
        }, 12 });

    return exports;
}

} // namespace vitreo::winapi
