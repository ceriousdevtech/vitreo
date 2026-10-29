// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file crypto_internal.hpp
/// @brief State shared by the legacy advapi32 CryptoAPI exports.
///
/// Private to advapi32's security implementation; not a published surface.
/// The crypto_*_exports.cpp files each register one slice of the Crypt*
/// API and share this process-wide handle state (providers, hashes, keys)
/// through crypt_state(), plus the small helpers below. Moved out of the
/// function body they were local to when security_registration.cpp was split.

#include <vitreo/common.hpp>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <CommonCrypto/CommonDigest.h>

namespace vitreo::winapi::capi {

/// One provider of the legacy CryptoAPI catalogue (kCapiProviders in
/// crypto_provider_exports.cpp).
struct CapiProviderInfo {
    u32 type;
    const char* name;
    const char* enumalgs;     ///< PROV_ENUMALGS records, hex
    const char* enumalgs_ex;  ///< PROV_ENUMALGS_EX records, hex
};

struct HashCtx {
    u32 provider{0};
    u32 alg_id{0};
    std::variant<std::monostate,
                 CC_MD5_CTX, CC_SHA1_CTX, CC_SHA256_CTX, CC_SHA512_CTX,
                 CC_MD2_CTX, CC_MD4_CTX> ctx;
    std::vector<u8> finished_digest; // populated lazily on HP_HASHVAL
};
struct KeyCtx {
    u32 provider{0};
    u32 key_spec{0};
    u32 alg_id{0};
    u8 blob_type{0};
    u32 pubexp{0};
    bool exportable{false};
    std::vector<u8> modulus_be;  // big-endian modulus for RSA public keys
    std::vector<u8> prime1_be;
    std::vector<u8> prime2_be;
    std::vector<u8> exponent1_be;
    std::vector<u8> exponent2_be;
    std::vector<u8> coefficient_be;
    std::vector<u8> private_exponent_be;
    std::vector<u8> raw_key;     // PLAINTEXTKEYBLOB material (unused yet)
};
struct ProviderCtx {
    u32 type{0};
    u32 flags{0};
    std::string container;
    std::map<u32, KeyCtx> user_keys;
    const CapiProviderInfo* info{nullptr};
    std::string name;          ///< PP_NAME: as the caller spelled it
    u32 refs{1};               ///< CryptContextAddRef count
    bool verify{false};        ///< CRYPT_VERIFYCONTEXT
    usize enum_alg{0};         ///< PP_ENUMALGS cursor
    usize enum_alg_ex{0};      ///< PP_ENUMALGS_EX cursor
    usize enum_container{0};   ///< PP_ENUMCONTAINERS cursor
};
struct CryptState {
    std::mutex mtx;
    /// Key containers that exist, per keyset (0 user, 1 machine).
    /// The user's default container exists, as on the measured box
    /// (probe acq_default_container). Containers live for the
    /// process: key persistence across processes is not modelled.
    std::set<std::string> containers[2];
    bool default_container_seeded{false};
    std::map<u32, HashCtx> hashes;
    std::map<u32, ProviderCtx> providers;
    std::map<u32, KeyCtx> keys;
    u32 next_prov{0xCAFE0001u};
    u32 next_hash{0xBEEF0001u};
    u32 next_key{0xA11C0001u};
};

/// The process-wide CryptoAPI state (every Crypt* export reads and writes it).
[[nodiscard]] CryptState& crypt_state();

/// Decode a lowercase hex string; a trailing odd nibble is ignored.
[[nodiscard]] std::vector<u8> capi_hex(std::string_view hex);

/// Whether `alg` is one of the provider's PP_ENUMALGS algorithms.
[[nodiscard]] bool capi_alg_known(const CapiProviderInfo& p, u32 alg);

/// HP_HASHSIZE of every hash the catalogue offers.
[[nodiscard]] u32 capi_digest_length(u32 alg);

/// Finish into finished_digest; false when there is nothing to
/// finish (an SSL3_SHAMD5 hash whose value was never set).
[[nodiscard]] bool capi_hash_finish(HashCtx& hc);

} // namespace vitreo::winapi::capi
