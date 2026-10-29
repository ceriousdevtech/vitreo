// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_provider_exports.cpp
/// @brief advapi32 legacy CryptoAPI providers: CryptEnumProvidersA/W,
/// CryptAcquireContextA/W, CryptReleaseContext and CryptContextAddRef.
///
/// Part of the legacy CryptoAPI shim; the other slices live in the
/// crypto_*_exports.cpp files and share crypto_internal.hpp.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "winapi/advapi32/security/crypto_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace vitreo::winapi {

using namespace capi;

namespace {

// ---------------------------------------------------------------------------
// Legacy CryptoAPI provider catalogue (tools/oracle_probe/advapi_capi_probe):
// the RSA-family CSPs a Windows 10 box offers, with their PP_ENUMALGS /
// PP_ENUMALGS_EX records generated from the measurement.
// ---------------------------------------------------------------------------

constexpr CapiProviderInfo kCapiProviders[] = {
#include "winapi/advapi32/security/capi_providers.inc"
};

/// The provider CryptAcquireContext picks when pszProvider is NULL or "":
/// PROV_RSA_FULL -> Strong, PROV_RSA_AES -> AES, PROV_RSA_SCHANNEL -> SChannel
/// (probe acq_type_*).
[[nodiscard]] const CapiProviderInfo* capi_default_provider(u32 type) {
    const char* want = type == 1 ? "Microsoft Strong Cryptographic Provider"
                     : type == 24 ? "Microsoft Enhanced RSA and AES Cryptographic Provider"
                     : type == 12 ? "Microsoft RSA SChannel Cryptographic Provider"
                                  : nullptr;
    for (const auto& p : kCapiProviders) {
        if (want != nullptr && std::strcmp(p.name, want) == 0) return &p;
    }
    return nullptr;
}

} // namespace

/// Registers CryptEnumProvidersA/W, CryptAcquireContextA/W,
/// CryptReleaseContext, CryptContextAddRef.
void add_crypto_provider_exports(DllExports& exports, SecurityStubs& stubs) {
    // ====================================================================
    // Legacy CryptoAPI hash + RNG shim
    //
    // bcrypt.dll already provides full SHA1/SHA256/MD5/HMAC via
    // CommonCrypto.  Many older Windows binaries (and parts of MSVC CRT)
    // still resolve advapi32!Crypt* exports — without these registrations
    // the IAT fails and the app crashes.  We back the API with our own
    // CommonCrypto contexts so it works end-to-end.
    // ====================================================================

    static CryptState* s_state = &capi::crypt_state();
    static constexpr char PROVIDER_NAME[] =
        "Microsoft Base Cryptographic Provider v1.0";
    static constexpr u32 PROV_RSA_FULL = 1;

    // CryptEnumProvidersA / W
    //
    // Expose the standard base-provider identity for the subset Vitreo
    // implements: contexts, CSPRNG, MD5/SHA hashes, RSA key blobs, and
    // PKCS#1 signature creation/verification.
    auto validate_enumeration = [](Marshaler& m) {
        constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
        const GuestAddr reserved = m.arg_ptr(1);
        const u32 flags = static_cast<u32>(m.arg(2));
        const GuestAddr size_ptr = m.arg_ptr(5);

        if (reserved != 0 || size_ptr == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return false;
        }
        if (flags != 0) {
            set_last_error(NTE_BAD_FLAGS);
            m.set_return(0);
            return false;
        }
        return true;
    };

    auto enum_providers_a = [validate_enumeration](Marshaler& m) {
        if (!validate_enumeration(m)) return;
        const u32 index = static_cast<u32>(m.arg(0));
        if (index != 0) {
            set_last_error(error_codes::ERROR_NO_MORE_ITEMS);
            m.set_return(0);
            return;
        }
        const GuestAddr type_ptr = m.arg_ptr(3);
        const GuestAddr name_ptr = m.arg_ptr(4);
        const GuestAddr size_ptr = m.arg_ptr(5);
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        constexpr u32 required = sizeof(PROVIDER_NAME);
        (void)m.write_u32(size_ptr, required);
        if (type_ptr != 0) (void)m.write_u32(type_ptr, PROV_RSA_FULL);
        if (name_ptr == 0) {
            m.set_return(1);
            return;
        }
        if (capacity < required) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            m.set_return(0);
            return;
        }
        (void)m.write_ansi_string(name_ptr, PROVIDER_NAME);
        VITREO_TRACE(
            "[advapi32] CryptEnumProvidersA(index=0) -> "
            "\"%s\" type=PROV_RSA_FULL\n",
            PROVIDER_NAME);
        m.set_return(1);
    };

    auto enum_providers_w = [validate_enumeration](Marshaler& m) {
        if (!validate_enumeration(m)) return;
        const u32 index = static_cast<u32>(m.arg(0));
        if (index != 0) {
            set_last_error(error_codes::ERROR_NO_MORE_ITEMS);
            m.set_return(0);
            return;
        }
        const GuestAddr type_ptr = m.arg_ptr(3);
        const GuestAddr name_ptr = m.arg_ptr(4);
        const GuestAddr size_ptr = m.arg_ptr(5);
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        constexpr u32 required = sizeof(PROVIDER_NAME) * 2;
        (void)m.write_u32(size_ptr, required);
        if (type_ptr != 0) (void)m.write_u32(type_ptr, PROV_RSA_FULL);
        if (name_ptr == 0) {
            m.set_return(1);
            return;
        }
        if (capacity < required) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            m.set_return(0);
            return;
        }
        (void)m.write_wide_string(name_ptr, PROVIDER_NAME);
        m.set_return(1);
    };
    exports.entries.push_back(
        {"CryptEnumProvidersA", 0, enum_providers_a, 24});
    exports.entries.push_back(
        {"CryptEnumProvidersW", 0, enum_providers_w, 24});

    // BOOL CryptAcquireContext{A,W}(HCRYPTPROV*, LPCTSTR pszContainer,
    //   LPCTSTR pszProvider, DWORD dwProvType, DWORD dwFlags) — 20.
    // https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptacquirecontextw
    // tools/oracle_probe/advapi_capi_probe acq_*:
    //  - NULL phProv -> ERROR_INVALID_PARAMETER;
    //  - type 0 -> NTE_BAD_PROV_TYPE, a type with no CSP ->
    //    NTE_PROV_TYPE_NOT_DEF; a provider name nobody installed ->
    //    NTE_KEYSET_NOT_DEF, one of another type -> NTE_PROV_TYPE_NO_MATCH
    //    (these leave *phProv alone); NULL/"" name -> the type's default
    //    (Strong for PROV_RSA_FULL); names match case-insensitively and
    //    PP_NAME echoes the caller's spelling;
    //  - past that *phProv is zeroed; unknown flags -> NTE_BAD_FLAGS;
    //    CRYPT_VERIFYCONTEXT with a container name or
    //    CRYPT_DEFAULT_CONTAINER_OPTIONAL -> NTE_BAD_FLAGS, with
    //    CRYPT_DELETEKEYSET -> ERROR_INVALID_PARAMETER, with
    //    CRYPT_NEWKEYSET -> accepted;
    //  - containers: open missing / delete missing -> NTE_BAD_KEYSET,
    //    NEWKEYSET on an existing one -> NTE_EXISTS, DELETEKEYSET ->
    //    TRUE with *phProv NULL (last error left ERROR_NO_MORE_FILES);
    //  - success sets ERROR_SUCCESS.
    // DSS / Diffie-Hellman CSPs (types 3, 13, 18) are not implemented.
    auto acquire_context = [&stubs](Marshaler& m, bool wide) {
        constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
        constexpr u32 NTE_BAD_PROV_TYPE = 0x80090014u;
        constexpr u32 NTE_PROV_TYPE_NOT_DEF = 0x80090017u;
        constexpr u32 NTE_KEYSET_NOT_DEF = 0x80090019u;
        constexpr u32 NTE_PROV_TYPE_NO_MATCH = 0x8009001Bu;
        constexpr u32 NTE_BAD_KEYSET = 0x80090016u;
        constexpr u32 NTE_EXISTS = 0x8009000Fu;
        constexpr u32 CRYPT_VERIFYCONTEXT = 0xF0000000u;
        constexpr u32 CRYPT_NEWKEYSET = 0x00000008u;
        constexpr u32 CRYPT_DELETEKEYSET = 0x00000010u;
        constexpr u32 CRYPT_MACHINE_KEYSET = 0x00000020u;
        constexpr u32 CRYPT_SILENT = 0x00000040u;
        constexpr u32 CRYPT_DEFAULT_CONTAINER_OPTIONAL = 0x00000080u;
        constexpr u32 kKnownFlags = CRYPT_VERIFYCONTEXT | CRYPT_NEWKEYSET |
            CRYPT_DELETEKEYSET | CRYPT_MACHINE_KEYSET | CRYPT_SILENT |
            CRYPT_DEFAULT_CONTAINER_OPTIONAL;
        constexpr u32 ERROR_NO_MORE_FILES = 18;
        auto fail = [&m](u32 err) {
            set_last_error(err);
            m.set_return(0);
        };
        const GuestAddr ph_prov = m.arg_ptr(0);
        const GuestAddr container_ptr = m.arg_ptr(1);
        const GuestAddr provider_ptr = m.arg_ptr(2);
        const u32 provider_type = m.arg32(3);
        const u32 flags = m.arg32(4);
        if (ph_prov == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        if (provider_type == 0) return fail(NTE_BAD_PROV_TYPE);

        auto read_text = [&](GuestAddr p) -> std::string {
            if (p == 0) return {};
            return (wide ? m.read_wide_string(p) : m.read_ansi_string(p)).value_or("");
        };
        auto lower = [](std::string t) {
            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return t;
        };
        // Provider lookup by name, else the type's default.
        const std::string requested_name = read_text(provider_ptr);
        const CapiProviderInfo* info = nullptr;
        std::string name;
        if (!requested_name.empty()) {
            // Every CSP a Windows 10 box registers, with its type, so an
            // unimplemented one reports NTE_PROV_TYPE_NO_MATCH /
            // NTE_PROV_TYPE_NOT_DEF like a missing type would.
            static const std::pair<const char*, u32> kInstalled[] = {
                {"microsoft base cryptographic provider v1.0", 1},
                {"microsoft enhanced cryptographic provider v1.0", 1},
                {"microsoft strong cryptographic provider", 1},
                {"microsoft enhanced rsa and aes cryptographic provider", 24},
                {"microsoft rsa schannel cryptographic provider", 12},
                {"microsoft base dss cryptographic provider", 3},
                {"microsoft base dss and diffie-hellman cryptographic provider", 13},
                {"microsoft enhanced dss and diffie-hellman cryptographic provider", 13},
                {"microsoft dh schannel cryptographic provider", 18},
            };
            const std::string want = lower(requested_name);
            u32 installed_type = 0;
            for (const auto& [n, t] : kInstalled) {
                if (want == n) installed_type = t;
            }
            if (installed_type == 0) return fail(NTE_KEYSET_NOT_DEF);
            if (installed_type != provider_type) return fail(NTE_PROV_TYPE_NO_MATCH);
            for (const auto& p : kCapiProviders) {
                if (lower(p.name) == want) info = &p;
            }
            name = requested_name;
        } else {
            info = capi_default_provider(provider_type);
            if (info != nullptr) name = info->name;
        }
        if (info == nullptr) {
            if (provider_type == 3 || provider_type == 13 || provider_type == 18) {
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED advapi32!CryptAcquireContext: "
                                 "DSS / Diffie-Hellman CSP (type %u)\n", provider_type);
                }
            }
            return fail(NTE_PROV_TYPE_NOT_DEF);
        }

        (void)m.write_ptr(ph_prov, 0);
        if ((flags & ~kKnownFlags) != 0) return fail(NTE_BAD_FLAGS);
        const bool verify = (flags & CRYPT_VERIFYCONTEXT) == CRYPT_VERIFYCONTEXT;
        const bool machine = (flags & CRYPT_MACHINE_KEYSET) != 0;
        std::string container = read_text(container_ptr);
        if (verify) {
            if (container_ptr != 0 || (flags & CRYPT_DEFAULT_CONTAINER_OPTIONAL) != 0) {
                return fail(NTE_BAD_FLAGS);
            }
            if ((flags & CRYPT_DELETEKEYSET) != 0) {
                return fail(error_codes::ERROR_INVALID_PARAMETER);
            }
        }

        std::lock_guard<std::mutex> lock(s_state->mtx);
        std::string user;
        {
            u32 size = 256;
            (void)stubs.get_user_name_a(user, size);
        }
        if (!s_state->default_container_seeded) {
            s_state->containers[0].insert(user);
            s_state->default_container_seeded = true;
        }
        if (!verify) {
            if (container.empty()) container = user;
            auto& set = s_state->containers[machine ? 1 : 0];
            const bool exists = set.contains(container);
            if ((flags & CRYPT_DELETEKEYSET) != 0) {
                if (!exists) return fail(NTE_BAD_KEYSET);
                set.erase(container);
                set_last_error(ERROR_NO_MORE_FILES);
                m.set_return(1);   // *phProv stays NULL
                return;
            }
            if ((flags & CRYPT_NEWKEYSET) != 0) {
                if (exists) return fail(NTE_EXISTS);
                set.insert(container);
            } else if (!exists) {
                return fail(NTE_BAD_KEYSET);
            }
        }
        const u32 h = s_state->next_prov++;
        ProviderCtx provider_ctx;
        provider_ctx.type = provider_type;
        provider_ctx.flags = flags;
        provider_ctx.container = verify ? std::string{} : container;
        provider_ctx.info = info;
        provider_ctx.name = name;
        provider_ctx.verify = verify;
        s_state->providers.emplace(h, std::move(provider_ctx));
        (void)m.write_ptr(ph_prov, h);
        VITREO_TRACE("[advapi32] CryptAcquireContext%s(type=%u \"%s\" flags=0x%x) -> 0x%x\n",
                     wide ? "W" : "A", provider_type, name.c_str(), flags, h);
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"CryptAcquireContextA", 50,
        [acquire_context](Marshaler& m) {
            acquire_context(m, false);
        }, 20});
    exports.entries.push_back({"CryptAcquireContextW", 51,
        [acquire_context](Marshaler& m) {
            acquire_context(m, true);
        }, 20});

    // BOOL CryptReleaseContext(HCRYPTPROV, DWORD dwFlags) — 8. Probe
    // release_*: an unknown handle -> ERROR_INVALID_PARAMETER; non-zero
    // flags -> NTE_BAD_FLAGS but the reference is still released; each
    // CryptContextAddRef needs its own release.
    exports.entries.push_back({"CryptReleaseContext", 52,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 flags = m.arg32(1);
            std::lock_guard<std::mutex> lock(s_state->mtx);
            const auto it = s_state->providers.find(provider);
            if (it == s_state->providers.end()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (--it->second.refs == 0) s_state->providers.erase(it);
            if (flags != 0) {
                set_last_error(NTE_BAD_FLAGS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 8});

    // BOOL CryptContextAddRef(HCRYPTPROV, DWORD* pdwReserved, DWORD
    // dwFlags) — 12. Reserved must be NULL and flags 0, else
    // ERROR_INVALID_PARAMETER, as for an unknown handle (probe addref_*).
    exports.entries.push_back({"CryptContextAddRef", 0,
        [](Marshaler& m) {
            const u32 provider = static_cast<u32>(m.arg(0));
            const GuestAddr reserved = m.arg_ptr(1);
            const u32 flags = m.arg32(2);
            std::lock_guard<std::mutex> lock(s_state->mtx);
            const auto it = s_state->providers.find(provider);
            if (it == s_state->providers.end() || reserved != 0 || flags != 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            ++it->second.refs;
            m.set_return(1);
        }, 12});
}

} // namespace vitreo::winapi
