// SPDX-License-Identifier: LGPL-2.1-only
/// @file crypt32.cpp
/// @brief CRYPT32.dll certificate contexts and chain verification.
///
/// Guest-side temporary structures use a dedicated scratch region. Objects with an
/// explicit Windows lifetime, including CERT_CONTEXT, use guest heap storage.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
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

namespace vitreo::winapi {

namespace {

bool crypt32_diag_enabled() {
    static const bool enabled =
        std::getenv("VITREO_CRYPT32_DIAG") != nullptr;
    return enabled;
}

template <typename... Args>
void crypt32_diag(const char* format, Args... args) {
    if (crypt32_diag_enabled()) {
        if constexpr (sizeof...(args) == 0) {
            ipc::ipc_log("%s", format);
        } else {
            ipc::ipc_log(format, args...);
        }
    }
}

} // namespace

// =============================================================================
// Scratch Allocator
// =============================================================================

GuestAddr Crypt32Manager::alloc_guest(u32 size) noexcept {
    if (m_local_memory != nullptr) {
        return m_local_memory->global_alloc(0, size);
    }
    return scratch_alloc(size);
}

void Crypt32Manager::free_guest(GuestAddr addr) noexcept {
    if (addr == 0 || m_local_memory == nullptr) return;
    (void)m_local_memory->global_free(static_cast<u64>(addr));
}

GuestAddr Crypt32Manager::scratch_alloc(u32 size) noexcept {
    size = (size + 3) & ~3u; // 4-byte alignment
    if (m_scratch_offset + size > CRYPT32_SCRATCH_SIZE) {
        return 0;
    }
    GuestAddr addr = CRYPT32_SCRATCH_BASE + m_scratch_offset;
    m_scratch_offset += size;
    return addr;
}





// CertCreateCertificateContext lives in cert_context.cpp.

std::vector<GuestAddr> Crypt32Manager::release_cert_context_locked(
    GuestAddr cert_ctx) {
    auto entry = m_cert_contexts.find(cert_ctx);
    if (entry == m_cert_contexts.end()) return {};
    if (--entry->second.reference_count != 0) return {};
    const u32 store = entry->second.store_handle;
    if (entry->second.owns_store_handle) {
        m_store_certs.erase(store);
        m_element_properties.erase(store);
    }
    auto allocations = std::move(entry->second.owned_allocations);
    m_cert_contexts.erase(entry);
    // The last context of a store whose handle was closed closes it.
    finish_closed_store_locked(store);
    return allocations;
}

u32 Crypt32Manager::cert_free_certificate_context(Marshaler& m) {
    const GuestAddr cert_ctx = static_cast<GuestAddr>(m.arg(0));
    std::vector<GuestAddr> allocations;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        allocations = release_cert_context_locked(cert_ctx);
    }
    for (GuestAddr allocation : allocations) free_guest(allocation);
    VITREO_TRACE("[crypt32] CertFreeCertificateContext(0x%llx) -> TRUE\n",
                 static_cast<unsigned long long>(cert_ctx));
    return 1;
}

// =============================================================================
// CertDuplicateCertificateContext
// =============================================================================
//
// PCCERT_CONTEXT CertDuplicateCertificateContext(
//     PCCERT_CONTEXT pCertContext
// );
//
// On Windows this increments the context's reference count and returns the
// same pointer.

GuestAddr Crypt32Manager::cert_duplicate_certificate_context(Marshaler& m) {
    GuestAddr cert_ctx = static_cast<GuestAddr>(m.arg(0));
    VITREO_TRACE("[crypt32] CertDuplicateCertificateContext(0x%llx)\n",
                 static_cast<unsigned long long>(cert_ctx));
    if (cert_ctx == 0) {
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto entry = m_cert_contexts.find(cert_ctx);
        if (entry != m_cert_contexts.end()) {
            ++entry->second.reference_count;
        }
    }
    return cert_ctx;
}


// CertAddEncodedCertificateToStore lives in certaddcertificatecontexttostore.cpp.

// Chain building, policy and engines live in cert_chain.cpp.

namespace {

/// A `pszOID` argument is either a pointer to a dotted-decimal string or, when
/// its high word is zero, a numeric OID group id (the CRYPT_OID_* constants
/// wincrypt.h passes as an integer cast to LPCSTR). Both forms must key the
/// same registry, so the numeric form is rendered as "#<n>".
std::string decode_oid_argument(Marshaler& m, GuestAddr oid_arg) {
    if (oid_arg == 0) return {};
    if ((oid_arg >> 16) == 0) return "#" + std::to_string(oid_arg);
    auto text = m.read_ansi_string(oid_arg, 256);
    if (text.is_err()) return {};
    return text.value();
}

} // namespace

u32 Crypt32Manager::crypt_install_oid_function_address(Marshaler& m) {
    // BOOL CryptInstallOIDFunctionAddress(
    //   HMODULE, DWORD dwEncodingType, LPCSTR pszFuncName,
    //   DWORD cFuncEntry, CRYPT_OID_FUNC_ENTRY *rgFuncEntry, DWORD dwFlags);
    // Correct x86 stdcall cleanup is 24 bytes — the missing-export stub used
    // 0 and left the caller with a corrupted stack (AV near 0x2a).
    const u32 encoding = static_cast<u32>(m.arg(1));
    const GuestAddr name_ptr = m.arg_ptr(2);
    const u32 count = static_cast<u32>(m.arg(3));
    std::string func_name;
    if (name_ptr != 0) {
        auto nr = m.read_ansi_string(name_ptr, 256);
        if (nr.is_ok()) func_name = nr.value();
    }
    // Record each entry so CryptGetOIDFunctionAddress can hand the guest its
    // own callback back. Dropping them made every later lookup miss, and the
    // caller reads that as "this OID is not supported".
    const GuestAddr entries = m.arg_ptr(4);
    const bool is64 = m.cpu_is_64bit();
    const u32 entry_size = is64 ? 16u : 8u;
    u32 installed = 0;
    if (entries != 0 && !func_name.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr entry = entries + i * entry_size;
            const auto oid_ptr = m.read_ptr(entry);
            const auto func_addr = m.read_ptr(entry + (is64 ? 8u : 4u));
            if (oid_ptr.is_err() || func_addr.is_err()) break;
            const std::string oid = decode_oid_argument(
                m, static_cast<GuestAddr>(oid_ptr.value()));
            if (oid.empty()) continue;
            m_oid_functions[oid_function_key(func_name, encoding, oid)] =
                static_cast<GuestAddr>(func_addr.value());
            ++installed;
        }
    }

    ipc::ipc_log(
        "CryptInstallOIDFunctionAddress enc=0x%x name='%s' entries=%u "
        "installed=%u -> TRUE",
        encoding, func_name.c_str(), count, installed);
    return 1;
}

std::string Crypt32Manager::oid_function_key(const std::string& func_name,
                                             u32 encoding,
                                             const std::string& oid) {
    // The encoding type is a bit field of X509_ASN_ENCODING /
    // PKCS_7_ASN_ENCODING; a lookup must match the same combination that was
    // installed, so it is part of the key rather than a filter.
    return func_name + '\0' + std::to_string(encoding) + '\0' + oid;
}

u32 Crypt32Manager::crypt_init_oid_function_set(Marshaler& m) {
    // HCRYPTOIDFUNCSET CryptInitOIDFunctionSet(LPCSTR pszFuncName,
    //                                          DWORD dwFlags)
    // Learn: returns the (created on first use) function set for that name.
    // The same name must yield the same handle for the lifetime of the process.
    const GuestAddr name_ptr = m.arg_ptr(0);
    std::string func_name;
    if (name_ptr != 0) {
        auto nr = m.read_ansi_string(name_ptr, 256);
        if (nr.is_ok()) func_name = nr.value();
    }
    if (func_name.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto existing = m_oid_func_set_handles.find(func_name);
    if (existing != m_oid_func_set_handles.end()) {
        return existing->second;
    }
    const u32 handle = m_next_oid_func_set++;
    m_oid_func_set_handles[func_name] = handle;
    m_oid_func_set_names[handle] = func_name;
    return handle;
}

u32 Crypt32Manager::crypt_get_oid_function_address(Marshaler& m) {
    // BOOL CryptGetOIDFunctionAddress(HCRYPTOIDFUNCSET hFuncSet,
    //   DWORD dwEncodingType, LPCSTR pszOID, DWORD dwFlags,
    //   void** ppvFuncAddr, HCRYPTOIDFUNCADDR* phFuncAddr)
    // Learn: fails with ERROR_FILE_NOT_FOUND when no function is installed for
    // that OID — the outcome for every OID nothing registered here.
    const u32 func_set = static_cast<u32>(m.arg(0));
    const u32 encoding = static_cast<u32>(m.arg(1));
    const std::string oid = decode_oid_argument(m, m.arg_ptr(2));
    const GuestAddr out_func = m.arg_ptr(4);
    const GuestAddr out_handle = m.arg_ptr(5);

    GuestAddr address = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto name = m_oid_func_set_names.find(func_set);
        if (name == m_oid_func_set_names.end()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        auto found = m_oid_functions.find(
            oid_function_key(name->second, encoding, oid));
        if (found != m_oid_functions.end()) address = found->second;
    }

    if (address == 0) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if (out_func != 0) (void)m.write_ptr(out_func, address);
    // The address handle only has to be releasable by
    // CryptFreeOIDFunctionAddress; the function address itself serves.
    if (out_handle != 0) (void)m.write_ptr(out_handle, address);
    return 1;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void crypt32::impl_CryptProtectData(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_protect_data(m));
}

void crypt32::impl_CryptUnprotectData(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_unprotect_data(m));
}

void crypt32::impl_CertOpenStore(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_open_store(m);
    VITREO_TRACE("[crypt32] impl_CertOpenStore -> 0x%x\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertOpenSystemStoreW(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_open_system_store_w(m);
    VITREO_TRACE("[crypt32] impl_CertOpenSystemStoreW -> 0x%x\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertEnumCertificatesInStore(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_enum_certificates_in_store(m));
}

void crypt32::impl_CertDeleteCertificateFromStore(
    Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_delete_certificate_from_store(m));
}

void crypt32::impl_CertCloseStore(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_close_store(
        static_cast<u32>(m.arg(0)), static_cast<u32>(m.arg(1)));
    VITREO_TRACE("[crypt32] impl_CertCloseStore -> %u\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertAddStoreToCollection(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_add_store_to_collection(m));
}

void crypt32::impl_CertFindCertificateInStore(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_find_certificate_in_query_store(m));
}

void crypt32::impl_CertFreeCertificateContext(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_free_certificate_context(m);
    VITREO_TRACE("[crypt32] impl_CertFreeCertificateContext -> %u\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertDuplicateCertificateContext(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_duplicate_certificate_context(m));
}

void crypt32::impl_CertGetCertificateChain(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_get_certificate_chain(m);
    VITREO_TRACE("[crypt32] impl_CertGetCertificateChain -> %u (return to guest)\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertFreeCertificateChain(Marshaler& m, Crypt32Manager& mgr) {
    mgr.cert_free_certificate_chain(m);
    m.set_return(0);
}

void crypt32::impl_CertVerifyCertificateChainPolicy(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_verify_certificate_chain_policy(m);
    VITREO_TRACE("[crypt32] impl_CertVerifyCertificateChainPolicy -> %u (return to guest)\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertCreateCertificateChainEngine(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_create_certificate_chain_engine(m));
}

void crypt32::impl_CertFreeCertificateChainEngine(Marshaler& m, Crypt32Manager& mgr) {
    mgr.cert_free_certificate_chain_engine(m);
    m.set_return(0);
}

void crypt32::impl_CryptInstallOIDFunctionAddress(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_install_oid_function_address(m));
}

void crypt32::impl_CryptStringToBinaryA(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_string_to_binary_a(m));
}

void crypt32::impl_CryptStringToBinaryW(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_string_to_binary_w(m));
}

void crypt32::impl_CryptBinaryToStringA(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_binary_to_string_a(m));
}

void crypt32::impl_CryptDecodeObjectEx(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_decode_object_ex(m));
}

void crypt32::impl_CertFindExtension(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_find_extension(m));
}

void crypt32::impl_CryptEncodeObjectEx(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.crypt_encode_object_ex());
}

void crypt32::impl_PFXImportCertStore(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.pfx_import_cert_store());
}

void crypt32::impl_CertAddCertificateContextToStore(Marshaler& m, Crypt32Manager& mgr) {
    u32 ret = mgr.cert_add_certificate_context_to_store(m);
    VITREO_TRACE("[crypt32] impl_CertAddCertificateContextToStore -> %u (return to guest)\n", ret);
    m.set_return(ret);
}

void crypt32::impl_CertAddEncodedCertificateToStore(
    Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_add_encoded_certificate_to_store(m));
}

void crypt32::impl_CertCreateCertificateContext(Marshaler& m, Crypt32Manager& mgr) {
    m.set_return(mgr.cert_create_certificate_context(m));
}


} // namespace vitreo::winapi
