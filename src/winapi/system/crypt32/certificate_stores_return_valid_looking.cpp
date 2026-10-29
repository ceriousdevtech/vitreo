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
#include "winapi/system/crypt32/guest_block.hpp"
#include "x509_parse.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonDigest.h>
#endif

/// @file certificate_stores_return_valid_looking.cpp
/// @brief Certificate Stores — return valid-looking handles
///
/// Closed cut from crypt32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

namespace {

constexpr std::string_view kCertificateStoresRoot =
    "C:\\ProgramData\\Vitreo\\CertificateStores";
constexpr std::string_view kCurrentUserStoreLocation = "CurrentUser";
constexpr std::string_view kLocalMachineStoreLocation = "LocalMachine";
constexpr std::string_view kUsersStoreLocation = "Users";
constexpr std::string_view kCurrentServiceStoreLocation = "CurrentService";
constexpr std::string_view kServicesStoreLocation = "Services";
constexpr std::string_view kLocalMachineGroupPolicyStoreLocation =
    "LocalMachineGroupPolicy";
constexpr std::string_view kCurrentUserGroupPolicyStoreLocation =
    "CurrentUserGroupPolicy";
constexpr std::string_view kLocalMachineEnterpriseStoreLocation =
    "LocalMachineEnterprise";

std::string system_store_key(std::string_view location,
                             std::string_view name) {
    return std::string(location) + "\\" + std::string(name);
}

// Map CERT_SYSTEM_STORE_* high-word locations to capsule directory names.
// Unknown locations return empty — callers fail with ERROR_NOT_SUPPORTED.
std::string_view system_store_location_name(u32 location) {
    constexpr u32 CERT_SYSTEM_STORE_CURRENT_USER = 0x00010000u;
    constexpr u32 CERT_SYSTEM_STORE_LOCAL_MACHINE = 0x00020000u;
    constexpr u32 CERT_SYSTEM_STORE_USERS = 0x00030000u;
    constexpr u32 CERT_SYSTEM_STORE_CURRENT_SERVICE = 0x00040000u;
    constexpr u32 CERT_SYSTEM_STORE_SERVICES = 0x00050000u;
    constexpr u32 CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY = 0x00070000u;
    constexpr u32 CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY = 0x00080000u;
    constexpr u32 CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE = 0x00090000u;
    switch (location) {
    case CERT_SYSTEM_STORE_CURRENT_USER:
        return kCurrentUserStoreLocation;
    case CERT_SYSTEM_STORE_LOCAL_MACHINE:
        return kLocalMachineStoreLocation;
    case CERT_SYSTEM_STORE_USERS:
        return kUsersStoreLocation;
    case CERT_SYSTEM_STORE_CURRENT_SERVICE:
        return kCurrentServiceStoreLocation;
    case CERT_SYSTEM_STORE_SERVICES:
        return kServicesStoreLocation;
    case CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY:
        return kLocalMachineGroupPolicyStoreLocation;
    case CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY:
        return kCurrentUserGroupPolicyStoreLocation;
    case CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE:
        return kLocalMachineEnterpriseStoreLocation;
    default:
        return {};
    }
}

std::string encoded_store_name(std::string_view name) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(name.size() * 2);
    for (const char raw_byte : name) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 0x0f]);
    }
    return encoded;
}

std::string system_store_directory(std::string_view key) {
    const usize separator = key.find('\\');
    if (separator == std::string_view::npos || separator + 1 >= key.size()) {
        return {};
    }
    return std::string(kCertificateStoresRoot) + "\\" +
           std::string(key.substr(0, separator)) + "\\" +
           encoded_store_name(key.substr(separator + 1));
}

bool has_certificate_suffix(std::string_view name) {
    return name.size() == 20 && name.ends_with(".cer");
}

u64 record_number(std::string_view name) {
    if (!has_certificate_suffix(name)) return 0;
    u64 value = 0;
    for (usize i = 0; i < 16; ++i) {
        const char c = name[i];
        const u8 digit = c >= '0' && c <= '9'
                             ? static_cast<u8>(c - '0')
                             : c >= 'a' && c <= 'f'
                                   ? static_cast<u8>(c - 'a' + 10)
                                   : c >= 'A' && c <= 'F'
                                         ? static_cast<u8>(c - 'A' + 10)
                                         : 0xff;
        if (digit == 0xff) return 0;
        value = (value << 4) | digit;
    }
    return value;
}

} // namespace

// =============================================================================
// Certificate Stores — return valid-looking handles
// =============================================================================

u32 Crypt32Manager::cert_open_store() {
    std::lock_guard<std::mutex> lock(m_mutex);
    u32 handle = m_next_store_handle++;
    m_store_certs.emplace(handle, std::vector<std::vector<u8>>{});
    VITREO_TRACE("[crypt32] CertOpenStore -> 0x%x\n", handle);
    return handle;
}

void Crypt32Manager::fill_message_store_locked(
    u32 store_handle, const std::vector<authenticode::Certificate>& certificates) {
    auto& elements = m_store_certs[store_handle];
    elements.clear();
    for (auto it = certificates.rbegin(); it != certificates.rend(); ++it) {
        elements.push_back(it->der);
    }
}

u32 Crypt32Manager::cert_open_store(Marshaler& m) {
    constexpr u32 CERT_STORE_PROV_MEMORY = 2;
    constexpr u32 CERT_STORE_PROV_COLLECTION = 11;
    constexpr u32 CERT_STORE_PROV_SYSTEM_A = 9;
    constexpr u32 CERT_STORE_PROV_SYSTEM_W = 10;
    constexpr u32 CERT_STORE_PROV_SYSTEM_REGISTRY_A = 12;
    constexpr u32 CERT_STORE_PROV_SYSTEM_REGISTRY_W = 13;
    constexpr u32 CERT_SYSTEM_STORE_LOCATION_MASK = 0x00ff0000u;
    constexpr u32 CERT_SYSTEM_STORE_RELOCATE_FLAG = 0x80000000u;
    constexpr u32 CERT_STORE_READONLY_FLAG = 0x00008000u;

    const GuestAddr provider_value = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(3));
    const GuestAddr parameter = m.arg_ptr(4);
    bool memory_provider = provider_value == CERT_STORE_PROV_MEMORY;
    bool collection_provider = provider_value == CERT_STORE_PROV_COLLECTION;
    bool system_provider = provider_value == CERT_STORE_PROV_SYSTEM_A ||
                           provider_value == CERT_STORE_PROV_SYSTEM_W;
    bool system_registry_provider =
        provider_value == CERT_STORE_PROV_SYSTEM_REGISTRY_A ||
        provider_value == CERT_STORE_PROV_SYSTEM_REGISTRY_W;
    bool wide_parameter = provider_value != CERT_STORE_PROV_SYSTEM_A &&
                          provider_value != CERT_STORE_PROV_SYSTEM_REGISTRY_A;

    if (provider_value > 0xffffu) {
        auto provider = m.read_ansi_string(provider_value, 64);
        if (provider.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        std::string name = provider.value();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        memory_provider = name == "memory";
        collection_provider = name == "collection";
        system_provider = name == "system";
        // Microsoft Learn: sz_CERT_STORE_PROV_SYSTEM_REGISTRY(_W) = "SystemRegistry"
        system_registry_provider = name == "systemregistry";
        wide_parameter = true;
    }

    if (memory_provider) return cert_open_store();
    if (collection_provider) {
        // Microsoft Learn: CERT_STORE_PROV_COLLECTION requires pvPara NULL.
        if (parameter != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        const u32 handle = m_next_store_handle++;
        m_collection_stores.emplace(handle, std::vector<CollectionSibling>{});
        ipc::ipc_log("CertOpenStore collection provider flags=0x%x -> 0x%x",
                     flags, handle);
        VITREO_TRACE("[crypt32] CertOpenStore(Collection,flags=0x%x) -> 0x%x\n",
                     flags, handle);
        return handle;
    }
    if (!system_provider && !system_registry_provider) {
        ipc::ipc_log(
            "CertOpenStore unsupported provider=0x%llx flags=0x%x",
            static_cast<unsigned long long>(provider_value), flags);
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }
    const u32 location = flags & CERT_SYSTEM_STORE_LOCATION_MASK;
    const std::string_view location_name = system_store_location_name(location);
    if ((flags & CERT_SYSTEM_STORE_RELOCATE_FLAG) != 0 ||
        location_name.empty()) {
        ipc::ipc_log(
            "CertOpenStore unsupported system location provider=0x%llx "
            "flags=0x%x",
            static_cast<unsigned long long>(provider_value), flags);
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (parameter == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    auto name_result = wide_parameter
        ? m.read_wide_string(parameter, 256)
        : m.read_ansi_string(parameter, 256);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::string name = name_result.value();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    const std::string store_key = system_store_key(location_name, name);
    ipc::ipc_log(
        "CertOpenStore %s provider=0x%llx flags=0x%x key='%s'",
        system_registry_provider ? "system_registry" : "system",
        static_cast<unsigned long long>(provider_value), flags,
        store_key.c_str());

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!load_system_store_locked(store_key)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return 0;
    }
    const u32 handle = m_next_store_handle++;
    m_system_store_names.emplace(handle, store_key);
    m_system_store_read_only.emplace(
        handle, (flags & CERT_STORE_READONLY_FLAG) != 0);
    VITREO_TRACE("[crypt32] CertOpenStore(%s,%s,flags=0x%x) -> 0x%x\n",
                 system_registry_provider ? "SystemRegistry" : "System",
                 store_key.c_str(), flags, handle);
    return handle;
}

u32 Crypt32Manager::cert_open_system_store_w(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(1);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    auto name_result = m.read_wide_string(name_ptr, 256);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    return open_system_store_by_name(name_result.value());
}

u32 Crypt32Manager::cert_open_system_store_a(Marshaler& m) {
    // Learn (CertOpenSystemStoreA): identical to the W form except the store
    // name is ANSI. Every system store name ("MY", "ROOT", "CA") is ASCII, so
    // only the read differs — the lookup below must not be duplicated.
    const GuestAddr name_ptr = m.arg_ptr(1);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    auto name_result = m.read_ansi_string(name_ptr);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    return open_system_store_by_name(name_result.value());
}

u32 Crypt32Manager::open_system_store_by_name(const std::string& raw_name) {
    std::string name = raw_name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    const std::string store_key =
        system_store_key(kCurrentUserStoreLocation, name);
    std::lock_guard<std::mutex> lock(m_mutex);
    u32 handle = m_next_store_handle++;
    if (!load_system_store_locked(store_key)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return 0;
    }
    m_system_store_names.emplace(handle, store_key);
    VITREO_TRACE("[crypt32] CertOpenSystemStoreW(%s) -> 0x%x\n",
                 name.c_str(), handle);
    return handle;
}

void Crypt32Manager::hold_store_locked(u32 store) {
    if (store != 0) ++m_store_holds[store];
}

void Crypt32Manager::release_store_hold_locked(u32 store) {
    auto hold = m_store_holds.find(store);
    if (hold == m_store_holds.end()) return;
    if (--hold->second != 0) return;
    m_store_holds.erase(hold);
    finish_closed_store_locked(store);
}

bool Crypt32Manager::store_has_live_contexts_locked(u32 store) const {
    for (const auto& [_, context] : m_cert_contexts) {
        if (context.store_handle == store && !context.owns_store_handle) {
            return true;
        }
    }
    return false;
}

void Crypt32Manager::finish_closed_store_locked(u32 store) {
    if (!m_stores_closed_while_held.contains(store) ||
        m_store_holds.contains(store) ||
        store_has_live_contexts_locked(store)) {
        return;
    }
    m_stores_closed_while_held.erase(store);
    erase_store_locked(store);
}

void Crypt32Manager::erase_store_locked(u32 store_handle) {
    auto memory = m_store_certs.find(store_handle);
    if (memory != m_store_certs.end()) {
        for (auto& [_, context] : m_cert_contexts) {
            if (context.store_handle == store_handle) {
                context.originating_store_certificates = memory->second;
            }
        }
        m_store_certs.erase(memory);
    }
    m_system_store_names.erase(store_handle);
    m_system_store_read_only.erase(store_handle);
    m_collection_stores.erase(store_handle);
    m_element_properties.erase(store_handle);
}

// BOOL CertCloseStore(HCERTSTORE hCertStore, DWORD dwFlags)
// Learn: the store is always closed. CERT_CLOSE_STORE_CHECK_FLAG reports
// FALSE / CRYPT_E_PENDING_CLOSE while contexts from it are still referenced
// (their memory stays valid); CERT_CLOSE_STORE_FORCE_FLAG frees every
// context of the store. Without CHECK the last error is preserved.
u32 Crypt32Manager::cert_close_store(u32 store_handle, u32 flags) {
    constexpr u32 CERT_CLOSE_STORE_FORCE_FLAG = 0x1;
    constexpr u32 CERT_CLOSE_STORE_CHECK_FLAG = 0x2;
    if (store_handle == 0) return 1;  // measured: NULL is TRUE, last error untouched
    std::vector<GuestAddr> released;
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool known = m_store_certs.contains(store_handle) ||
                           m_system_store_names.contains(store_handle) ||
                           m_collection_stores.contains(store_handle);
        if (!known || m_stores_closed_while_held.contains(store_handle)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return 0;
        }
        if ((flags & CERT_CLOSE_STORE_FORCE_FLAG) != 0) {
            std::vector<GuestAddr> contexts;
            for (const auto& [address, context] : m_cert_contexts) {
                if (context.store_handle == store_handle) contexts.push_back(address);
            }
            for (const GuestAddr context : contexts) {
                auto entry = m_cert_contexts.find(context);
                if (entry == m_cert_contexts.end()) continue;
                entry->second.reference_count = 1;
                auto blocks = release_cert_context_locked(context);
                released.insert(released.end(), blocks.begin(), blocks.end());
            }
        } else if ((flags & CERT_CLOSE_STORE_CHECK_FLAG) != 0) {
            for (const auto& [_, context] : m_cert_contexts) {
                if (context.store_handle == store_handle) pending = true;
            }
        }
        // A chain engine that names this store keeps it alive (Windows
        // duplicates the handle); the caller's handle is still closed. So does
        // every certificate context still taken from it (Learn CertCloseStore:
        // the store stays open until its contexts are freed), and a context's
        // hCertStore stays usable -- Chromium closes its memory store and then
        // passes the leaf's hCertStore to CertGetCertificateChain
        // (crypt32_ssl_chain_probe CSC_CLOSE_STORE).
        if (m_store_holds.contains(store_handle) ||
            store_has_live_contexts_locked(store_handle)) {
            m_stores_closed_while_held.insert(store_handle);
        } else {
            erase_store_locked(store_handle);
        }
    }
    for (GuestAddr block : released) free_guest(block);
    if (pending) {
        set_last_error(crypt32::err::kCryptEPendingClose);
        return 0;
    }
    VITREO_TRACE("[crypt32] CertCloseStore(0x%x) -> TRUE\n", store_handle);
    return 1;
}

bool Crypt32Manager::load_system_store_locked(const std::string& name) {
    if (m_vfs == nullptr) {
        m_system_store_certs.try_emplace(name);
        return true;
    }

    const usize separator = name.find('\\');
    if (separator == std::string::npos || separator + 1 >= name.size()) {
        return false;
    }
    const std::string location_directory =
        std::string(kCertificateStoresRoot) + "\\" +
        name.substr(0, separator);
    const std::array<std::string, 5> directories = {
        "C:\\ProgramData",
        "C:\\ProgramData\\Vitreo",
        std::string(kCertificateStoresRoot),
        location_directory,
        system_store_directory(name),
    };
    for (const auto& directory : directories) {
        auto created = m_vfs->create_dir(directory);
        if (created.is_err() &&
            created.error() != capsule::VfsError::PathAlreadyExists) {
            return false;
        }
    }

    auto listed = m_vfs->list_dir(system_store_directory(name));
    if (listed.is_err()) return false;
    auto entries = std::move(listed.value());
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) {
                  return left.name < right.name;
              });

    std::vector<StoredCertificate> certificates;
    for (const auto& entry : entries) {
        if (!has_certificate_suffix(entry.name) || entry.info.size == 0 ||
            entry.info.size > 0x100000) {
            continue;
        }
        const std::string path =
            system_store_directory(name) + "\\" + entry.name;
        auto opened = m_vfs->open(
            path, capsule::AccessMode::Read, capsule::ShareMode::Read,
            capsule::CreationDisposition::OpenExisting);
        if (opened.is_err()) return false;
        std::vector<u8> der(static_cast<usize>(entry.info.size));
        usize offset = 0;
        while (offset < der.size()) {
            auto read = m_vfs->read(
                opened.value(), std::span<u8>(der).subspan(offset));
            if (read.is_err() || read.value() == 0) {
                (void)m_vfs->close(opened.value());
                return false;
            }
            offset += read.value();
        }
        (void)m_vfs->close(opened.value());
        certificates.push_back(StoredCertificate{std::move(der), entry.name});
    }
    m_system_store_certs[name] = std::move(certificates);

    // Microsoft Learn: CertOpenStore(CERT_SYSTEM_STORE_LOCAL_MACHINE, "ROOT")
    // exposes the Trusted Root Certification Authorities store. Its physical
    // stores are additive: certificates installed by a guest do not replace
    // the platform trust anchors. Merge missing host anchors even when the
    // capsule already contains private roots.
    if (name == system_store_key(kLocalMachineStoreLocation, "ROOT")) {
        if (!seed_local_machine_root_from_host_locked(name)) {
            return false;
        }
    }
    return true;
}

bool Crypt32Manager::seed_local_machine_root_from_host_locked(
    const std::string& name) {
#ifdef __APPLE__
    // Opening LOCAL_MACHINE\ROOT must return the same anchors every time.
    // Identical DER already stored (including copies written by an earlier
    // open) is not a second trust anchor.
    {
        auto& stored = m_system_store_certs[name];
        std::unordered_set<std::string> seen;
        std::vector<StoredCertificate> unique;
        unique.reserve(stored.size());
        for (auto& certificate : stored) {
            const std::string key(
                reinterpret_cast<const char*>(certificate.der_data.data()),
                certificate.der_data.size());
            if (!seen.insert(key).second) {
                remove_system_record_locked(name, certificate.record_name);
                continue;
            }
            unique.push_back(std::move(certificate));
        }
        stored = std::move(unique);
    }
    const auto already_stored = [&](std::span<const u8> der) {
        const auto& existing = m_system_store_certs[name];
        return std::any_of(
            existing.begin(), existing.end(),
            [der](const StoredCertificate& stored) {
                return stored.der_data.size() == der.size() &&
                       std::equal(stored.der_data.begin(),
                                  stored.der_data.end(), der.begin());
            });
    };
    usize seeded = 0;
    const auto consider = [&](std::span<const u8> der) {
        if (der.empty() || already_stored(der)) return;
        std::string record_name;
        if (!append_system_record_locked(name, der, record_name)) return;
        m_system_store_certs[name].push_back(StoredCertificate{
            std::vector<u8>(der.begin(), der.end()), std::move(record_name)});
        ++seeded;
    };

    // SecTrustCopyAnchorCertificates fails or returns an empty array on some
    // macOS versions. SystemRootCertificates.keychain is the physical store
    // Windows LOCAL_MACHINE\ROOT corresponds to, so it is always merged.
    // DER equality keeps a later open from writing a second copy.
    CFArrayRef anchors = nullptr;
    const OSStatus status = SecTrustCopyAnchorCertificates(&anchors);
    CFIndex trust_count = 0;
    if (status == errSecSuccess && anchors != nullptr) {
        trust_count = CFArrayGetCount(anchors);
        for (CFIndex index = 0; index < trust_count; ++index) {
            const auto certificate = static_cast<SecCertificateRef>(
                const_cast<void*>(CFArrayGetValueAtIndex(anchors, index)));
            if (certificate == nullptr) continue;
            CFDataRef der_data = SecCertificateCopyData(certificate);
            if (der_data == nullptr) continue;
            const auto* bytes = CFDataGetBytePtr(der_data);
            const CFIndex length = CFDataGetLength(der_data);
            if (bytes != nullptr && length > 0) {
                consider(std::span<const u8>(bytes, static_cast<usize>(length)));
            }
            CFRelease(der_data);
        }
    } else {
        ipc::ipc_log(
            "CertOpenStore LocalMachine\\ROOT host seed failed status=%d",
            static_cast<int>(status));
    }
    if (anchors != nullptr) CFRelease(anchors);

    OSStatus keychain_status = errSecSuccess;
    SecKeychainRef keychain = nullptr;
    keychain_status = SecKeychainOpen(
        "/System/Library/Keychains/SystemRootCertificates.keychain",
        &keychain);
    if (keychain_status == errSecSuccess && keychain != nullptr) {
        SecKeychainSearchRef search = nullptr;
        if (SecKeychainSearchCreateFromAttributes(
                keychain, kSecCertificateItemClass, nullptr, &search) ==
                errSecSuccess &&
            search != nullptr) {
            for (;;) {
                SecKeychainItemRef item = nullptr;
                if (SecKeychainSearchCopyNext(search, &item) !=
                    errSecSuccess) {
                    break;
                }
                CFDataRef der_data = SecCertificateCopyData(
                    reinterpret_cast<SecCertificateRef>(item));
                if (der_data != nullptr) {
                    const auto* bytes = CFDataGetBytePtr(der_data);
                    const CFIndex length = CFDataGetLength(der_data);
                    if (bytes != nullptr && length > 0) {
                        consider(std::span<const u8>(
                            bytes, static_cast<usize>(length)));
                    }
                    CFRelease(der_data);
                }
                CFRelease(item);
            }
            CFRelease(search);
        }
        CFRelease(keychain);
    }
    if (m_system_store_certs[name].empty()) {
        // Called under m_mutex. One line per process when the store stays
        // empty so a missing trust anchor is visible without IPC tracing.
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(
                stderr,
                "[vitreo] crypt32!CertOpenStore LocalMachine\\ROOT: no host "
                "trust anchors (SecTrust status=%d count=%ld keychain=%d)\n",
                static_cast<int>(status), static_cast<long>(trust_count),
                static_cast<int>(keychain_status));
        }
    }
    ipc::ipc_log(
        "CertOpenStore LocalMachine\\ROOT seeded %zu host trust anchors",
        seeded);
    return true;
#else
    (void)name;
    return true;
#endif
}

bool Crypt32Manager::append_system_record_locked(
    const std::string& name, std::span<const u8> der,
    std::string& record_name) {
    if (m_vfs == nullptr) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%016llX.cer",
                      static_cast<unsigned long long>(
                          m_system_store_certs[name].size() + 1));
        record_name = buffer;
        return true;
    }
    u64 next = 1;
    for (const auto& certificate : m_system_store_certs[name]) {
        next = std::max(next, record_number(certificate.record_name) + 1);
    }
    for (;;) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%016llX.cer",
                      static_cast<unsigned long long>(next++));
        const std::string path =
            system_store_directory(name) + "\\" + buffer;
        auto opened = m_vfs->open(
            path, capsule::AccessMode::Write, capsule::ShareMode::Read,
            capsule::CreationDisposition::CreateNew);
        if (opened.is_err()) {
            if (opened.error() == capsule::VfsError::PathAlreadyExists) continue;
            return false;
        }
        usize offset = 0;
        bool success = true;
        while (offset < der.size()) {
            auto written = m_vfs->write(opened.value(), der.subspan(offset));
            if (written.is_err() || written.value() == 0) {
                success = false;
                break;
            }
            offset += written.value();
        }
        if (success) success = m_vfs->flush(opened.value()).is_ok();
        (void)m_vfs->close(opened.value());
        if (!success) {
            (void)m_vfs->remove(path);
            return false;
        }
        record_name = buffer;
        return true;
    }
}

bool Crypt32Manager::overwrite_system_record_locked(
    const std::string& name, const std::string& record_name,
    std::span<const u8> der) {
    if (m_vfs == nullptr) return true;
    const std::string path =
        system_store_directory(name) + "\\" + record_name;
    auto opened = m_vfs->open(
        path, capsule::AccessMode::Write, capsule::ShareMode::Read,
        capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;
    usize offset = 0;
    bool success = true;
    while (offset < der.size()) {
        auto written = m_vfs->write(opened.value(), der.subspan(offset));
        if (written.is_err() || written.value() == 0) {
            success = false;
            break;
        }
        offset += written.value();
    }
    if (success) success = m_vfs->flush(opened.value()).is_ok();
    (void)m_vfs->close(opened.value());
    return success;
}

bool Crypt32Manager::remove_system_record_locked(
    const std::string& name, const std::string& record_name) {
    if (m_vfs == nullptr) return true;
    return m_vfs->remove(
        system_store_directory(name) + "\\" + record_name).is_ok();
}

u32 Crypt32Manager::cert_add_store_to_collection(Marshaler& m) {
    constexpr u32 CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG = 0x1u;
    const u32 collection_handle = static_cast<u32>(m.arg(0));
    const u32 sibling_handle = static_cast<u32>(m.arg(1));
    const u32 update_flags = static_cast<u32>(m.arg(2));
    const u32 priority = static_cast<u32>(m.arg(3));

    std::lock_guard<std::mutex> lock(m_mutex);
    auto collection = m_collection_stores.find(collection_handle);
    if (collection == m_collection_stores.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    // Optional sibling NULL is a documented no-op that still succeeds.
    if (sibling_handle == 0) {
        return 1;
    }
    const bool sibling_ok =
        m_store_certs.contains(sibling_handle) ||
        m_system_store_names.contains(sibling_handle) ||
        m_collection_stores.contains(sibling_handle);
    if (!sibling_ok) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    if (sibling_handle == collection_handle) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    CollectionSibling sibling{};
    sibling.store_handle = sibling_handle;
    if (auto system = m_system_store_names.find(sibling_handle);
        system != m_system_store_names.end()) {
        sibling.system_store_name = system->second;
    }
    sibling.update_flags = update_flags & CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG;
    sibling.priority = priority;

    auto& siblings = collection->second;
    if (priority == 0) {
        siblings.push_back(sibling);
    } else {
        auto insert_at = siblings.begin();
        while (insert_at != siblings.end() && insert_at->priority >= priority) {
            ++insert_at;
        }
        siblings.insert(insert_at, sibling);
    }
    ipc::ipc_log(
        "CertAddStoreToCollection collection=0x%x sibling=0x%x "
        "update=0x%x priority=%u siblings=%zu",
        collection_handle, sibling_handle, update_flags, priority,
        siblings.size());
    return 1;
}

GuestAddr Crypt32Manager::cert_enum_certificates_in_store(Marshaler& m) {
    std::vector<GuestAddr> released_allocations;
    auto free_released = [this, &released_allocations](void*) {
        for (GuestAddr allocation : released_allocations) free_guest(allocation);
    };
    std::unique_ptr<void, decltype(free_released)> release_guard(
        reinterpret_cast<void*>(1), free_released);
    const u32 store_handle = static_cast<u32>(m.arg(0));
    const GuestAddr previous = m.arg_ptr(1);
    constexpr u32 CRYPT_E_NOT_FOUND = 0x80092004u;

    std::vector<u8> der;
    std::string record_name;
    usize index = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto system_name = m_system_store_names.find(store_handle);
        auto memory_store = m_store_certs.find(store_handle);
        auto collection = m_collection_stores.find(store_handle);
        if (system_name == m_system_store_names.end() &&
            memory_store == m_store_certs.end() &&
            collection == m_collection_stores.end()) {
            if (previous != 0) released_allocations = release_cert_context_locked(previous);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return 0;
        }
        if (previous != 0) {
            auto prior = m_cert_contexts.find(previous);
            if (prior == m_cert_contexts.end() ||
                prior->second.store_handle != store_handle ||
                prior->second.store_index == std::numeric_limits<usize>::max()) {
                released_allocations = release_cert_context_locked(previous);
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                return 0;
            }
            index = prior->second.store_index + 1;
            released_allocations = release_cert_context_locked(previous);
        } else if (system_name != m_system_store_names.end() &&
                   !load_system_store_locked(system_name->second)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return 0;
        }

        if (collection != m_collection_stores.end()) {
            struct FlatEntry {
                std::vector<u8> der;
                std::string record;
            };
            std::vector<FlatEntry> flat;
            for (const auto& sibling : collection->second) {
                auto sibling_system = m_system_store_names.find(
                    sibling.store_handle);
                const std::string* system_name =
                    sibling_system != m_system_store_names.end()
                        ? &sibling_system->second
                        : (sibling.system_store_name.empty()
                               ? nullptr
                               : &sibling.system_store_name);
                if (system_name != nullptr) {
                    if (!load_system_store_locked(*system_name)) continue;
                    for (const auto& certificate :
                         m_system_store_certs[*system_name]) {
                        flat.push_back(
                            FlatEntry{certificate.der_data, certificate.record_name});
                    }
                    continue;
                }
                auto sibling_memory = m_store_certs.find(sibling.store_handle);
                if (sibling_memory != m_store_certs.end()) {
                    for (const auto& certificate : sibling_memory->second) {
                        flat.push_back(FlatEntry{certificate, {}});
                    }
                }
            }
            if (index >= flat.size()) {
                set_last_error(CRYPT_E_NOT_FOUND);
                return 0;
            }
            der = std::move(flat[index].der);
            record_name = std::move(flat[index].record);
        } else if (system_name != m_system_store_names.end()) {
            const auto& certificates = m_system_store_certs[system_name->second];
            if (index >= certificates.size()) {
                set_last_error(CRYPT_E_NOT_FOUND);
                return 0;
            }
            der = certificates[index].der_data;
            record_name = certificates[index].record_name;
        } else {
            if (index >= memory_store->second.size()) {
                set_last_error(CRYPT_E_NOT_FOUND);
                return 0;
            }
            der = memory_store->second[index];
        }
    }

    const GuestAddr context = create_cert_context_from_der(
        m, der, store_handle, 0x00010001u, std::move(record_name), index);
    if (context == 0) {
        set_last_error(error_codes::ERROR_INVALID_DATA);
        return 0;
    }
    return context;
}

namespace {

/// Windows FILETIME for "now": 100ns ticks since 1601-01-01 UTC.
u64 current_filetime() {
    return (static_cast<u64>(std::time(nullptr)) + 11644473600ULL) * 10000000ULL;
}

} // namespace

/// Flatten one store's certificates in enumeration order — memory store,
/// system store, or collection. Caller holds m_mutex.
std::vector<std::vector<u8>> Crypt32Manager::store_certificates_locked(
    u32 store_handle) {
    std::vector<std::vector<u8>> flat;

    auto collection = m_collection_stores.find(store_handle);
    if (collection != m_collection_stores.end()) {
        for (const auto& sibling : collection->second) {
            if (!sibling.system_store_name.empty() &&
                !m_system_store_names.contains(sibling.store_handle)) {
                if (!load_system_store_locked(sibling.system_store_name)) {
                    continue;
                }
                for (const auto& record :
                     m_system_store_certs[sibling.system_store_name]) {
                    flat.push_back(record.der_data);
                }
                continue;
            }
            for (auto& der : store_certificates_locked(
                     sibling.store_handle)) {
                flat.push_back(std::move(der));
            }
        }
        return flat;
    }

    auto system_name = m_system_store_names.find(store_handle);
    if (system_name != m_system_store_names.end()) {
        if (!load_system_store_locked(system_name->second)) return flat;
        for (const auto& record : m_system_store_certs[system_name->second]) {
            flat.push_back(record.der_data);
        }
        return flat;
    }

    auto memory_store = m_store_certs.find(store_handle);
    if (memory_store != m_store_certs.end()) {
        flat = memory_store->second;
    }
    return flat;
}

GuestAddr Crypt32Manager::cert_get_issuer_certificate_from_store(Marshaler& m) {
    std::vector<GuestAddr> released_allocations;
    auto free_released = [this, &released_allocations](void*) {
        for (GuestAddr allocation : released_allocations) free_guest(allocation);
    };
    std::unique_ptr<void, decltype(free_released)> release_guard(
        reinterpret_cast<void*>(1), free_released);
    // PCCERT_CONTEXT CertGetIssuerCertificateFromStore(
    //   HCERTSTORE hCertStore, PCCERT_CONTEXT pSubjectContext,
    //   PCCERT_CONTEXT pPrevIssuerContext, DWORD *pdwFlags)
    //
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetissuercertificatefromstore
    // Enumerates the store's certificates whose Subject equals the subject
    // certificate's Issuer. `pPrevIssuerContext` continues a previous
    // enumeration and is always freed. Returns NULL with CRYPT_E_NOT_FOUND
    // once no further issuer exists. `pdwFlags` selects checks on input and
    // reports the ones that *failed* on output.
    const u32 store_handle = static_cast<u32>(m.arg(0));
    const GuestAddr subject_ctx = m.arg_ptr(1);
    const GuestAddr previous = m.arg_ptr(2);
    const GuestAddr flags_ptr = m.arg_ptr(3);
    constexpr u32 CRYPT_E_NOT_FOUND = 0x80092004u;
    constexpr u32 CRYPT_E_SELF_SIGNED = 0x80092007u;
    constexpr u32 CERT_STORE_SIGNATURE_FLAG      = 0x00000001u;
    constexpr u32 CERT_STORE_TIME_VALIDITY_FLAG  = 0x00000002u;

    if (subject_ctx == 0) {
        if (previous != 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            released_allocations = release_cert_context_locked(previous);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    const u32 requested_flags =
        flags_ptr != 0 ? m.read_u32(flags_ptr).value_or(0) : 0;

    std::vector<u8> issuer_der_match;
    std::vector<u8> subject_der;
    std::vector<std::vector<u8>> candidates;
    usize start = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto subject = m_cert_contexts.find(subject_ctx);
        if (subject == m_cert_contexts.end()) {
            if (previous != 0) released_allocations = release_cert_context_locked(previous);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        subject_der = subject->second.der_data;
        auto parsed_subject = x509::parse_certificate(subject_der);
        if (!parsed_subject) {
            if (previous != 0) released_allocations = release_cert_context_locked(previous);
            set_last_error(error_codes::ERROR_INVALID_DATA);
            return 0;
        }
        issuer_der_match = parsed_subject->issuer_der;

        if (previous != 0) {
            auto prior = m_cert_contexts.find(previous);
            if (prior != m_cert_contexts.end() &&
                prior->second.store_index !=
                    std::numeric_limits<usize>::max()) {
                start = prior->second.store_index + 1;
            }
            released_allocations = release_cert_context_locked(previous);
        }

        candidates = store_certificates_locked(store_handle);
        // A context carries its originating store's material, which is how a
        // chain can still be built after CertCloseStore.
        if (candidates.empty()) {
            candidates = subject->second.originating_store_certificates;
        }
    }

    // Learn: a self-signed root has no issuer other than itself, and the
    // call reports that as NULL + CRYPT_E_SELF_SIGNED. This must be decided
    // from the subject alone, before any store search: returning some other
    // certificate that merely shares the root's name (a re-issued root, a
    // key-rollover certificate) makes a caller that walks the chain until
    // NULL ping-pong between the two forever.
    {
        auto self_check = x509::parse_certificate(subject_der);
        if (self_check && self_check->issuer_der == self_check->subject_der &&
            certificate_signed_by(*self_check, *self_check)) {
            set_last_error(CRYPT_E_SELF_SIGNED);
            return 0;
        }
    }

    for (usize index = start; index < candidates.size(); ++index) {
        auto parsed = x509::parse_certificate(candidates[index]);
        if (!parsed || parsed->subject_der != issuer_der_match) continue;
        // A self-issued subject whose own key does not verify its signature
        // is still never its own issuer.
        if (candidates[index] == subject_der) continue;

        u32 failed = 0;
        if ((requested_flags & CERT_STORE_TIME_VALIDITY_FLAG) != 0) {
            const u64 now = current_filetime();
            if (parsed->not_before_filetime != 0 &&
                parsed->not_after_filetime != 0 &&
                (now < parsed->not_before_filetime ||
                 now > parsed->not_after_filetime)) {
                failed |= CERT_STORE_TIME_VALIDITY_FLAG;
            }
        }
        if ((requested_flags & CERT_STORE_SIGNATURE_FLAG) != 0) {
            auto subject_parsed = x509::parse_certificate(subject_der);
            if (!subject_parsed ||
                !certificate_signed_by(*subject_parsed, *parsed)) {
                failed |= CERT_STORE_SIGNATURE_FLAG;
            }
        }
        if (flags_ptr != 0) (void)m.write_u32(flags_ptr, failed);

        const GuestAddr context = create_cert_context_from_der(
            m, candidates[index], store_handle, 0x00010001u, {}, index);
        if (context == 0) {
            set_last_error(error_codes::ERROR_INVALID_DATA);
            return 0;
        }
        // Full guest pointer: an x64 context above 4 GiB must not be
        // truncated on the way back to the guest.
        return context;
    }

    set_last_error(CRYPT_E_NOT_FOUND);
    return 0;
}

u32 Crypt32Manager::cert_delete_certificate_from_store(Marshaler& m) {
    std::vector<GuestAddr> released_allocations;
    auto free_released = [this, &released_allocations](void*) {
        for (GuestAddr allocation : released_allocations) free_guest(allocation);
    };
    std::unique_ptr<void, decltype(free_released)> release_guard(
        reinterpret_cast<void*>(1), free_released);
    const GuestAddr context = m.arg_ptr(0);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto certificate = m_cert_contexts.find(context);
    if (certificate == m_cert_contexts.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    const CertEntry entry = certificate->second;
    released_allocations = release_cert_context_locked(context);
    auto system_name = m_system_store_names.find(entry.store_handle);
    if (system_name != m_system_store_names.end()) {
        if (m_system_store_read_only[entry.store_handle]) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return 0;
        }
        if (!load_system_store_locked(system_name->second)) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return 0;
        }
        auto& records = m_system_store_certs[system_name->second];
        auto found = std::find_if(records.begin(), records.end(),
            [&](const StoredCertificate& record) {
                return !entry.persistent_record.empty()
                    ? record.record_name == entry.persistent_record
                    : record.der_data == entry.der_data;
            });
        if (found == records.end() ||
            !remove_system_record_locked(system_name->second,
                                         found->record_name)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        records.erase(found);
        set_last_error(error_codes::ERROR_SUCCESS);
        return 1;
    }

    auto memory_store = m_store_certs.find(entry.store_handle);
    if (memory_store == m_store_certs.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    auto found = std::find(
        memory_store->second.begin(), memory_store->second.end(), entry.der_data);
    if (found == memory_store->second.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    memory_store->second.erase(found);
    return 1;
}

u32 Crypt32Manager::cert_find_certificate_in_store()  { return 0; }

u32 Crypt32Manager::crypt_query_object(Marshaler& m) {
    static constexpr u32 kObjectFile = 1;
    static constexpr u32 kContentFlagPkcs7SignedEmbed = 0x400;
    static constexpr u32 kFormatFlagBinary = 0x2;
    static constexpr u32 kContentPkcs7SignedEmbed = 10;
    static constexpr u32 kFormatBinary = 1;
    static constexpr u32 kCryptENoMatch = 0x80092009u;

    const u32 object_type = static_cast<u32>(m.arg(0));
    const GuestAddr object = m.arg_ptr(1);
    const u32 expected_content = static_cast<u32>(m.arg(2));
    const u32 expected_format = static_cast<u32>(m.arg(3));
    const u32 flags = static_cast<u32>(m.arg(4));
    const GuestAddr encoding_out = m.arg_ptr(5);
    const GuestAddr content_out = m.arg_ptr(6);
    const GuestAddr format_out = m.arg_ptr(7);
    const GuestAddr store_out = m.arg_ptr(8);
    const GuestAddr message_out = m.arg_ptr(9);
    const GuestAddr context_out = m.arg_ptr(10);

    auto zero_outputs = [&] {
        if (encoding_out) (void)m.write_u32(encoding_out, 0);
        if (content_out) (void)m.write_u32(content_out, 0);
        if (format_out) (void)m.write_u32(format_out, 0);
        if (store_out) (void)m.write_ptr(store_out, 0);
        if (message_out) (void)m.write_ptr(message_out, 0);
        if (context_out) (void)m.write_ptr(context_out, 0);
    };
    zero_outputs();

    if (object_type != kObjectFile || object == 0 || flags != 0 || m_vfs == nullptr ||
        (expected_content & kContentFlagPkcs7SignedEmbed) == 0 ||
        (expected_format & kFormatFlagBinary) == 0) {
        crypt32::report_unimplemented(
            "crypt32!CryptQueryObject",
            "anything but a file's embedded PKCS#7 (CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, binary)");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        return 0;
    }

    // Anything that is not a readable PE or cabinet with an embedded
    // signature is CRYPT_E_NO_MATCH, outputs zeroed (crypt_msg_probe "query" rows).
    auto no_match = [&] {
        set_last_error(kCryptENoMatch);
        return 0u;
    };
    auto path_result = m.read_wide_string(object);
    if (path_result.is_err()) return no_match();
    const std::string& path = path_result.value();
    auto info_result = m_vfs->stat(path);
    if (info_result.is_err() ||
        capsule::has_attribute(info_result.value().attributes, capsule::FileAttributes::Directory) ||
        info_result.value().size > 512u * 1024u * 1024u) {
        return no_match();
    }
    auto open_result = m_vfs->open(path, capsule::AccessMode::Read,
                                   capsule::ShareMode::ReadWrite,
                                   capsule::CreationDisposition::OpenExisting);
    if (open_result.is_err()) return no_match();
    const capsule::FileHandle file = open_result.value();
    std::vector<u8> image(static_cast<usize>(info_result.value().size));
    usize total = 0;
    while (total < image.size()) {
        auto read_result = m_vfs->read(file, std::span<u8>(image).subspan(total));
        if (read_result.is_err() || read_result.value() == 0) break;
        total += read_result.value();
    }
    (void)m_vfs->close(file);
    if (total != image.size()) return no_match();

    // The subject interface packages Windows consults: a cabinet's signature
    // sits in its CFHEADER reserve area, a PE's in its certificate table
    // (crypt_signer_trio_probe "query cab_*" rows: a signed cabinet is
    // CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED like a PE; one whose signature
    // cannot be located is CRYPT_E_NO_MATCH).
    // A structured storage (Windows Installer package) carries its PKCS#7 in
    // the root stream "\x05DigitalSignature" (the same SIP WinVerifyTrust uses).
    std::vector<u8> pkcs7;
    if (authenticode::is_compound_file_image(image)) {
        auto located = authenticode::locate_storage_signature(image);
        if (located.is_err()) return no_match();
        pkcs7 = std::move(located.value().pkcs7);
    } else if (authenticode::is_cab_image(image)) {
        auto located = authenticode::locate_cab_signature(image);
        if (located.is_err()) return no_match();
        pkcs7 = std::move(located.value().pkcs7);
    } else {
        auto located = authenticode::locate_pe_signature(image);
        if (located.is_err()) return no_match();
        pkcs7 = std::move(located.value().pkcs7);
    }
    auto parsed = authenticode::parse_signed_data(pkcs7);
    if (parsed.is_err()) {
        ipc::ipc_log("CryptQueryObject path='%s' -> FALSE (%s)", path.c_str(),
                     authenticode::parse_error_string(parsed.error()));
        return no_match();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const u32 store_handle = m_next_store_handle++;
    fill_message_store_locked(store_handle, parsed.value().certificates);
    const u32 message_handle = m_next_message_handle++;
    QueryMessage message;
    message.certificates = std::move(parsed.value().certificates);
    message.signers = std::move(parsed.value().signers);
    message.content_type = parsed.value().content_type;
    message.content = std::move(parsed.value().content);
    message.content_value = std::move(parsed.value().content_value);
    m_query_messages.emplace(message_handle, std::move(message));

    if (encoding_out) (void)m.write_u32(encoding_out, 0x00010001u);
    if (content_out) (void)m.write_u32(content_out, kContentPkcs7SignedEmbed);
    if (format_out) (void)m.write_u32(format_out, kFormatBinary);
    if (store_out) (void)m.write_ptr(store_out, store_handle);
    if (message_out) (void)m.write_ptr(message_out, message_handle);
    return 1;
}

// BOOL CryptMsgGetParam(HCRYPTMSG hCryptMsg, DWORD dwParamType,
//     DWORD dwIndex, void* pvData, DWORD* pcbData)
// Over the signed message CryptQueryObject / WinVerifyTrust decoded.
// Measured (crypt_msg_probe): size query and ERROR_MORE_DATA as usual; a
// successful SIGNER_INFO / SIGNER_CERT_INFO leaves last error 0; a signer
// index out of range is CRYPT_E_INVALID_INDEX, a certificate index out of
// range fails without touching the last error; an unknown parameter is
// CRYPT_E_INVALID_MSG_TYPE.
u32 Crypt32Manager::crypt_msg_get_param(Marshaler& m) {
    constexpr u32 kCryptEInvalidMsgType = 0x80091004u;
    constexpr u32 kCryptEInvalidIndex = 0x80091008u;
    constexpr u32 kCryptEAttributesMissing = 0x8009100Fu;
    const u32 message_handle = static_cast<u32>(m.arg(0));
    const u32 param_type = m.arg32(1);
    const u32 index = m.arg32(2);
    const GuestAddr data = m.arg_ptr(3);
    const GuestAddr size_out = m.arg_ptr(4);
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;

    crypt32::GuestBlock block(is64);
    std::optional<u32> error_after;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto message = m_query_messages.find(message_handle);
        if (message == m_query_messages.end() || size_out == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return 0;
        }
        const QueryMessage& q = message->second;
        auto dword = [&](u32 value) { block.put_u32(block.reserve(4, 1), value); };
        auto signer = [&]() -> const authenticode::SignerInfo* {
            return index < q.signers.size() ? &q.signers[index] : nullptr;
        };
        switch (param_type) {
        case 1: dword(2); break;                                   // CMSG_SIGNED
        case 2: block.add_bytes(q.content, 1); break;              // CMSG_CONTENT_PARAM
        case 4: block.add_cstring(q.content_type, 1); break;       // inner content type
        case 5: dword(static_cast<u32>(q.signers.size())); break;
        case 6: {
            const auto* s = signer();
            if (s == nullptr) { set_last_error(kCryptEInvalidIndex); return 0; }
            (void)crypt32::put_cmsg_signer_info(block, *s);
            error_after = 0;
            break;
        }
        case 7: {                                                  // CERT_INFO: Issuer + SerialNumber
            const auto* s = signer();
            if (s == nullptr) { set_last_error(kCryptEInvalidIndex); return 0; }
            const crypt32::CertLayout l(is64);
            const u32 info = block.reserve(l.info_size);
            block.put_blob(info + l.issuer, s->issuer);
            const std::vector<u8> serial(s->serial.rbegin(), s->serial.rend());
            block.put_blob(info + l.serial, serial);
            error_after = 0;
            break;
        }
        case 8: {                                                  // CRYPT_ALGORITHM_IDENTIFIER
            const auto* s = signer();
            if (s == nullptr) { set_last_error(kCryptEInvalidIndex); return 0; }
            const u32 alg = block.reserve(3 * ps);
            block.put_ptr(alg, block.add_cstring(s->digest_oid));
            if (!(s->digest_parameters.size() == 2 && s->digest_parameters[0] == 0x05)) {
                block.put_blob(alg + ps, s->digest_parameters);
            }
            break;
        }
        case 9:
        case 10: {                                                 // CRYPT_ATTRIBUTES
            const auto* s = signer();
            if (s == nullptr) { set_last_error(kCryptEInvalidIndex); return 0; }
            const auto& list = param_type == 9 ? s->signed_attribute_list : s->unsigned_attribute_list;
            if (list.empty()) { set_last_error(kCryptEAttributesMissing); return 0; }
            const u32 attrs = block.reserve(2 * ps);
            block.put_u32(attrs, static_cast<u32>(list.size()));
            const u32 array = block.reserve(static_cast<u32>(list.size()) * 3 * ps);
            block.put_ptr(attrs + ps, array);
            for (usize i = 0; i < list.size(); ++i) {
                const u32 a = array + static_cast<u32>(i) * 3 * ps;
                block.put_ptr(a, block.add_cstring(list[i].oid));
                block.put_u32(a + ps, static_cast<u32>(list[i].values.size()));
                const u32 values = block.reserve(static_cast<u32>(list[i].values.size()) * 2 * ps);
                block.put_ptr(a + 2 * ps, values);
                for (usize v = 0; v < list[i].values.size(); ++v) {
                    block.put_blob(values + static_cast<u32>(v) * 2 * ps, list[i].values[v]);
                }
            }
            break;
        }
        case 11: dword(static_cast<u32>(q.certificates.size())); break;
        case 12:
            if (index >= q.certificates.size()) return 0;          // last error untouched
            block.add_bytes(q.certificates[index].der, 1);
            break;
        case 3: case 13: case 14: case 20: case 21: case 22: case 30: case 31: case 32:
        case 38: case 39:
            crypt32::report_unimplemented("crypt32!CryptMsgGetParam",
                                          "this signed-message parameter type");
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            return 0;
        default:
            set_last_error(kCryptEInvalidMsgType);
            return 0;
        }
    }
    const u32 needed = block.size();
    const u32 capacity = m.read_u32(size_out).value_or(0);
    (void)m.write_u32(size_out, needed);
    if (data == 0) return 1;
    if (capacity < needed) {
        set_last_error(error_codes::ERROR_MORE_DATA);
        return 0;
    }
    const auto image = block.resolve(data);
    if (m.memory().write_bytes(data, reinterpret_cast<const Byte*>(image.data()),
                               image.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return 0;
    }
    if (error_after) set_last_error(*error_after);
    return 1;
}

u32 Crypt32Manager::crypt_msg_close(Marshaler& m) {
    const u32 message_handle = static_cast<u32>(m.arg(0));
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_query_messages.erase(message_handle) == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    return 1;
}

// CertFindCertificateInStore and CertGetNameString live in cert_find_names.cpp.

u32 Crypt32Manager::cert_name_to_str_a(Marshaler& m) {
    // Microsoft Learn (wincrypt!CertNameToStrA): convert CERT_NAME_BLOB DER to
    // a null-terminated ANSI string; return count includes the terminator.
    (void)m.arg(0); // encoding type ignored for X.509 Name
    const GuestAddr name_blob = m.arg_ptr(1);
    const u32 str_type = static_cast<u32>(m.arg(2));
    const GuestAddr output = m.arg_ptr(3);
    const u32 capacity = static_cast<u32>(m.arg(4));
    if (name_blob == 0) return 1;
    const bool is64 = m.cpu_is_64bit();
    const u32 cb = m.read_u32(name_blob).value_or(0);
    const GuestAddr pb = is64
        ? static_cast<GuestAddr>(m.read_u64(name_blob + 8).value_or(0))
        : static_cast<GuestAddr>(m.read_u32(name_blob + 4).value_or(0));
    if (cb == 0 || pb == 0 || cb > 0x100000u) {
        if (output != 0 && capacity != 0) (void)m.write_u8(output, 0);
        return 1;
    }
    std::vector<u8> der(cb);
    for (u32 i = 0; i < cb; ++i) {
        der[i] = m.read_u8(pb + i).value_or(0);
    }
    const std::string text = x509::format_distinguished_name(der, str_type);
    const u32 required = static_cast<u32>(text.size() + 1);
    if (output == 0 || capacity == 0) return required;
    if (capacity < required) {
        (void)m.write_u8(output, 0);
        return required;
    }
    for (usize i = 0; i < text.size(); ++i) {
        if (m.write_u8(output + i, static_cast<u8>(text[i])).is_err()) return 0;
    }
    if (m.write_u8(output + text.size(), 0).is_err()) return 0;
    return required;
}

u32 Crypt32Manager::cert_name_to_str_w(Marshaler& m) {
    (void)m.arg(0);
    const GuestAddr name_blob = m.arg_ptr(1);
    const u32 str_type = static_cast<u32>(m.arg(2));
    const GuestAddr output = m.arg_ptr(3);
    const u32 capacity = static_cast<u32>(m.arg(4));
    if (name_blob == 0) return 1;
    const bool is64 = m.cpu_is_64bit();
    const u32 cb = m.read_u32(name_blob).value_or(0);
    const GuestAddr pb = is64
        ? static_cast<GuestAddr>(m.read_u64(name_blob + 8).value_or(0))
        : static_cast<GuestAddr>(m.read_u32(name_blob + 4).value_or(0));
    if (cb == 0 || pb == 0 || cb > 0x100000u) {
        if (output != 0 && capacity != 0) (void)m.write_u16(output, 0);
        return 1;
    }
    std::vector<u8> der(cb);
    for (u32 i = 0; i < cb; ++i) {
        der[i] = m.read_u8(pb + i).value_or(0);
    }
    const std::string text = x509::format_distinguished_name(der, str_type);
    const u32 required = static_cast<u32>(text.size() + 1);
    if (output == 0 || capacity == 0) return required;
    if (capacity < required) {
        (void)m.write_u16(output, 0);
        return required;
    }
    if (m.write_wide_string(output, text).is_err()) return 0;
    return required;
}

} // namespace vitreo::winapi
