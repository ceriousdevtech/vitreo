// SPDX-License-Identifier: LGPL-2.1-only
/// @file ncrypt.cpp
/// @brief NCrypt.dll — CNG key storage provider.
///
/// Vitreo's "Microsoft Software Key Storage Provider" opens, answers provider
/// property queries, and holds no keys: it cannot create, import or persist
/// any key. Key-creating calls fail with NTE_NOT_SUPPORTED plus one
/// "[vitreo] UNIMPLEMENTED" line; key-handle calls fail with
/// NTE_INVALID_HANDLE because no key handle can exist; NCryptOpenKey reports
/// NTE_BAD_KEYSET because no named key exists.
///
/// Status values are measured on Windows 11 x86/x64 by
/// tools/oracle_probe/sspi_ncrypt_probe.c (see its .expected files).

#include "ncrypt.hpp"
#include "winapi/core/marshaler.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

// winerror.h / ntstatus.h.
constexpr u64 ERROR_SUCCESS_NT       = 0x00000000ULL;
constexpr u64 NTE_BAD_FLAGS          = 0x80090009ULL;
constexpr u64 NTE_BAD_KEYSET         = 0x80090016ULL;
constexpr u64 NTE_INVALID_HANDLE     = 0x80090026ULL;
constexpr u64 NTE_INVALID_PARAMETER  = 0x80090027ULL;
constexpr u64 NTE_BUFFER_TOO_SMALL   = 0x80090028ULL;
constexpr u64 NTE_NOT_SUPPORTED      = 0x80090029ULL;
constexpr u64 STATUS_NOT_FOUND       = 0xC0000225ULL;  // unregistered provider name
// HRESULT_FROM_WIN32(RPC_X_NULL_REF_POINTER): NULL pszAlgId.
constexpr u64 HRESULT_RPC_X_NULL_REF_POINTER = 0x800706F4ULL;

// ncrypt.h flags.
constexpr u32 NCRYPT_MACHINE_KEY_FLAG         = 0x00000020u;
constexpr u32 NCRYPT_SILENT_FLAG              = 0x00000040u;
constexpr u32 NCRYPT_AUTHORITY_KEY_FLAG       = 0x00000100u;
constexpr u32 NCRYPT_IGNORE_DEVICE_STATE_FLAG = 0x00001000u;
constexpr u32 NCRYPT_EXTENDED_ERRORS_FLAG     = 0x10000000u;
constexpr u32 NCRYPT_IMPL_SOFTWARE_FLAG       = 0x00000002u;

constexpr const char* kSoftwareKsp = "Microsoft Software Key Storage Provider";

enum class ObjectKind { Provider };

struct NcryptState {
    std::mutex mtx;
    std::map<u64, ObjectKind> objects;
    u64 next{0x4E430001ull};
};

NcryptState& state() {
    static NcryptState s;
    return s;
}

bool is_provider(u64 handle) {
    auto& st = state();
    std::lock_guard<std::mutex> lock(st.mtx);
    auto it = st.objects.find(handle);
    return it != st.objects.end() && it->second == ObjectKind::Provider;
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (usize i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

void unimplemented_once(const std::string& what) {
    static std::mutex mtx;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(mtx);
    if (seen.insert(what).second)
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what.c_str());
}

// Algorithms the Windows software KSP supports (NCryptIsAlgSupported ==
// ERROR_SUCCESS in sspi_ncrypt_probe.expected, plus the named-curve siblings).
// Hash algorithms are not KSP algorithms on Windows either.
bool windows_ksp_alg(std::string_view alg) {
    static constexpr const char* kAlgs[] = {
        "RSA", "DSA", "DH", "ECDSA", "ECDH", "ECDSA_P256", "ECDSA_P384",
        "ECDSA_P521", "ECDH_P256", "ECDH_P384", "ECDH_P521", "AES", "3DES",
    };
    for (const char* name : kAlgs) {
        if (equals_ignore_case(alg, name)) return true;
    }
    return false;
}

// Key storage providers Windows registers that Vitreo does not have.
bool windows_provider(std::string_view name) {
    static constexpr const char* kNames[] = {
        "Microsoft Platform Crypto Provider",
        "Microsoft Smart Card Key Storage Provider",
        "Microsoft Passport Key Storage Provider",
    };
    for (const char* n : kNames) {
        if (equals_ignore_case(name, n)) return true;
    }
    return false;
}

inline void fail(Marshaler& m, u32 nargs, u64 status) {
    for (u32 i = 0; i < nargs; ++i) (void)m.arg(i);
    m.set_return(status);
}

u64 write_bytes(Marshaler& m, GuestAddr out, u32 cb_out, GuestAddr pcb,
                const void* data, u32 size) {
    if (pcb == 0) return NTE_INVALID_PARAMETER;
    if (m.write_u32(pcb, size).is_err()) return NTE_INVALID_PARAMETER;
    if (out == 0) return ERROR_SUCCESS_NT;
    if (cb_out < size) return NTE_BUFFER_TOO_SMALL;
    const auto* bytes = static_cast<const u8*>(data);
    for (u32 i = 0; i < size; ++i) {
        if (m.write_u8(out + i, bytes[i]).is_err()) return NTE_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS_NT;
}

u64 write_wide_property(Marshaler& m, GuestAddr out, u32 cb_out, GuestAddr pcb,
                        std::string_view ascii) {
    std::vector<u8> utf16((ascii.size() + 1) * 2, 0);
    for (usize i = 0; i < ascii.size(); ++i) {
        utf16[i * 2] = static_cast<u8>(ascii[i]);
    }
    return write_bytes(m, out, cb_out, pcb, utf16.data(),
                       static_cast<u32>(utf16.size()));
}

} // namespace

DllExports create_ncrypt_thunks() {
    DllExports exports;
    exports.entries.reserve(13);

    // SECURITY_STATUS NCryptOpenStorageProvider(
    //     NCRYPT_PROV_HANDLE* phProvider, LPCWSTR pszProviderName, DWORD dwFlags)
    exports.entries.push_back(ExportEntry{
        "NCryptOpenStorageProvider", 0,
        [](Marshaler& m) {
            const GuestAddr ph = m.arg_ptr(0);
            const GuestAddr name_ptr = m.arg_ptr(1);
            const u32 flags = static_cast<u32>(m.arg(2));
            if (ph == 0) {
                m.set_return(NTE_INVALID_PARAMETER);
                return;
            }
            if ((flags & ~(NCRYPT_SILENT_FLAG | NCRYPT_IGNORE_DEVICE_STATE_FLAG)) != 0) {
                m.set_return(NTE_BAD_FLAGS);
                return;
            }
            if (name_ptr != 0) {
                const std::string name = m.read_wide_string(name_ptr).value_or("");
                if (!name.empty() && !equals_ignore_case(name, kSoftwareKsp)) {
                    if (windows_provider(name))
                        unimplemented_once("ncrypt!NCryptOpenStorageProvider: " + name);
                    VITREO_TRACE(
                        "[ncrypt] NCryptOpenStorageProvider \"%s\" -> not found\n",
                        name.c_str());
                    m.set_return(STATUS_NOT_FOUND);
                    return;
                }
            }
            u64 handle = 0;
            {
                auto& st = state();
                std::lock_guard<std::mutex> lock(st.mtx);
                handle = st.next++;
                st.objects.emplace(handle, ObjectKind::Provider);
            }
            if (m.write_ptr(ph, handle).is_err()) {
                auto& st = state();
                std::lock_guard<std::mutex> lock(st.mtx);
                st.objects.erase(handle);
                m.set_return(NTE_INVALID_PARAMETER);
                return;
            }
            VITREO_TRACE("[ncrypt] NCryptOpenStorageProvider -> 0x%llx\n",
                         static_cast<unsigned long long>(handle));
            m.set_return(ERROR_SUCCESS_NT);
        },
        12
    });

    // SECURITY_STATUS NCryptOpenKey(NCRYPT_PROV_HANDLE hProvider,
    //     NCRYPT_KEY_HANDLE* phKey, LPCWSTR pszKeyName, DWORD dwLegacyKeySpec,
    //     DWORD dwFlags)
    exports.entries.push_back(ExportEntry{
        "NCryptOpenKey", 0,
        [](Marshaler& m) {
            const u64 provider = m.arg(0);
            const GuestAddr ph = m.arg_ptr(1);
            const GuestAddr name = m.arg_ptr(2);
            (void)m.arg(3);
            const u32 flags = m.arg32(4);
            // *phKey is left untouched on every failure.
            if (!is_provider(provider)) { m.set_return(NTE_INVALID_HANDLE); return; }
            if ((flags & ~(NCRYPT_MACHINE_KEY_FLAG | NCRYPT_SILENT_FLAG |
                           NCRYPT_AUTHORITY_KEY_FLAG | NCRYPT_EXTENDED_ERRORS_FLAG)) != 0) {
                m.set_return(NTE_BAD_FLAGS);
                return;
            }
            if (ph == 0 || name == 0) { m.set_return(NTE_INVALID_PARAMETER); return; }
            // No key is ever persisted by this provider.
            m.set_return(NTE_BAD_KEYSET);
        },
        20
    });

    // SECURITY_STATUS NCryptCreatePersistedKey(NCRYPT_PROV_HANDLE hProvider,
    //     NCRYPT_KEY_HANDLE* phKey, LPCWSTR pszAlgId, LPCWSTR pszKeyName,
    //     DWORD dwLegacyKeySpec, DWORD dwFlags)          -- 24 bytes on x86
    exports.entries.push_back(ExportEntry{
        "NCryptCreatePersistedKey", 0,
        [](Marshaler& m) {
            const u64 provider = m.arg(0);
            (void)m.arg_ptr(1);
            const GuestAddr alg_ptr = m.arg_ptr(2);
            (void)m.arg_ptr(3);
            (void)m.arg32(4);
            (void)m.arg32(5);
            if (!is_provider(provider)) { m.set_return(NTE_INVALID_HANDLE); return; }
            const std::string alg =
                alg_ptr ? m.read_wide_string(alg_ptr).value_or("") : std::string{};
            if (windows_ksp_alg(alg))
                unimplemented_once("ncrypt!NCryptCreatePersistedKey: " + alg + " key");
            VITREO_TRACE("[ncrypt] NCryptCreatePersistedKey(\"%s\") -> NTE_NOT_SUPPORTED\n",
                         alg.c_str());
            m.set_return(NTE_NOT_SUPPORTED);
        },
        24
    });

    exports.entries.push_back(ExportEntry{
        "NCryptFinalizeKey", 0,
        [](Marshaler& m) {
            fail(m, 2, NTE_INVALID_HANDLE);
        },
        8
    });

    exports.entries.push_back(ExportEntry{
        "NCryptDeleteKey", 0,
        [](Marshaler& m) {
            fail(m, 2, NTE_INVALID_HANDLE);
        },
        8
    });

    // SECURITY_STATUS NCryptExportKey(NCRYPT_KEY_HANDLE hKey,
    //     NCRYPT_KEY_HANDLE hExportKey, LPCWSTR pszBlobType,
    //     NCryptBufferDesc* pParameterList, PBYTE pbOutput, DWORD cbOutput,
    //     DWORD* pcbResult, DWORD dwFlags)                -- 32 bytes on x86
    exports.entries.push_back(ExportEntry{
        "NCryptExportKey", 0,
        [](Marshaler& m) {
            fail(m, 8, NTE_INVALID_HANDLE);
        },
        32
    });

    // SECURITY_STATUS NCryptImportKey(NCRYPT_PROV_HANDLE hProvider,
    //     NCRYPT_KEY_HANDLE hImportKey, LPCWSTR pszBlobType,
    //     NCryptBufferDesc* pParameterList, NCRYPT_KEY_HANDLE* phKey,
    //     PBYTE pbData, DWORD cbData, DWORD dwFlags)      -- 32 bytes on x86
    exports.entries.push_back(ExportEntry{
        "NCryptImportKey", 0,
        [](Marshaler& m) {
            const u64 provider = m.arg(0);
            (void)m.arg(1);
            const GuestAddr blob_ptr = m.arg_ptr(2);
            (void)m.arg(3); (void)m.arg(4); (void)m.arg(5);
            (void)m.arg32(6); (void)m.arg32(7);
            if (!is_provider(provider)) { m.set_return(NTE_INVALID_HANDLE); return; }
            const std::string blob =
                blob_ptr ? m.read_wide_string(blob_ptr).value_or("") : std::string{};
            unimplemented_once("ncrypt!NCryptImportKey: " + blob);
            m.set_return(NTE_NOT_SUPPORTED);
        },
        32
    });

    // SECURITY_STATUS NCryptFreeObject(NCRYPT_HANDLE hObject)
    exports.entries.push_back(ExportEntry{
        "NCryptFreeObject", 0,
        [](Marshaler& m) {
            const u64 handle = m.arg(0);
            if (handle == 0) {
                m.set_return(NTE_INVALID_HANDLE);
                return;
            }
            auto& st = state();
            std::lock_guard<std::mutex> lock(st.mtx);
            if (st.objects.erase(handle) == 0) {
                m.set_return(NTE_INVALID_HANDLE);
                return;
            }
            m.set_return(ERROR_SUCCESS_NT);
        },
        4
    });

    // SECURITY_STATUS NCryptSetProperty(NCRYPT_HANDLE hObject,
    //     LPCWSTR pszProperty, PBYTE pbInput, DWORD cbInput, DWORD dwFlags)
    exports.entries.push_back(ExportEntry{
        "NCryptSetProperty", 0,
        [](Marshaler& m) {
            const u64 handle = m.arg(0);
            (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            (void)m.arg32(4);
            if (!is_provider(handle)) { m.set_return(NTE_INVALID_HANDLE); return; }
            m.set_return(NTE_NOT_SUPPORTED);
        },
        20
    });

    // SECURITY_STATUS NCryptIsAlgSupported(
    //     NCRYPT_PROV_HANDLE hProvider, LPCWSTR pszAlgId, DWORD dwFlags)
    exports.entries.push_back(ExportEntry{
        "NCryptIsAlgSupported", 0,
        [](Marshaler& m) {
            const u64 handle = m.arg(0);
            const GuestAddr alg_ptr = m.arg_ptr(1);
            const u32 flags = static_cast<u32>(m.arg(2));
            if (!is_provider(handle)) {
                m.set_return(NTE_INVALID_HANDLE);
                return;
            }
            if (flags != 0 && flags != NCRYPT_SILENT_FLAG) {
                m.set_return(NTE_BAD_FLAGS);
                return;
            }
            if (alg_ptr == 0) {
                m.set_return(HRESULT_RPC_X_NULL_REF_POINTER);
                return;
            }
            const std::string alg = m.read_wide_string(alg_ptr).value_or("");
            // This provider can neither create nor import any key, so no
            // algorithm is supported. Windows' KSP supports the asymmetric
            // and symmetric key algorithms; say so once.
            if (windows_ksp_alg(alg))
                unimplemented_once("ncrypt!NCryptIsAlgSupported: " + alg + " keys");
            m.set_return(NTE_NOT_SUPPORTED);
        },
        12
    });

    // SECURITY_STATUS NCryptGetProperty(
    //     NCRYPT_HANDLE, LPCWSTR pszProperty, PBYTE pbOutput,
    //     DWORD cbOutput, DWORD* pcbResult, DWORD dwFlags)
    exports.entries.push_back(ExportEntry{
        "NCryptGetProperty", 0,
        [](Marshaler& m) {
            const u64 handle = m.arg(0);
            const GuestAddr prop_ptr = m.arg_ptr(1);
            const GuestAddr out = m.arg_ptr(2);
            const u32 cb_out = static_cast<u32>(m.arg(3));
            const GuestAddr pcb = m.arg_ptr(4);
            const u32 flags = m.arg32(5);
            if (flags != 0 && flags != NCRYPT_SILENT_FLAG) {
                m.set_return(NTE_BAD_FLAGS);
                return;
            }
            if (!is_provider(handle)) {
                m.set_return(NTE_INVALID_HANDLE);
                return;
            }
            if (prop_ptr == 0 || pcb == 0) {
                m.set_return(NTE_INVALID_PARAMETER);
                return;
            }
            const std::string prop = m.read_wide_string(prop_ptr).value_or("");
            if (equals_ignore_case(prop, "Name")) {
                m.set_return(write_wide_property(m, out, cb_out, pcb,
                                                 kSoftwareKsp));
                return;
            }
            if (equals_ignore_case(prop, "Impl Type")) {
                // Windows 11 reports 0x22 (software | virtual-isolation
                // capable); this provider has no VBS key isolation.
                const u32 impl = NCRYPT_IMPL_SOFTWARE_FLAG;
                m.set_return(write_bytes(m, out, cb_out, pcb, &impl, 4));
                return;
            }
            VITREO_TRACE("[ncrypt] NCryptGetProperty \"%s\" -> NTE_NOT_SUPPORTED\n",
                         prop.c_str());
            // Windows zeroes *pcbResult for a property it does not have.
            (void)m.write_u32(pcb, 0);
            m.set_return(NTE_NOT_SUPPORTED);
        },
        24
    });

    // SECURITY_STATUS NCryptSignHash(
    //     NCRYPT_KEY_HANDLE, VOID* pPaddingInfo, PBYTE pbHashValue,
    //     DWORD cbHashValue, PBYTE pbSignature, DWORD cbSignature,
    //     DWORD* pcbResult, DWORD dwFlags)
    exports.entries.push_back(ExportEntry{
        "NCryptSignHash", 0,
        [](Marshaler& m) {
            VITREO_TRACE("[ncrypt] NCryptSignHash -> NTE_INVALID_HANDLE\n");
            fail(m, 8, NTE_INVALID_HANDLE);
        },
        32
    });

    return exports;
}

} // namespace vitreo::winapi
