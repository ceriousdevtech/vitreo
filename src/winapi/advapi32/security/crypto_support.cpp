// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_support.cpp
/// @brief Shared state and helpers for the legacy advapi32 CryptoAPI exports.
///
/// Definitions for crypto_internal.hpp. Registration order does not apply
/// here: nothing in this file registers an export.

#include "winapi/advapi32/security/crypto_internal.hpp"

#include <cstring>
#include <variant>
#include <vector>
#include <CommonCrypto/CommonDigest.h>

namespace vitreo::winapi::capi {

CryptState& crypt_state() {
    static CryptState* state = new CryptState();
    return *state;
}

[[nodiscard]] std::vector<u8> capi_hex(std::string_view hex) {
    std::vector<u8> out;
    auto nib = [](char c) -> u8 {
        return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    for (usize i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<u8>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

/// Whether `alg` is one of the provider's PP_ENUMALGS algorithms.
[[nodiscard]] bool capi_alg_known(const CapiProviderInfo& p, u32 alg) {
    const std::vector<u8> records = capi_hex(p.enumalgs);
    for (usize off = 0; off + 32 <= records.size(); off += 32) {
        u32 id = 0;
        std::memcpy(&id, records.data() + off, 4);
        if (id == alg) return true;
    }
    return false;
}

/// HP_HASHSIZE of every hash the catalogue offers.
[[nodiscard]] u32 capi_digest_length(u32 alg) {
    switch (alg) {
        case 0x8001: return CC_MD2_DIGEST_LENGTH;
        case 0x8002: return CC_MD4_DIGEST_LENGTH;
        case 0x8003: return CC_MD5_DIGEST_LENGTH;
        case 0x8004: return CC_SHA1_DIGEST_LENGTH;
        case 0x800c: return CC_SHA256_DIGEST_LENGTH;
        case 0x800d: return CC_SHA384_DIGEST_LENGTH;
        case 0x800e: return CC_SHA512_DIGEST_LENGTH;
        case 0x8008: return 36;   // CALG_SSL3_SHAMD5: MD5 || SHA-1 (probe ssl3_hashsize)
        default: return 0;
    }
}

/// Finish into finished_digest; false when there is nothing to
/// finish (an SSL3_SHAMD5 hash whose value was never set).
bool capi_hash_finish(HashCtx& hc) {
    if (!hc.finished_digest.empty()) return true;
    const u32 len = capi_digest_length(hc.alg_id);
    if (len == 0 || std::holds_alternative<std::monostate>(hc.ctx)) return false;
    hc.finished_digest.assign(len, 0);
    u8* out = hc.finished_digest.data();
    if (auto* c = std::get_if<CC_MD2_CTX>(&hc.ctx)) CC_MD2_Final(out, c);
    else if (auto* c = std::get_if<CC_MD4_CTX>(&hc.ctx)) CC_MD4_Final(out, c);
    else if (auto* c = std::get_if<CC_MD5_CTX>(&hc.ctx)) CC_MD5_Final(out, c);
    else if (auto* c = std::get_if<CC_SHA1_CTX>(&hc.ctx)) CC_SHA1_Final(out, c);
    else if (auto* c = std::get_if<CC_SHA256_CTX>(&hc.ctx)) CC_SHA256_Final(out, c);
    else if (auto* c = std::get_if<CC_SHA512_CTX>(&hc.ctx)) {
        if (hc.alg_id == 0x800d) CC_SHA384_Final(out, c);
        else CC_SHA512_Final(out, c);
    }
    return true;
}

} // namespace vitreo::winapi::capi
