// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_param_exports.cpp
/// @brief advapi32 legacy CryptoAPI parameters and key export: CryptGetProvParam,
/// CryptSetHashParam and CryptExportKey.
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
#include <string>
#include <vector>

namespace vitreo::winapi {

using namespace capi;

/// Registers CryptGetProvParam, CryptSetHashParam, CryptExportKey.
void add_crypto_param_exports(DllExports& exports) {
    static CryptState* s_state = &capi::crypt_state();
    // CryptGetHashParam dwParam value
    constexpr u32 HP_HASHVAL  = 0x0002;

    // BOOL CryptGetProvParam(HCRYPTPROV, DWORD dwParam, BYTE*, DWORD*,
    //   DWORD dwFlags) — 20. tools/oracle_probe/advapi_capi_probe:
    //   unknown provider -> ERROR_INVALID_PARAMETER; flags other than
    //   CRYPT_FIRST/NEXT/SGC_ENUM -> NTE_BAD_FLAGS; an unsupported
    //   dwParam (and PP_CONTAINER on a verify context) -> NTE_BAD_TYPE;
    //   NULL buffer -> TRUE with the size, a short one -> ERROR_MORE_DATA
    //   with the size; an enumeration past its end -> ERROR_NO_MORE_ITEMS.
    //   PP_NAME echoes the name the context was acquired with; PP_IMPTYPE
    //   is CRYPT_IMPL_SOFTWARE; PP_VERSION 2.0; PP_ENUMALGS(_EX) replay the
    //   measured records.
    exports.entries.push_back({"CryptGetProvParam", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_BAD_TYPE = 0x8009000Au;
            constexpr u32 PP_ENUMALGS = 1, PP_ENUMCONTAINERS = 2, PP_IMPTYPE = 3,
                          PP_NAME = 4, PP_VERSION = 5, PP_CONTAINER = 6,
                          PP_PROVTYPE = 16, PP_ENUMALGS_EX = 22,
                          PP_KEYSET_TYPE = 27, PP_UNIQUE_CONTAINER = 36,
                          PP_KEYSPEC = 39;
            constexpr u32 CRYPT_FIRST = 1, CRYPT_NEXT = 2, CRYPT_SGC_ENUM = 4;
            constexpr u32 CRYPT_MACHINE_KEYSET = 0x20;
            constexpr u32 CRYPT_IMPL_SOFTWARE = 2;
            constexpr u32 AT_KEYEXCHANGE = 1, AT_SIGNATURE = 2;
            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 param = m.arg32(1);
            const GuestAddr out = m.arg_ptr(2);
            const GuestAddr size_ptr = m.arg_ptr(3);
            const u32 flags = m.arg32(4);
            auto fail = [&m](u32 err) {
                set_last_error(err);
                m.set_return(0);
            };
            std::lock_guard<std::mutex> lock(s_state->mtx);
            const auto it = s_state->providers.find(provider);
            if (it == s_state->providers.end() || size_ptr == 0) {
                return fail(error_codes::ERROR_INVALID_PARAMETER);
            }
            if ((flags & ~(CRYPT_FIRST | CRYPT_NEXT | CRYPT_SGC_ENUM)) != 0) {
                return fail(NTE_BAD_FLAGS);
            }
            ProviderCtx& ctx = it->second;
            auto dword = [](u32 v) {
                std::vector<u8> r(4);
                std::memcpy(r.data(), &v, 4);
                return r;
            };
            auto text = [](const std::string& t) {
                std::vector<u8> r(t.begin(), t.end());
                r.push_back(0);
                return r;
            };
            std::vector<u8> result;
            usize* cursor = nullptr;
            switch (param) {
                case PP_NAME: result = text(ctx.name); break;
                case PP_IMPTYPE: result = dword(CRYPT_IMPL_SOFTWARE); break;
                case PP_VERSION: result = dword(0x200); break;
                case PP_PROVTYPE: result = dword(ctx.type); break;
                case PP_KEYSET_TYPE: result = dword(ctx.flags & CRYPT_MACHINE_KEYSET); break;
                case PP_KEYSPEC: result = dword(AT_KEYEXCHANGE | AT_SIGNATURE); break;
                case PP_CONTAINER:
                    if (ctx.verify) return fail(NTE_BAD_TYPE);
                    result = text(ctx.container);
                    break;
                case PP_ENUMALGS:
                case PP_ENUMALGS_EX: {
                    const bool ex = param == PP_ENUMALGS_EX;
                    const std::vector<u8> records =
                        capi_hex(ex ? ctx.info->enumalgs_ex : ctx.info->enumalgs);
                    const usize size = ex ? 88u : 32u;
                    cursor = ex ? &ctx.enum_alg_ex : &ctx.enum_alg;
                    if ((flags & CRYPT_FIRST) != 0) *cursor = 0;
                    const usize at = *cursor * size;
                    if (at + size > records.size()) return fail(error_codes::ERROR_NO_MORE_ITEMS);
                    result.assign(records.begin() + static_cast<std::ptrdiff_t>(at),
                                  records.begin() + static_cast<std::ptrdiff_t>(at + size));
                    break;
                }
                case PP_ENUMCONTAINERS: {
                    const auto& set = s_state->containers[(ctx.flags & CRYPT_MACHINE_KEYSET) ? 1 : 0];
                    cursor = &ctx.enum_container;
                    if ((flags & CRYPT_FIRST) != 0) *cursor = 0;
                    if (*cursor >= set.size()) return fail(error_codes::ERROR_NO_MORE_ITEMS);
                    auto name_it = set.begin();
                    std::advance(name_it, static_cast<std::ptrdiff_t>(*cursor));
                    result = text(*name_it);
                    break;
                }
                case PP_UNIQUE_CONTAINER: {
                    // md5(lowercased name + NUL) as DWORDs, "_", MachineGuid:
                    // the machine GUID is not modelled.
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true)) {
                        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED advapi32!CryptGetProvParam: "
                                     "PP_UNIQUE_CONTAINER\n");
                    }
                    return fail(NTE_BAD_TYPE);
                }
                default:
                    return fail(NTE_BAD_TYPE);
            }
            const u32 required = static_cast<u32>(result.size());
            const u32 capacity = m.read_u32(size_ptr).value_or(0);
            (void)m.write_u32(size_ptr, required);
            if (out == 0) {
                m.set_return(1);
                return;
            }
            if (capacity < required) return fail(error_codes::ERROR_MORE_DATA);
            (void)m.memory().write_bytes(out, result.data(), result.size());
            if (cursor != nullptr) ++*cursor;
            // The container enumeration clears the last error on success
            // (advapi_capi_probe PP_ENUMCONTAINERS_verify gle=0); the
            // other parameters leave it alone.
            if (param == PP_ENUMCONTAINERS) set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        }, 20});

    // CryptSetHashParam(HCRYPTHASH, DWORD, const BYTE*, DWORD)
    exports.entries.push_back({"CryptSetHashParam", 0,
        [HP_HASHVAL](Marshaler& m) {
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_BAD_HASH = 0x80090002u;
            constexpr u32 NTE_BAD_TYPE = 0x8009000Au;
            const u32 hash = static_cast<u32>(m.arg(0));
            const u32 param = static_cast<u32>(m.arg(1));
            const GuestAddr data = m.arg_ptr(2);
            const u32 flags = static_cast<u32>(m.arg(3));
            if (flags != 0) {
                set_last_error(NTE_BAD_FLAGS);
                m.set_return(0);
                return;
            }
            if (param != HP_HASHVAL) {
                set_last_error(NTE_BAD_TYPE);
                m.set_return(0);
                return;
            }

            std::lock_guard<std::mutex> lock(s_state->mtx);
            const auto it = s_state->hashes.find(hash);
            if (it == s_state->hashes.end()) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            const usize digest_size = capi_digest_length(it->second.alg_id);
            if (data == 0 || digest_size == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            it->second.finished_digest.resize(digest_size);
            if (m.memory().read_bytes(data,
                    it->second.finished_digest.data(), digest_size).is_err()) {
                it->second.finished_digest.clear();
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 16});

    // CryptExportKey(HCRYPTKEY, HCRYPTKEY, DWORD, DWORD, BYTE*, DWORD*)
    exports.entries.push_back({"CryptExportKey", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_KEY = 0x80090003u;
            constexpr u32 NTE_BAD_TYPE = 0x8009000Au;
            constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
            constexpr u32 NTE_BAD_KEY_STATE = 0x8009000Bu;
            constexpr u8 PUBLICKEYBLOB = 0x06;
            constexpr u8 PRIVATEKEYBLOB = 0x07;
            const u32 key = static_cast<u32>(m.arg(0));
            const u32 export_key = static_cast<u32>(m.arg(1));
            const u32 blob_type = static_cast<u32>(m.arg(2));
            const u32 flags = static_cast<u32>(m.arg(3));
            const GuestAddr out = m.arg_ptr(4);
            const GuestAddr size_ptr = m.arg_ptr(5);
            if (size_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (flags != 0) {
                set_last_error(NTE_BAD_FLAGS);
                m.set_return(0);
                return;
            }
            // PLAINTEXTKEYBLOB (8) exports a *session* key: an 8-byte
            // BLOBHEADER, a DWORD length, then the raw key -- 28 bytes for
            // a 128-bit RC4 key, matching Windows exactly. A key derived
            // without CRYPT_EXPORTABLE reports NTE_BAD_KEY_STATE rather
            // than NTE_BAD_TYPE; callers use the distinction to tell "wrong
            // blob kind" from "this key may not leave the provider".
            constexpr u8 PLAINTEXTKEYBLOB = 8;
            if (blob_type == PLAINTEXTKEYBLOB) {
                std::vector<u8> raw;
                u32 alg = 0;
                bool exportable = false;
                {
                    std::lock_guard<std::mutex> lock(s_state->mtx);
                    const auto it = s_state->keys.find(key);
                    if (it == s_state->keys.end() || it->second.raw_key.empty()) {
                        set_last_error(NTE_BAD_KEY);
                        m.set_return(0);
                        return;
                    }
                    raw = it->second.raw_key;
                    alg = it->second.alg_id;
                    exportable = it->second.exportable;
                }
                if (!exportable) {
                    set_last_error(NTE_BAD_KEY_STATE);
                    m.set_return(0);
                    return;
                }
                std::vector<u8> blob;
                blob.push_back(PLAINTEXTKEYBLOB);      // bType
                blob.push_back(2);                     // bVersion (CUR_BLOB_VERSION)
                blob.push_back(0); blob.push_back(0);  // reserved
                for (u32 sh = 0; sh < 32; sh += 8)
                    blob.push_back(static_cast<u8>(alg >> sh));
                const u32 klen = static_cast<u32>(raw.size());
                for (u32 sh = 0; sh < 32; sh += 8)
                    blob.push_back(static_cast<u8>(klen >> sh));
                blob.insert(blob.end(), raw.begin(), raw.end());

                const u32 cb = m.read_u32(size_ptr).value_or(0);
                (void)m.write_u32(size_ptr, static_cast<u32>(blob.size()));
                if (out == 0) {
                    m.set_return(1);
                    return;
                }
                if (cb < blob.size()) {
                    set_last_error(error_codes::ERROR_MORE_DATA);
                    m.set_return(0);
                    return;
                }
                (void)m.memory().write_bytes(out,
                    reinterpret_cast<const Byte*>(blob.data()), blob.size());
                m.set_return(1);
                return;
            }
            if (blob_type != PUBLICKEYBLOB && blob_type != PRIVATEKEYBLOB) {
                set_last_error(NTE_BAD_TYPE);
                m.set_return(0);
                return;
            }
            if (export_key != 0) {
                set_last_error(NTE_BAD_KEY);
                m.set_return(0);
                return;
            }

            KeyCtx key_ctx;
            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                const auto it = s_state->keys.find(key);
                if (it == s_state->keys.end() || it->second.modulus_be.empty()) {
                    set_last_error(NTE_BAD_KEY);
                    m.set_return(0);
                    return;
                }
                key_ctx = it->second;
            }
            if (blob_type == PRIVATEKEYBLOB &&
                (!key_ctx.exportable || key_ctx.private_exponent_be.empty())) {
                set_last_error(NTE_BAD_KEY_STATE);
                m.set_return(0);
                return;
            }

            auto append_u32 = [](std::vector<u8>& bytes, u32 value) {
                for (u32 shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<u8>(value >> shift));
            };
            auto append_le = [](std::vector<u8>& bytes,
                                const std::vector<u8>& be) {
                bytes.insert(bytes.end(), be.rbegin(), be.rend());
            };
            std::vector<u8> blob;
            blob.reserve(20 + key_ctx.modulus_be.size() * 5);
            blob.push_back(static_cast<u8>(blob_type));
            blob.push_back(2);
            blob.push_back(0);
            blob.push_back(0);
            append_u32(blob, key_ctx.alg_id);
            append_u32(blob, blob_type == PUBLICKEYBLOB
                ? 0x31415352u : 0x32415352u);
            append_u32(blob,
                static_cast<u32>(key_ctx.modulus_be.size() * 8));
            append_u32(blob, key_ctx.pubexp);
            append_le(blob, key_ctx.modulus_be);
            if (blob_type == PRIVATEKEYBLOB) {
                append_le(blob, key_ctx.prime1_be);
                append_le(blob, key_ctx.prime2_be);
                append_le(blob, key_ctx.exponent1_be);
                append_le(blob, key_ctx.exponent2_be);
                append_le(blob, key_ctx.coefficient_be);
                append_le(blob, key_ctx.private_exponent_be);
            }

            const u32 required = static_cast<u32>(blob.size());
            const u32 capacity = m.read_u32(size_ptr).value_or(0);
            if (m.write_u32(size_ptr, required).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            if (out == 0) {
                m.set_return(1);
                return;
            }
            if (capacity < required) {
                set_last_error(error_codes::ERROR_MORE_DATA);
                m.set_return(0);
                return;
            }
            if (m.memory().write_bytes(out, blob.data(), blob.size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 24});
}

} // namespace vitreo::winapi
