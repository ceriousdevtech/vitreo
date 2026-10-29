// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file bcrypt.hpp
/// @brief bcrypt.dll API stubs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <variant>

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// BcryptManager
// =============================================================================

class BcryptManager {
public:
    enum class AlgType : u32 {
        SHA256 = 0,
        SHA1 = 1,
        MD5 = 2,
        AES = 3,
        RNG = 4,
    };

    static constexpr u32 kAlgHandleBase  = 0x1000;
    static constexpr u32 kHashHandleBase = 0x2000;
    static constexpr u32 kKeyHandleBase  = 0x3000;

    enum class ChainingMode { Cbc, Ecb };

    struct AlgorithmContext {
        AlgType alg;
        bool is_hmac = false;
        ChainingMode chaining_mode = ChainingMode::Cbc;
    };

    struct HashContext {
        AlgType alg;
        bool is_hmac = false;
        std::variant<CC_SHA256_CTX, CC_SHA1_CTX, CC_MD5_CTX, CCHmacContext> ctx;
    };

    struct KeyContext {
        std::vector<u8> key_data;
        ChainingMode chaining_mode = ChainingMode::Cbc;
        u32 block_size = 16; // AES block size
    };

    BcryptManager() = default;
    ~BcryptManager() = default;

    BcryptManager(const BcryptManager&) = delete;
    BcryptManager& operator=(const BcryptManager&) = delete;

    [[nodiscard]] u32 bcrypt_gen_random(Marshaler& m, u32 algorithm,
                                        GuestAddr buffer, u32 count, u32 flags);

    [[nodiscard]] u32 bcrypt_open_algorithm_provider(Marshaler& m,
                                                      GuestAddr ph_algorithm,
                                                      GuestAddr psz_alg_id,
                                                      GuestAddr psz_impl,
                                                      u32 flags);

    [[nodiscard]] u32 bcrypt_close_algorithm_provider(u32 h_algorithm, u32 flags);

    [[nodiscard]] u32 bcrypt_get_property(Marshaler& m, u32 h_object,
                                           GuestAddr psz_property,
                                           GuestAddr pb_output, u32 cb_output,
                                           GuestAddr pcb_result, u32 flags);

    [[nodiscard]] u32 bcrypt_set_property(Marshaler& m, u32 h_object,
                                           GuestAddr psz_property,
                                           GuestAddr pb_input, u32 cb_input,
                                           u32 flags);

    [[nodiscard]] u32 bcrypt_hash(Marshaler& m, u64 h_algorithm,
                                 GuestAddr pb_secret, u32 cb_secret,
                                 GuestAddr pb_input, u32 cb_input,
                                 GuestAddr pb_output, u32 cb_output);

    [[nodiscard]] u32 bcrypt_create_hash(Marshaler& m, u32 h_algorithm,
                                          GuestAddr ph_hash,
                                          GuestAddr pb_hash_object,
                                          u32 cb_hash_object,
                                          GuestAddr pb_secret, u32 cb_secret,
                                          u32 flags);

    [[nodiscard]] u32 bcrypt_hash_data(Marshaler& m, u32 h_hash,
                                        GuestAddr pb_input, u32 cb_input,
                                        u32 flags);

    [[nodiscard]] u32 bcrypt_finish_hash(Marshaler& m, u32 h_hash,
                                          GuestAddr pb_output, u32 cb_output,
                                          u32 flags);

    [[nodiscard]] u32 bcrypt_destroy_hash(u32 h_hash);

    // AES symmetric key operations
    [[nodiscard]] u32 bcrypt_generate_symmetric_key(Marshaler& m, u32 h_algorithm,
                                                     GuestAddr ph_key,
                                                     GuestAddr pb_key_object,
                                                     u32 cb_key_object,
                                                     GuestAddr pb_secret,
                                                     u32 cb_secret, u32 flags);

    [[nodiscard]] u32 bcrypt_encrypt(Marshaler& m, u32 h_key,
                                      GuestAddr pb_input, u32 cb_input,
                                      GuestAddr pb_padding_info,
                                      GuestAddr pb_iv, u32 cb_iv,
                                      GuestAddr pb_output, u32 cb_output,
                                      GuestAddr pcb_result, u32 flags);

    [[nodiscard]] u32 bcrypt_decrypt(Marshaler& m, u32 h_key,
                                      GuestAddr pb_input, u32 cb_input,
                                      GuestAddr pb_padding_info,
                                      GuestAddr pb_iv, u32 cb_iv,
                                      GuestAddr pb_output, u32 cb_output,
                                      GuestAddr pcb_result, u32 flags);

    [[nodiscard]] u32 bcrypt_destroy_key(u32 h_key);

private:
    u32 next_alg_handle_  = kAlgHandleBase;
    u32 next_hash_handle_ = kHashHandleBase;
    u32 next_key_handle_  = kKeyHandleBase;
    mutable std::recursive_mutex mutex_;
    std::unordered_map<u32, AlgorithmContext> algorithm_contexts_;
    std::unordered_map<u32, HashContext> hash_contexts_;
    std::unordered_map<u32, KeyContext> key_contexts_;

    static u32 digest_length(AlgType alg);
    [[nodiscard]] std::optional<AlgorithmContext> lookup_alg(u32 handle) const;
    [[nodiscard]] bool is_valid_alg_handle(u32 handle) const;
    [[nodiscard]] AlgType alg_from_handle(u32 handle) const;
};

/// Create bcrypt.dll export thunks
DllExports create_bcrypt_thunks(BcryptManager& mgr);

} // namespace vitreo::winapi
