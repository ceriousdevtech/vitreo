// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_hash_exports.cpp
/// @brief advapi32 legacy CryptoAPI hashes: CryptCreateHash, CryptHashData,
/// CryptGetHashParam, CryptDestroyHash and CryptDestroyKey.
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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>
#include <CommonCrypto/CommonDigest.h>

namespace vitreo::winapi {

using namespace capi;

/// Registers CryptCreateHash, CryptHashData, CryptGetHashParam,
/// CryptDestroyHash, CryptDestroyKey.
void add_crypto_hash_exports(DllExports& exports) {
    static CryptState* s_state = &capi::crypt_state();
    // CryptGetHashParam dwParam values
    constexpr u32 HP_ALGID    = 0x0001;
    constexpr u32 HP_HASHVAL  = 0x0002;
    constexpr u32 HP_HASHSIZE = 0x0004;

    // Digest plumbing shared by the hash entry points.
    auto hash_init = [](HashCtx& hc) {
        switch (hc.alg_id) {
            case 0x8001: { CC_MD2_CTX c; CC_MD2_Init(&c); hc.ctx = c; break; }
            case 0x8002: { CC_MD4_CTX c; CC_MD4_Init(&c); hc.ctx = c; break; }
            case 0x8003: { CC_MD5_CTX c; CC_MD5_Init(&c); hc.ctx = c; break; }
            case 0x8004: { CC_SHA1_CTX c; CC_SHA1_Init(&c); hc.ctx = c; break; }
            case 0x800c: { CC_SHA256_CTX c; CC_SHA256_Init(&c); hc.ctx = c; break; }
            case 0x800d: { CC_SHA512_CTX c; CC_SHA384_Init(&c); hc.ctx = c; break; }
            case 0x800e: { CC_SHA512_CTX c; CC_SHA512_Init(&c); hc.ctx = c; break; }
            default: hc.ctx = std::monostate{}; break;   // SSL3_SHAMD5: set, not hashed
        }
    };
    auto hash_update = [](HashCtx& hc, const u8* data, u32 len) {
        if (auto* c = std::get_if<CC_MD2_CTX>(&hc.ctx)) CC_MD2_Update(c, data, len);
        else if (auto* c = std::get_if<CC_MD4_CTX>(&hc.ctx)) CC_MD4_Update(c, data, len);
        else if (auto* c = std::get_if<CC_MD5_CTX>(&hc.ctx)) CC_MD5_Update(c, data, len);
        else if (auto* c = std::get_if<CC_SHA1_CTX>(&hc.ctx)) CC_SHA1_Update(c, data, len);
        else if (auto* c = std::get_if<CC_SHA256_CTX>(&hc.ctx)) CC_SHA256_Update(c, data, len);
        else if (auto* c = std::get_if<CC_SHA512_CTX>(&hc.ctx)) {
            if (hc.alg_id == 0x800d) CC_SHA384_Update(c, data, len);
            else CC_SHA512_Update(c, data, len);
        }
    };
    // Shared with finalize_hash (crypto_signature_exports.cpp); defined in
    // crypto_support.cpp.
    const auto hash_finish = &capi::capi_hash_finish;

    // BOOL CryptCreateHash(HCRYPTPROV, ALG_ID, HCRYPTKEY, DWORD dwFlags,
    //   HCRYPTHASH*) — 20. Probe hash_*: unknown provider / key handle or
    //   NULL out -> ERROR_INVALID_PARAMETER; flags -> NTE_BAD_FLAGS; an
    //   ALG_ID the CSP does not list -> NTE_BAD_ALGID, one it lists that
    //   is not a hash -> ERROR_NOT_SUPPORTED; MAC/HMAC without a key ->
    //   NTE_BAD_KEY. SHA-2 needs PROV_RSA_AES. Success: last error kept.
    exports.entries.push_back({"CryptCreateHash", 53,
        [hash_init](Marshaler& m) {
            constexpr u32 NTE_BAD_ALGID = 0x80090008u;
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_BAD_KEY = 0x80090003u;
            constexpr u32 CALG_MAC = 0x8005, CALG_HMAC = 0x8009;
            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 alg = m.arg32(1);
            const u32 h_key = static_cast<u32>(m.arg(2));
            const u32 flags = m.arg32(3);
            const GuestAddr ph_hash = m.arg_ptr(4);
            auto fail = [&m](u32 err) {
                set_last_error(err);
                m.set_return(0);
            };
            std::lock_guard<std::mutex> lock(s_state->mtx);
            const auto prov = s_state->providers.find(provider);
            if (prov == s_state->providers.end() || ph_hash == 0) {
                return fail(error_codes::ERROR_INVALID_PARAMETER);
            }
            if (h_key != 0 && !s_state->keys.contains(h_key)) {
                return fail(error_codes::ERROR_INVALID_PARAMETER);
            }
            if (flags != 0) return fail(NTE_BAD_FLAGS);
            const CapiProviderInfo* info = prov->second.info;
            if (info == nullptr || !capi_alg_known(*info, alg)) return fail(NTE_BAD_ALGID);
            if ((alg & 0xE000u) != 0x8000u) return fail(error_codes::ERROR_NOT_SUPPORTED);
            if (alg == CALG_MAC || alg == CALG_HMAC) {
                if (h_key == 0) return fail(NTE_BAD_KEY);
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED advapi32!CryptCreateHash: "
                                 "keyed %s\n", alg == CALG_HMAC ? "CALG_HMAC" : "CALG_MAC");
                }
                return fail(NTE_BAD_ALGID);
            }
            if (h_key != 0) return fail(NTE_BAD_KEY);
            HashCtx hc;
            hc.provider = provider;
            hc.alg_id = alg;
            hash_init(hc);
            const u32 h = s_state->next_hash++;
            s_state->hashes.emplace(h, std::move(hc));
            (void)m.write_ptr(ph_hash, h);
            m.set_return(1);
        }, 20});

    // BOOL CryptHashData(HCRYPTHASH, const BYTE*, DWORD, DWORD dwFlags)
    // — 16. Probe hashdata_*: unknown hash -> ERROR_INVALID_PARAMETER;
    // flags other than CRYPT_USERDATA -> NTE_BAD_FLAGS; after the value
    // was read or set -> NTE_BAD_HASH_STATE; an SSL3_SHAMD5 hash is never
    // hashed into (ERROR_INVALID_HANDLE, probe ssl3_hashdata).
    exports.entries.push_back({"CryptHashData", 54,
        [hash_update](Marshaler& m) {
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_BAD_HASH_STATE = 0x8009000Cu;
            constexpr u32 CRYPT_USERDATA = 1;
            const u32 h_hash = static_cast<u32>(m.arg(0));
            const GuestAddr buf_ptr = m.arg_ptr(1);
            const u32 buf_len = m.arg32(2);
            const u32 flags = m.arg32(3);
            auto fail = [&m](u32 err) {
                set_last_error(err);
                m.set_return(0);
            };
            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto it = s_state->hashes.find(h_hash);
            if (it == s_state->hashes.end()) return fail(error_codes::ERROR_INVALID_PARAMETER);
            if ((flags & ~CRYPT_USERDATA) != 0) return fail(NTE_BAD_FLAGS);
            auto& hc = it->second;
            if (!hc.finished_digest.empty()) return fail(NTE_BAD_HASH_STATE);
            if (hc.alg_id == 0x8008) return fail(error_codes::ERROR_INVALID_HANDLE);
            if (buf_len == 0) {
                m.set_return(1);
                return;
            }
            std::vector<u8> bytes(buf_len);
            if (m.memory().read_bytes(buf_ptr, bytes.data(), buf_len).is_err()) {
                return fail(error_codes::ERROR_NOACCESS);
            }
            hash_update(hc, bytes.data(), buf_len);
            m.set_return(1);
        }, 16});

    // BOOL CryptGetHashParam(HCRYPTHASH, DWORD dwParam, BYTE*, DWORD*,
    //   DWORD dwFlags) — 20. Probe gethash_*: unknown hash ->
    //   ERROR_INVALID_PARAMETER; flags -> NTE_BAD_FLAGS; an unknown
    //   dwParam -> NTE_BAD_TYPE; NULL buffer -> TRUE with the size; a short
    //   buffer -> ERROR_MORE_DATA with the size; reading HP_HASHVAL
    //   finishes the hash; an unset SSL3_SHAMD5 value -> NTE_BAD_HASH_STATE.
    exports.entries.push_back({"CryptGetHashParam", 55,
        [HP_ALGID, HP_HASHVAL, HP_HASHSIZE, hash_finish]
        (Marshaler& m) {
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_BAD_TYPE = 0x8009000Au;
            constexpr u32 NTE_BAD_HASH_STATE = 0x8009000Cu;
            const u32 h_hash = static_cast<u32>(m.arg(0));
            const u32 param = m.arg32(1);
            const GuestAddr out = m.arg_ptr(2);
            const GuestAddr cb_ptr = m.arg_ptr(3);
            const u32 flags = m.arg32(4);
            auto fail = [&m](u32 err) {
                set_last_error(err);
                m.set_return(0);
            };
            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto it = s_state->hashes.find(h_hash);
            if (it == s_state->hashes.end() || cb_ptr == 0) {
                return fail(error_codes::ERROR_INVALID_PARAMETER);
            }
            if (flags != 0) return fail(NTE_BAD_FLAGS);
            auto& hc = it->second;
            std::vector<u8> value;
            if (param == HP_ALGID || param == HP_HASHSIZE) {
                const u32 v = param == HP_ALGID ? hc.alg_id : capi_digest_length(hc.alg_id);
                value.resize(4);
                std::memcpy(value.data(), &v, 4);
            } else if (param == HP_HASHVAL) {
                const u32 len = capi_digest_length(hc.alg_id);
                if (out != 0 && m.read_u32(cb_ptr).value_or(0) >= len) {
                    if (!hash_finish(hc)) return fail(NTE_BAD_HASH_STATE);
                    value = hc.finished_digest;
                } else {
                    value.resize(len);   // size query / short buffer only
                }
            } else {
                return fail(NTE_BAD_TYPE);
            }
            const u32 need = static_cast<u32>(value.size());
            const u32 have = m.read_u32(cb_ptr).value_or(0);
            (void)m.write_u32(cb_ptr, need);
            if (out == 0) {
                m.set_return(1);
                return;
            }
            if (have < need) return fail(error_codes::ERROR_MORE_DATA);
            (void)m.memory().write_bytes(out, value.data(), value.size());
            m.set_return(1);
        }, 20});

    // BOOL CryptDestroyHash(HCRYPTHASH) — 4. An unknown or already
    // destroyed handle -> ERROR_INVALID_PARAMETER (probe destroy_*).
    exports.entries.push_back({"CryptDestroyHash", 56,
        [](Marshaler& m) {
            const u32 h = static_cast<u32>(m.arg(0));
            std::lock_guard<std::mutex> lock(s_state->mtx);
            if (s_state->hashes.erase(h) == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4});

    // CryptDestroyKey(HCRYPTKEY) — 1 arg, stdcall=4
    //
    // Callers resolve this together with CryptAcquireContextA,
    // CryptCreateHash, and CryptDestroyHash, so the export must preserve
    // both handle lifetime and the x86 stdcall ABI.
    //
    // Windows returns FALSE with NTE_BAD_KEY for a handle the provider does
    // not own.
    exports.entries.push_back({"CryptDestroyKey", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_KEY = 0x80090003u;
            u32 h = static_cast<u32>(m.arg(0));
            if (h == 0) {
                // Windows reports a handle it does not recognise as
                // ERROR_INVALID_PARAMETER, not NTE_BAD_KEY.
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            std::lock_guard<std::mutex> lock(s_state->mtx);
            if (s_state->keys.erase(h) == 0) {
                VITREO_TRACE("[advapi32] CryptDestroyKey(0x%x): unknown key"
                             " -> FALSE NTE_BAD_KEY\n", h);
                // Windows reports a handle it does not recognise as
                // ERROR_INVALID_PARAMETER, not NTE_BAD_KEY.
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4});
}

} // namespace vitreo::winapi
