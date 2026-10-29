// SPDX-License-Identifier: LGPL-2.1-only
/// @file crypt32.cpp
/// @brief CRYPT32.dll — "always-trust" certificate verification
///
/// Implements the Crypt32 surface a statically linked OpenSSL verify
/// callback needs.  Guest-side
/// temporary structures use a dedicated scratch region. Objects with explicit
/// Windows lifetimes use guest heap storage.

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

/// @file certaddcertificatecontexttostore.cpp
/// @brief CertAddCertificateContextToStore
///
/// Closed cut from crypt32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// CertAddCertificateContextToStore / CertAddEncodedCertificateToStore
// =============================================================================
//
// Contracts:
//  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certaddcertificatecontexttostore
//  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certaddencodedcertificatetostore
// Measured dispositions (tools/oracle_probe/crypt_misc_probe.c, "disp_*"):
//  NEW with a match -> CRYPT_E_EXISTS; USE_EXISTING -> the existing context;
//  REPLACE_EXISTING -> new element without the old properties (last error
//  CRYPT_E_NOT_FOUND); ALWAYS -> a second element; REPLACE_EXISTING_INHERIT_
//  PROPERTIES -> the existing context, properties kept; NEWER / NEWER_INHERIT
//  compare NotBefore with the certificate of the same issuer and serial and
//  fail with CRYPT_E_EXISTS unless the new one is newer; any other value is
//  E_INVALIDARG. A memory store lists the newest element first.

namespace {
constexpr u32 kAddNew = 1;
constexpr u32 kAddUseExisting = 2;
constexpr u32 kAddReplaceExisting = 3;
constexpr u32 kAddAlways = 4;
constexpr u32 kAddReplaceInherit = 5;
constexpr u32 kAddNewer = 6;
constexpr u32 kAddNewerInherit = 7;
constexpr u32 kCryptEExists = 0x80092005u;
constexpr u32 kCryptENotFound = 0x80092004u;
} // namespace

u32 Crypt32Manager::store_add_certificate(Marshaler& m, u32 store_handle,
                                          const std::vector<u8>& der, u32 disposition,
                                          std::shared_ptr<CertProperties> source_props,
                                          GuestAddr out_ptr) {
    if (out_ptr != 0) (void)m.write_ptr(out_ptr, 0);
    if (disposition < kAddNew || disposition > kAddNewerInherit) {
        set_last_error(crypt32::err::kEInvalidArg);
        return 0;
    }
    auto parsed = x509::parse_certificate(der);
    if (!parsed) {
        set_last_error(x509::certificate_decode_error(der));
        return 0;
    }

    enum class Action : u8 { Add, UseExisting, Replace, ReplaceInherit };
    Action action = Action::Add;
    u32 target = store_handle;
    std::string record;
    usize index = 0;
    u32 success_error = 0;
    GuestAddr reuse = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Collection stores add to the first sibling opened with
        // CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG (Learn, CertAddStoreToCollection).
        if (auto collection = m_collection_stores.find(store_handle);
            collection != m_collection_stores.end()) {
            target = 0;
            for (const auto& sibling : collection->second) {
                if ((sibling.update_flags & 0x1u) != 0) { target = sibling.store_handle; break; }
            }
            if (target == 0) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                return 0;
            }
        }
        auto system_name = m_system_store_names.find(target);
        auto memory = m_store_certs.find(target);
        if (system_name == m_system_store_names.end() && memory == m_store_certs.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return 0;
        }
        const bool system = system_name != m_system_store_names.end();
        if (system && (m_system_store_read_only[target] || !load_system_store_locked(system_name->second))) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return 0;
        }
        // The element list, as DER.
        std::vector<std::vector<u8>> elements;
        if (system) {
            for (const auto& c : m_system_store_certs[system_name->second]) elements.push_back(c.der_data);
        } else {
            elements = memory->second;
        }
        std::optional<usize> match;
        for (usize i = 0; i < elements.size() && !match; ++i) {
            if (disposition == kAddNewer || disposition == kAddNewerInherit) {
                auto existing = x509::parse_certificate(elements[i]);
                if (existing && existing->issuer_der == parsed->issuer_der &&
                    existing->serial == parsed->serial) {
                    match = i;
                }
            } else if (elements[i] == der) {
                match = i;
            }
        }
        if (match) {
            switch (disposition) {
            case kAddNew:
                set_last_error(kCryptEExists);
                return 0;
            case kAddUseExisting: action = Action::UseExisting; break;
            case kAddReplaceExisting: action = Action::Replace; success_error = kCryptENotFound; break;
            case kAddAlways: action = Action::Add; break;
            case kAddReplaceInherit: action = Action::ReplaceInherit; break;
            case kAddNewer:
            case kAddNewerInherit: {
                auto existing = x509::parse_certificate(elements[*match]);
                if (!existing || parsed->not_before_filetime <= existing->not_before_filetime) {
                    set_last_error(kCryptEExists);
                    return 0;
                }
                action = disposition == kAddNewer ? Action::Replace : Action::ReplaceInherit;
                break;
            }
            }
        } else if (disposition != kAddAlways) {
            // Measured: every disposition but ALWAYS looks the certificate up
            // first, and a success after a miss leaves CRYPT_E_NOT_FOUND
            // (authenticode_chain_probe "disposition" rows).
            success_error = kCryptENotFound;
        }

        const std::vector<u8> old_der = match ? elements[*match] : std::vector<u8>{};
        std::shared_ptr<CertProperties> old_props =
            match ? element_properties_locked(target, old_der) : nullptr;
        if (action == Action::UseExisting || (action == Action::ReplaceInherit && old_der == der)) {
            index = *match;
            if (system) record = m_system_store_certs[system_name->second][index].record_name;
            // Hand back a live context of that element when the caller has one.
            for (auto& [address, entry] : m_cert_contexts) {
                if (entry.store_handle == target && entry.der_data == der) {
                    ++entry.reference_count;
                    reuse = address;
                    break;
                }
            }
        } else if (action == Action::Replace || action == Action::ReplaceInherit) {
            index = *match;
            if (system) {
                auto& stored = m_system_store_certs[system_name->second][index];
                if (!overwrite_system_record_locked(system_name->second, stored.record_name, der)) {
                    set_last_error(error_codes::ERROR_ACCESS_DENIED);
                    return 0;
                }
                stored.der_data = der;
                record = stored.record_name;
            } else {
                memory->second[index] = der;
            }
            auto& slot = m_element_properties[target][der];
            slot = std::make_shared<CertProperties>(
                action == Action::ReplaceInherit && old_props ? *old_props
                : source_props ? *source_props : CertProperties{});
        } else {  // Add
            if (system) {
                if (!append_system_record_locked(system_name->second, der, record)) {
                    set_last_error(error_codes::ERROR_ACCESS_DENIED);
                    return 0;
                }
                m_system_store_certs[system_name->second].push_back(StoredCertificate{der, record});
                index = m_system_store_certs[system_name->second].size() - 1;
            } else {
                memory->second.insert(memory->second.begin(), der);
                index = 0;
            }
            auto& slot = m_element_properties[target][der];
            if (!slot || action == Action::Add) {
                slot = std::make_shared<CertProperties>(source_props ? *source_props : CertProperties{});
            }
        }
    }

    if (out_ptr != 0) {
        GuestAddr context = reuse;
        if (context == 0) {
            context = create_cert_context_from_der(m, der, target, 0x00000001u, record, index);
            if (context == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
        }
        (void)m.write_ptr(out_ptr, context);
    }
    set_last_error(success_error);
    return 1;
}

// BOOL CertAddCertificateContextToStore(HCERTSTORE hCertStore,
//     PCCERT_CONTEXT pCertContext, DWORD dwAddDisposition, PCCERT_CONTEXT* ppStoreContext)
u32 Crypt32Manager::cert_add_certificate_context_to_store(Marshaler& m) {
    const u32 store_handle = static_cast<u32>(m.arg(0));
    const GuestAddr source = m.arg_ptr(1);
    const u32 disposition = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    std::vector<u8> der;
    std::shared_ptr<CertProperties> props;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto entry = m_cert_contexts.find(source);
        if (entry != m_cert_contexts.end()) {
            der = entry->second.der_data;
            props = entry->second.props;
        }
    }
    if (der.empty()) {
        const u32 ps = m.cpu_is_64bit() ? 8u : 4u;
        const GuestAddr bytes = static_cast<GuestAddr>(m.read_ptr(source + ps).value_or(0));
        const u32 size = m.read_u32(source + 2 * ps).value_or(0);
        der.resize(size);
        if (source == 0 || size == 0 || size > 0x100000 ||
            m.memory().read_bytes(bytes, reinterpret_cast<Byte*>(der.data()), size).is_err()) {
            set_last_error(crypt32::err::kEInvalidArg);
            return 0;
        }
    }
    return store_add_certificate(m, store_handle, der, disposition, std::move(props), out);
}

// BOOL CertAddEncodedCertificateToStore(HCERTSTORE hCertStore,
//     DWORD dwCertEncodingType, const BYTE* pbCertEncoded, DWORD cbCertEncoded,
//     DWORD dwAddDisposition, PCCERT_CONTEXT* ppCertContext)
u32 Crypt32Manager::cert_add_encoded_certificate_to_store(Marshaler& m) {
    const u32 store_handle = static_cast<u32>(m.arg(0));
    const u32 encoding = m.arg32(1);
    const GuestAddr encoded = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    const u32 disposition = m.arg32(4);
    const GuestAddr out = m.arg_ptr(5);
    if (out != 0) (void)m.write_ptr(out, 0);
    const u32 cert_encoding = encoding & 0xFFFFu;
    if (cert_encoding == 0) {
        set_last_error(crypt32::err::kEInvalidArg);
        return 0;
    }
    if (cert_encoding != 0x1u) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }
    std::vector<u8> der(size);
    if (size != 0 && m.memory().read_bytes(encoded, reinterpret_cast<Byte*>(der.data()), size).is_err()) {
        crypt32::raise_access_violation(m, encoded, false);
        return 0;
    }
    if (const u32 error = x509::certificate_decode_error(der)) {
        set_last_error(error);
        return 0;
    }
    return store_add_certificate(m, store_handle, der, disposition, nullptr, out);
}

} // namespace vitreo::winapi
