// SPDX-License-Identifier: LGPL-2.1-only
/// @file bcrypt.cpp
/// @brief Implementation of bcrypt.dll API stubs

#include "bcrypt.hpp"
#include "winapi/core/marshaler.hpp"

#include <cctype>
#include <atomic>
#include <cstdlib>    // arc4random_buf
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace vitreo::winapi {

static constexpr u32 STATUS_SUCCESS           = 0;
static constexpr u32 STATUS_NOT_FOUND         = 0xC0000225;
static constexpr u32 STATUS_INVALID_HANDLE    = 0xC0000008;
static constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000D;
static constexpr u32 STATUS_BUFFER_TOO_SMALL  = 0xC0000023;
static constexpr u32 STATUS_UNSUCCESSFUL      = 0xC0000001;


#include "property_constants.inc"
#include "hash_contract.inc"

// =============================================================================
// Helpers
// =============================================================================

u32 BcryptManager::digest_length(AlgType alg) {
    switch (alg) {
        case AlgType::SHA256: return CC_SHA256_DIGEST_LENGTH; // 32
        case AlgType::SHA1:   return CC_SHA1_DIGEST_LENGTH;   // 20
        case AlgType::MD5:    return CC_MD5_DIGEST_LENGTH;    // 16
        case AlgType::AES:
        case AlgType::RNG:
            return 0;
    }
    return 0;
}

std::optional<BcryptManager::AlgorithmContext>
BcryptManager::lookup_alg(u32 handle) const {
    if (auto it = algorithm_contexts_.find(handle); it != algorithm_contexts_.end()) {
        return it->second;
    }
    // CNG algorithm pseudo-handles (bcrypt.h BCRYPT_*_ALG_HANDLE, Windows 10+):
    // "You can use these handles in any situation that requires an
    // algorithm handle" (Learn "CNG Algorithm Pseudo-handles"). Only the
    // algorithms this provider implements are recognised.
    switch (handle) {
    case 0x21: return AlgorithmContext{AlgType::MD5, false};
    case 0x31: return AlgorithmContext{AlgType::SHA1, false};
    case 0x41: return AlgorithmContext{AlgType::SHA256, false};
    case 0x81: return AlgorithmContext{AlgType::RNG, false};
    case 0x91: return AlgorithmContext{AlgType::MD5, true};
    case 0xa1: return AlgorithmContext{AlgType::SHA1, true};
    case 0xb1: return AlgorithmContext{AlgType::SHA256, true};
    default: return std::nullopt;
    }
}

bool BcryptManager::is_valid_alg_handle(u32 handle) const {
    return lookup_alg(handle).has_value();
}

BcryptManager::AlgType BcryptManager::alg_from_handle(u32 handle) const {
    return lookup_alg(handle)->alg;
}

// =============================================================================
// BcryptManager Implementation
// =============================================================================

u32 BcryptManager::bcrypt_gen_random(Marshaler& m, u32 algorithm,
                                      GuestAddr buffer, u32 count, u32 flags) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    constexpr u32 BCRYPT_RNG_USE_ENTROPY_IN_BUFFER = 0x00000001u;
    constexpr u32 BCRYPT_USE_SYSTEM_PREFERRED_RNG = 0x00000002u;
    constexpr u32 supported_flags =
        BCRYPT_RNG_USE_ENTROPY_IN_BUFFER |
        BCRYPT_USE_SYSTEM_PREFERRED_RNG;

    VITREO_TRACE(
        "[bcrypt] BCryptGenRandom(alg=0x%x, buf=0x%llx, cb=%u, "
        "flags=0x%x)\n",
        algorithm, static_cast<unsigned long long>(buffer), count, flags);
    if ((flags & ~supported_flags) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        if (algorithm != 0) return STATUS_INVALID_PARAMETER;
    } else if (!is_valid_alg_handle(algorithm) ||
               alg_from_handle(algorithm) != AlgType::RNG) {
        return STATUS_INVALID_HANDLE;
    }
    if (count == 0) {
        return STATUS_SUCCESS;
    }
    if (buffer == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    std::vector<u8> data(count);
    arc4random_buf(data.data(), count);
    if (m.memory().write_bytes(
            buffer, reinterpret_cast<const Byte*>(data.data()), count)
            .is_err()) {
        return STATUS_INVALID_PARAMETER;
    }
    VITREO_TRACE("[bcrypt] BCryptGenRandom -> STATUS_SUCCESS\n");
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_open_algorithm_provider(Marshaler& m,
                                                    GuestAddr ph_algorithm,
                                                    GuestAddr psz_alg_id,
                                                    GuestAddr psz_impl,
                                                    u32 flags) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!ph_algorithm || !psz_alg_id)
        return STATUS_INVALID_PARAMETER;

    // pszImplementation (Learn: "the registered alias of the cryptographic
    // primitive provider"; NULL selects the default). Only the primitive
    // provider exists; NULL, "" and its name (any case) select it, anything
    // else is STATUS_NOT_FOUND with *phAlgorithm untouched
    // (tools/oracle_probe/sspi_bcrypt_probe.expected).
    if (psz_impl != 0) {
        const std::string impl = m.read_wide_string(psz_impl).value_or("");
        constexpr std::string_view kPrimitive = "Microsoft Primitive Provider";
        bool primitive = impl.empty();
        if (!primitive && impl.size() == kPrimitive.size()) {
            primitive = true;
            for (usize i = 0; i < impl.size() && primitive; ++i)
                primitive = std::tolower(static_cast<unsigned char>(impl[i])) ==
                            std::tolower(static_cast<unsigned char>(kPrimitive[i]));
        }
        if (!primitive) return STATUS_NOT_FOUND;
    }

    auto alg_name = m.read_wide_string(psz_alg_id);
    if (!alg_name.is_ok())
        return STATUS_INVALID_PARAMETER;

    AlgType alg;
    const auto& name = alg_name.value();
    if (name == "SHA256")       alg = AlgType::SHA256;
    else if (name == "SHA1")    alg = AlgType::SHA1;
    else if (name == "MD5")     alg = AlgType::MD5;
    else if (name == "AES")     alg = AlgType::AES;
    else if (name == "RNG")     alg = AlgType::RNG;
    else                        return STATUS_NOT_FOUND;

    constexpr u32 BCRYPT_ALG_HANDLE_HMAC_FLAG = 0x00000008;
    const bool is_hmac = (flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;
    if (is_hmac && alg != AlgType::SHA256 &&
        alg != AlgType::SHA1 && alg != AlgType::MD5) {
        return STATUS_INVALID_PARAMETER;
    }

    const u32 handle = next_alg_handle_++;
    algorithm_contexts_.emplace(handle, AlgorithmContext{alg, is_hmac});
    if (m.write_ptr(ph_algorithm, handle).is_err()) {
        algorithm_contexts_.erase(handle);
        return STATUS_INVALID_PARAMETER;
    }
    VITREO_TRACE(
        "[bcrypt] BCryptOpenAlgorithmProvider(\"%s\", flags=0x%x) "
        "-> 0x%x\n",
        name.c_str(), flags, handle);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_close_algorithm_provider(u32 h_algorithm, u32 flags) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (flags != 0) return STATUS_INVALID_PARAMETER;
    if (algorithm_contexts_.erase(h_algorithm) == 0)
        return STATUS_INVALID_HANDLE;
    VITREO_TRACE(
        "[bcrypt] BCryptCloseAlgorithmProvider(0x%x) -> STATUS_SUCCESS\n",
        h_algorithm);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_get_property(Marshaler& m, u32 h_object,
                                         GuestAddr psz_property,
                                         GuestAddr pb_output, u32 cb_output,
                                         GuestAddr pcb_result, u32 flags) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!psz_property)
        return STATUS_INVALID_PARAMETER;

    auto prop_name = m.read_wide_string(psz_property);
    if (!prop_name.is_ok())
        return STATUS_INVALID_PARAMETER;

    if (prop_name.value() == BCRYPT_CHAINING_MODE) {
        if (flags || !pcb_result) return STATUS_INVALID_PARAMETER;
        auto key = key_contexts_.find(h_object);
        auto algorithm = lookup_alg(h_object);
        if (key == key_contexts_.end() && (!algorithm || algorithm->alg != AlgType::AES))
            return STATUS_INVALID_HANDLE;
        const auto mode = key != key_contexts_.end() ? key->second.chaining_mode : algorithm->chaining_mode;
        const auto value = mode == ChainingMode::Cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
        const u32 bytes = static_cast<u32>((value.size() + 1) * sizeof(u16));
        if (m.memory().write_u32(pcb_result, bytes).is_err()) return STATUS_INVALID_PARAMETER;
        if (!pb_output) return STATUS_SUCCESS;
        if (cb_output < bytes) return STATUS_BUFFER_TOO_SMALL;
        return m.write_wide_string(pb_output, std::string(value)).is_ok()
            ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    if (!is_valid_alg_handle(h_object))
        return STATUS_INVALID_HANDLE;

    AlgType alg = alg_from_handle(h_object);

    if (prop_name.value() == "HashDigestLength") {
        if (alg == AlgType::AES || alg == AlgType::RNG)
            return STATUS_NOT_FOUND;
        u32 len = digest_length(alg);

        if (pcb_result)
            (void)m.memory().write_u32(pcb_result, 4);

        if (pb_output && cb_output >= 4)
            (void)m.memory().write_u32(pb_output, len);

        return STATUS_SUCCESS;
    }

    if (prop_name.value() == "ObjectLength") {
        // Return internal object size needed for hash/key objects
        if (alg == AlgType::RNG) return STATUS_NOT_FOUND;
        u32 obj_len = (alg == AlgType::AES) ? 512 : 256;
        if (pcb_result) (void)m.memory().write_u32(pcb_result, 4);
        if (pb_output && cb_output >= 4)
            (void)m.memory().write_u32(pb_output, obj_len);
        return STATUS_SUCCESS;
    }

    if (prop_name.value() == "BlockLength") {
        if (alg == AlgType::RNG) return STATUS_NOT_FOUND;
        u32 block = (alg == AlgType::AES) ? 16 : 1;
        if (pcb_result) (void)m.memory().write_u32(pcb_result, 4);
        if (pb_output && cb_output >= 4)
            (void)m.memory().write_u32(pb_output, block);
        return STATUS_SUCCESS;
    }

    if (prop_name.value() == "KeyLength") {
        // Default AES key length in bits
        if (alg != AlgType::AES) return STATUS_NOT_FOUND;
        if (pcb_result) (void)m.memory().write_u32(pcb_result, 4);
        if (pb_output && cb_output >= 4)
            (void)m.memory().write_u32(pb_output, 256); // AES-256
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_FOUND;
}

// Learn BCryptSetProperty and cng-property-identifiers; validation and
// snapshot semantics measured in bcrypt_set_property_probe{32,64}.expected.
u32 BcryptManager::bcrypt_set_property(Marshaler& m, u32 h_object,
                                        GuestAddr psz_property,
                                        GuestAddr pb_input, u32 /*cb_input*/,
                                        u32 flags) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (flags || !psz_property || !pb_input) return STATUS_INVALID_PARAMETER;
    const auto property = m.read_wide_string(psz_property);
    if (property.is_err()) return STATUS_INVALID_PARAMETER;
    auto algorithm = algorithm_contexts_.find(h_object);
    auto key = key_contexts_.find(h_object);
    if (algorithm == algorithm_contexts_.end() && key == key_contexts_.end()) {
        // Pseudo-handles are shared immutable algorithm objects.
        for (const u32 pseudo : BCRYPT_ALGORITHM_PSEUDO_HANDLES)
            if (h_object == pseudo) return STATUS_ACCESS_DENIED;
        if (hash_contexts_.contains(h_object)) return STATUS_NOT_SUPPORTED;
        return STATUS_INVALID_HANDLE;
    }
    auto unsupported = [&](const char* which) {
        static std::mutex report_mutex;
        static std::unordered_set<std::string> reported;
        std::lock_guard<std::mutex> report_lock(report_mutex);
        if (reported.emplace(which).second)
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED bcrypt.dll!BCryptSetProperty: %s\n", which);
        return STATUS_NOT_SUPPORTED;
    };
    if (property.value() != BCRYPT_CHAINING_MODE) {
        // The currently issued AES keys have two additional writable
        // properties. Do not silently accept them without their side effects.
        if (property.value() == "IV" || property.value() == "MessageBlockLength")
            return unsupported(property.value().c_str());
        // Custom and read-only properties are not supported by this provider.
        return STATUS_NOT_SUPPORTED;
    }
    if (algorithm != algorithm_contexts_.end() && algorithm->second.alg != AlgType::AES)
        return STATUS_INVALID_HANDLE;
    // This property's string is explicitly unbounded by cbInput (Learn).
    const auto value = m.read_wide_string(pb_input);
    if (value.is_err()) return STATUS_INVALID_PARAMETER;
    ChainingMode mode;
    if (value.value() == BCRYPT_CHAIN_MODE_CBC) mode = ChainingMode::Cbc;
    else if (value.value() == BCRYPT_CHAIN_MODE_ECB) mode = ChainingMode::Ecb;
    else if (value.value() == BCRYPT_CHAIN_MODE_CFB ||
             value.value() == BCRYPT_CHAIN_MODE_CCM ||
             value.value() == BCRYPT_CHAIN_MODE_GCM)
        return unsupported(value.value().c_str());
    else return STATUS_INVALID_PARAMETER;
    if (key != key_contexts_.end()) key->second.chaining_mode = mode;
    else algorithm->second.chaining_mode = mode;
    return STATUS_SUCCESS;
}

// BCryptHash is a one-shot computation: no hash handle or guest scratch escapes.
// Microsoft Learn BCryptHash; Windows bcrypt_hash_probe32/64.expected records
// validation, exceptions, digest bytes and unchanged LastError/output on error.
u32 BcryptManager::bcrypt_hash(Marshaler& m, u64 h_algorithm,
                               GuestAddr pb_secret, u32 cb_secret,
                               GuestAddr pb_input, u32 cb_input,
                               GuestAddr pb_output, u32 cb_output) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto access_violation = [&](bool write, GuestAddr address) -> u32 {
        (void)m.raise_access_violation(write, address);
        return HASH_STATUS_ACCESS_VIOLATION;
    };
    if (h_algorithm == 0) return STATUS_INVALID_HANDLE;
    const auto alg = h_algorithm <= UINT32_MAX
        ? lookup_alg(static_cast<u32>(h_algorithm)) : std::nullopt;
    if (!alg) {
        if (h_algorithm <= UINT32_MAX) {
            const u32 handle = static_cast<u32>(h_algorithm);
            if (hash_contexts_.contains(handle) || key_contexts_.contains(handle) ||
                (handle >= kAlgHandleBase && handle < next_alg_handle_))
                return STATUS_INVALID_HANDLE;
            for (u32 pseudo : BCRYPT_ALGORITHM_PSEUDO_HANDLES) {
                if (pseudo != handle) continue;
                static std::atomic_flag reported = ATOMIC_FLAG_INIT;
                if (!reported.test_and_set())
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED bcrypt.dll!BCryptHash: algorithm pseudo-handle backend\n");
                return STATUS_NOT_SUPPORTED;
            }
        }
        return access_violation(false, h_algorithm);
    }
    const u32 length = digest_length(alg->alg);
    if (length == 0) return STATUS_NOT_SUPPORTED; // Real non-hash provider.
    if (!alg->is_hmac && (pb_secret != 0 || cb_secret != 0))
        return STATUS_INVALID_PARAMETER;

    std::vector<u8> secret;
    if (alg->is_hmac && cb_secret != 0) {
        secret.resize(cb_secret);
        if (m.memory().read_bytes(pb_secret, secret.data(), cb_secret).is_err())
            return access_violation(false, pb_secret);
    }
    std::vector<u8> input(cb_input);
    if (cb_input != 0 && m.memory().read_bytes(pb_input, input.data(), cb_input).is_err())
        return access_violation(false, pb_input);
    // Windows processes HashData before FinishHash validates the output.
    if (pb_output == 0 || cb_output != length) return STATUS_INVALID_PARAMETER;
    std::vector<u8> digest(length);
    if (alg->is_hmac) {
        CCHmacAlgorithm backend;
        switch (alg->alg) {
        case AlgType::SHA256: backend = kCCHmacAlgSHA256; break;
        case AlgType::SHA1: backend = kCCHmacAlgSHA1; break;
        case AlgType::MD5: backend = kCCHmacAlgMD5; break;
        default: return STATUS_NOT_SUPPORTED;
        }
        CCHmac(backend, secret.data(), secret.size(), input.data(), input.size(), digest.data());
    } else {
        switch (alg->alg) {
        case AlgType::SHA256: CC_SHA256(input.data(), cb_input, digest.data()); break;
        case AlgType::SHA1: CC_SHA1(input.data(), cb_input, digest.data()); break;
        case AlgType::MD5: CC_MD5(input.data(), cb_input, digest.data()); break;
        default: return STATUS_NOT_SUPPORTED;
        }
    }
    if (m.memory().write_bytes(pb_output, digest.data(), length).is_err())
        return access_violation(true, pb_output);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_create_hash(Marshaler& m, u32 h_algorithm,
                                        GuestAddr ph_hash,
                                        GuestAddr /*pb_hash_object*/,
                                        u32 /*cb_hash_object*/,
                                        GuestAddr pb_secret, u32 cb_secret,
                                        u32 /*flags*/) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_valid_alg_handle(h_algorithm))
        return STATUS_INVALID_HANDLE;
    if (!ph_hash)
        return STATUS_INVALID_PARAMETER;

    AlgType alg = alg_from_handle(h_algorithm);

    // Check if this algorithm handle was opened with HMAC flag
    bool hmac = false;
    hmac = lookup_alg(h_algorithm)->is_hmac;

    HashContext hc;
    hc.alg = alg;
    hc.is_hmac = hmac;

    if (hmac) {
        // Read the HMAC key from guest memory
        std::vector<u8> key;
        if (pb_secret != 0 && cb_secret > 0) {
            key.resize(cb_secret);
            auto r = m.memory().read_bytes(pb_secret,
                         reinterpret_cast<Byte*>(key.data()), cb_secret);
            if (!r.is_ok())
                return STATUS_INVALID_PARAMETER;
        }

        CCHmacAlgorithm cc_alg;
        switch (alg) {
            case AlgType::SHA256: cc_alg = kCCHmacAlgSHA256; break;
            case AlgType::SHA1:   cc_alg = kCCHmacAlgSHA1;   break;
            case AlgType::MD5:    cc_alg = kCCHmacAlgMD5;    break;
            default:              return STATUS_NOT_FOUND;
        }

        auto& ctx = hc.ctx.emplace<CCHmacContext>();
        CCHmacInit(&ctx, cc_alg, key.data(), key.size());
    } else {
    switch (alg) {
        case AlgType::SHA256: {
            auto& ctx = hc.ctx.emplace<CC_SHA256_CTX>();
            CC_SHA256_Init(&ctx);
            break;
        }
        case AlgType::SHA1: {
            auto& ctx = hc.ctx.emplace<CC_SHA1_CTX>();
            CC_SHA1_Init(&ctx);
            break;
        }
        case AlgType::MD5: {
            auto& ctx = hc.ctx.emplace<CC_MD5_CTX>();
            CC_MD5_Init(&ctx);
            break;
        }
        case AlgType::AES:
        case AlgType::RNG:
            return STATUS_NOT_FOUND;
    }
    }

    u32 handle = next_hash_handle_++;
    hash_contexts_.emplace(handle, std::move(hc));
    (void)m.write_ptr(ph_hash, handle); // BCRYPT_HASH_HANDLE*
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_hash_data(Marshaler& m, u32 h_hash,
                                      GuestAddr pb_input, u32 cb_input,
                                      u32 /*flags*/) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = hash_contexts_.find(h_hash);
    if (it == hash_contexts_.end())
        return STATUS_INVALID_HANDLE;

    if (!pb_input || cb_input == 0)
        return STATUS_SUCCESS;

    std::vector<Byte> buf(cb_input);
    auto r = m.memory().read_bytes(pb_input, buf.data(), cb_input);
    if (!r.is_ok())
        return STATUS_INVALID_PARAMETER;

    auto& hc = it->second;
    if (hc.is_hmac) {
        CCHmacUpdate(&std::get<CCHmacContext>(hc.ctx), buf.data(), cb_input);
    } else {
    switch (hc.alg) {
        case AlgType::SHA256:
            CC_SHA256_Update(&std::get<CC_SHA256_CTX>(hc.ctx), buf.data(), cb_input);
            break;
        case AlgType::SHA1:
            CC_SHA1_Update(&std::get<CC_SHA1_CTX>(hc.ctx), buf.data(), cb_input);
            break;
        case AlgType::MD5:
            CC_MD5_Update(&std::get<CC_MD5_CTX>(hc.ctx), buf.data(), cb_input);
            break;
        case AlgType::AES:
        case AlgType::RNG:
            return STATUS_INVALID_HANDLE;
    }
    }
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_finish_hash(Marshaler& m, u32 h_hash,
                                        GuestAddr pb_output, u32 cb_output,
                                        u32 /*flags*/) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = hash_contexts_.find(h_hash);
    if (it == hash_contexts_.end())
        return STATUS_INVALID_HANDLE;

    auto& hc = it->second;
    u32 expected = digest_length(hc.alg);
    if (!pb_output || cb_output < expected)
        return STATUS_INVALID_PARAMETER;

    std::vector<u8> digest(expected);
    if (hc.is_hmac) {
        CCHmacFinal(&std::get<CCHmacContext>(hc.ctx), digest.data());
    } else {
    switch (hc.alg) {
        case AlgType::SHA256:
            CC_SHA256_Final(digest.data(), &std::get<CC_SHA256_CTX>(hc.ctx));
            break;
        case AlgType::SHA1:
            CC_SHA1_Final(digest.data(), &std::get<CC_SHA1_CTX>(hc.ctx));
            break;
        case AlgType::MD5:
            CC_MD5_Final(digest.data(), &std::get<CC_MD5_CTX>(hc.ctx));
            break;
        case AlgType::AES:
        case AlgType::RNG:
            return STATUS_INVALID_HANDLE;
    }
    }

    (void)m.memory().write_bytes(pb_output,
                                 reinterpret_cast<const Byte*>(digest.data()), expected);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_destroy_hash(u32 h_hash) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = hash_contexts_.find(h_hash);
    if (it == hash_contexts_.end())
        return STATUS_INVALID_HANDLE;
    hash_contexts_.erase(it);
    return STATUS_SUCCESS;
}

// =============================================================================
// AES Symmetric Key Operations (via CommonCrypto CCCrypt)
// =============================================================================

u32 BcryptManager::bcrypt_generate_symmetric_key(Marshaler& m, u32 h_algorithm,
                                                   GuestAddr ph_key,
                                                   GuestAddr /*pb_key_object*/,
                                                   u32 /*cb_key_object*/,
                                                   GuestAddr pb_secret,
                                                   u32 cb_secret, u32 /*flags*/) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_valid_alg_handle(h_algorithm))
        return STATUS_INVALID_HANDLE;
    if (alg_from_handle(h_algorithm) != AlgType::AES)
        return STATUS_INVALID_PARAMETER;
    if (!ph_key || !pb_secret || cb_secret == 0)
        return STATUS_INVALID_PARAMETER;

    // Valid AES key sizes: 16 (128-bit), 24 (192-bit), 32 (256-bit)
    if (cb_secret != 16 && cb_secret != 24 && cb_secret != 32)
        return STATUS_INVALID_PARAMETER;

    KeyContext kc;
    kc.chaining_mode = lookup_alg(h_algorithm)->chaining_mode;
    kc.key_data.resize(cb_secret);
    auto r = m.memory().read_bytes(pb_secret,
                                    reinterpret_cast<Byte*>(kc.key_data.data()), cb_secret);
    if (!r.is_ok()) return STATUS_INVALID_PARAMETER;

    u32 handle = next_key_handle_++;
    key_contexts_.emplace(handle, std::move(kc));
    (void)m.write_ptr(ph_key, handle); // BCRYPT_KEY_HANDLE*
    VITREO_TRACE("[bcrypt] BCryptGenerateSymmetricKey -> 0x%x (AES-%u)\n",
                 handle, cb_secret * 8);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_encrypt(Marshaler& m, u32 h_key,
                                    GuestAddr pb_input, u32 cb_input,
                                    GuestAddr /*pb_padding_info*/,
                                    GuestAddr pb_iv, u32 cb_iv,
                                    GuestAddr pb_output, u32 cb_output,
                                    GuestAddr pcb_result, u32 flags) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = key_contexts_.find(h_key);
    if (it == key_contexts_.end()) return STATUS_INVALID_HANDLE;

    // BCRYPT_BLOCK_PADDING = 0x00000001
    CCOptions opts = (flags & 0x1) ? kCCOptionPKCS7Padding : 0;
    const bool cbc = it->second.chaining_mode == ChainingMode::Cbc;
    if (!cbc) opts |= kCCOptionECBMode;
    // Windows rejects an IV pointer in ECB mode, but reports the required
    // output length first; measured for both APIs and both guest bitnesses.
    if (!cbc && pb_iv) {
        const u32 required = (flags & 1) ? (cb_input / 16 + 1) * 16 : cb_input;
        if (!pcb_result || m.memory().write_u32(pcb_result, required).is_err())
            return STATUS_INVALID_PARAMETER;
        return cb_iv == 16 ? STATUS_INVALID_PARAMETER : STATUS_INVALID_BUFFER_SIZE;
    }

    // Read IV if provided
    std::vector<u8> iv_buf;
    if (cbc && pb_iv && cb_iv > 0) {
        iv_buf.resize(cb_iv);
        auto r = m.memory().read_bytes(pb_iv, reinterpret_cast<Byte*>(iv_buf.data()), cb_iv);
        if (!r.is_ok()) return STATUS_INVALID_PARAMETER;
    }

    // If output buffer is NULL, just return required size
    size_t needed = cb_input;
    if (opts & kCCOptionPKCS7Padding) {
        needed = ((cb_input / 16) + 1) * 16; // PKCS7 always pads at least 1 byte
    }
    if (pcb_result && (!pb_output || cb_output == 0)) {
        (void)m.memory().write_u32(pcb_result, static_cast<u32>(needed));
        return STATUS_SUCCESS;
    }

    // Read plaintext
    std::vector<u8> input(cb_input);
    if (cb_input > 0) {
        auto r = m.memory().read_bytes(pb_input, reinterpret_cast<Byte*>(input.data()), cb_input);
        if (!r.is_ok()) return STATUS_INVALID_PARAMETER;
    }

    std::vector<u8> output(cb_output);
    size_t out_moved = 0;

    CCCryptorStatus status = CCCrypt(
        kCCEncrypt, kCCAlgorithmAES,
        opts,
        it->second.key_data.data(), it->second.key_data.size(),
        iv_buf.empty() ? nullptr : iv_buf.data(),
        input.data(), cb_input,
        output.data(), cb_output,
        &out_moved);

    if (status != kCCSuccess) {
        VITREO_TRACE("[bcrypt] BCryptEncrypt: CCCrypt failed status=%d\n", status);
        return (status == kCCBufferTooSmall) ? STATUS_BUFFER_TOO_SMALL
                                             : STATUS_UNSUCCESSFUL;
    }

    (void)m.memory().write_bytes(pb_output,
                                  reinterpret_cast<const Byte*>(output.data()),
                                  static_cast<u32>(out_moved));
    if (pcb_result)
        (void)m.memory().write_u32(pcb_result, static_cast<u32>(out_moved));

    // Update IV in guest memory (CBC mode chaining)
    if (cbc && pb_iv && cb_iv >= 16 && out_moved >= 16) {
        (void)m.memory().write_bytes(pb_iv,
                                      reinterpret_cast<const Byte*>(output.data() + out_moved - 16), 16);
    }

    VITREO_TRACE("[bcrypt] BCryptEncrypt: %u -> %zu bytes\n", cb_input, out_moved);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_decrypt(Marshaler& m, u32 h_key,
                                    GuestAddr pb_input, u32 cb_input,
                                    GuestAddr /*pb_padding_info*/,
                                    GuestAddr pb_iv, u32 cb_iv,
                                    GuestAddr pb_output, u32 cb_output,
                                    GuestAddr pcb_result, u32 flags) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = key_contexts_.find(h_key);
    if (it == key_contexts_.end()) return STATUS_INVALID_HANDLE;

    CCOptions opts = (flags & 0x1) ? kCCOptionPKCS7Padding : 0;
    const bool cbc = it->second.chaining_mode == ChainingMode::Cbc;
    if (!cbc) opts |= kCCOptionECBMode;
    if (!cbc && pb_iv) {
        if (!pcb_result || m.memory().write_u32(pcb_result, cb_input).is_err())
            return STATUS_INVALID_PARAMETER;
        return cb_iv == 16 ? STATUS_INVALID_PARAMETER : STATUS_INVALID_BUFFER_SIZE;
    }

    std::vector<u8> iv_buf;
    if (cbc && pb_iv && cb_iv > 0) {
        iv_buf.resize(cb_iv);
        auto r = m.memory().read_bytes(pb_iv, reinterpret_cast<Byte*>(iv_buf.data()), cb_iv);
        if (!r.is_ok()) return STATUS_INVALID_PARAMETER;
    }

    // Size query
    if (pcb_result && (!pb_output || cb_output == 0)) {
        (void)m.memory().write_u32(pcb_result, cb_input); // max possible
        return STATUS_SUCCESS;
    }

    // Save IV for chaining before decrypt overwrites it
    std::vector<u8> next_iv;
    if (cbc && pb_iv && cb_iv >= 16 && cb_input >= 16) {
        next_iv.resize(16);
        auto r = m.memory().read_bytes(pb_input + cb_input - 16,
                                        reinterpret_cast<Byte*>(next_iv.data()), 16);
        if (!r.is_ok()) return STATUS_INVALID_PARAMETER;
    }

    std::vector<u8> input(cb_input);
    if (cb_input > 0) {
        auto r = m.memory().read_bytes(pb_input, reinterpret_cast<Byte*>(input.data()), cb_input);
        if (!r.is_ok()) return STATUS_INVALID_PARAMETER;
    }

    std::vector<u8> output(cb_output);
    size_t out_moved = 0;

    CCCryptorStatus status = CCCrypt(
        kCCDecrypt, kCCAlgorithmAES,
        opts,
        it->second.key_data.data(), it->second.key_data.size(),
        iv_buf.empty() ? nullptr : iv_buf.data(),
        input.data(), cb_input,
        output.data(), cb_output,
        &out_moved);

    if (status != kCCSuccess) {
        VITREO_TRACE("[bcrypt] BCryptDecrypt: CCCrypt failed status=%d\n", status);
        // kCCDecodeError indicates bad padding; kCCBufferTooSmall = buffer too small
        if (status == kCCDecodeError) return STATUS_UNSUCCESSFUL; // STATUS_DATA_ERROR equivalent
        if (status == kCCBufferTooSmall) return STATUS_BUFFER_TOO_SMALL;
        return STATUS_UNSUCCESSFUL;
    }

    (void)m.memory().write_bytes(pb_output,
                                  reinterpret_cast<const Byte*>(output.data()),
                                  static_cast<u32>(out_moved));
    if (pcb_result)
        (void)m.memory().write_u32(pcb_result, static_cast<u32>(out_moved));

    // Update IV in guest memory
    if (cbc && pb_iv && !next_iv.empty()) {
        (void)m.memory().write_bytes(pb_iv,
                                      reinterpret_cast<const Byte*>(next_iv.data()), 16);
    }

    VITREO_TRACE("[bcrypt] BCryptDecrypt: %u -> %zu bytes\n", cb_input, out_moved);
    return STATUS_SUCCESS;
}

u32 BcryptManager::bcrypt_destroy_key(u32 h_key) {
    // CNG handles are shared across threads (Learn); serialise the maps.
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = key_contexts_.find(h_key);
    if (it == key_contexts_.end()) return STATUS_INVALID_HANDLE;
    key_contexts_.erase(it);
    return STATUS_SUCCESS;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

/// BCRYPT_*_HANDLE arguments are pointer-sized. Vitreo's handles and the
/// pseudo-handles fit in 32 bits; anything with upper bits set is not a
/// handle this provider issued, so it maps to a value no table contains.
static u32 bcrypt_handle_arg(u64 raw) {
    return raw > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(raw);
}

static void impl_BCryptGenRandom(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_gen_random(
        m,
        bcrypt_handle_arg(m.arg(0)),
        m.arg_ptr(1),
        static_cast<u32>(m.arg(2)),
        static_cast<u32>(m.arg(3)));
    m.set_return(rc);
}

// BOOL ProcessPrng(PBYTE pbData, SIZE_T cbData) — exported by
// bcryptprimitives.dll. This is the user-mode system PRNG. Callers resolve
// it via LoadLibraryW+GetProcAddress and call it during early allocator
// init. It must exist: if the export is missing, an RNG bootstrap that holds
// the allocator lock re-enters the allocator and deadlocks. Always succeeds
// (fills with random).
static void impl_ProcessPrng(Marshaler& m, BcryptManager& /*mgr*/) {
    GuestAddr buf = m.arg_ptr(0);
    u64 count = m.arg(1);
    if (buf && count > 0) {
        std::vector<u8> data(static_cast<size_t>(count));
        arc4random_buf(data.data(), data.size());
        (void)m.memory().write_bytes(buf,
                                     reinterpret_cast<const Byte*>(data.data()),
                                     data.size());
    }
    m.set_return(1);  // TRUE
}

static void impl_BCryptOpenAlgorithmProvider(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_open_algorithm_provider(
        m,
        m.arg_ptr(0),                     // phAlgorithm
        m.arg_ptr(1),                      // pszAlgId
        m.arg_ptr(2),                      // pszImplementation
        static_cast<u32>(m.arg(3)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptCloseAlgorithmProvider(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_close_algorithm_provider(
        bcrypt_handle_arg(m.arg(0)),        // hAlgorithm
        static_cast<u32>(m.arg(1)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptGetProperty(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_get_property(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hObject
        m.arg_ptr(1),                      // pszProperty
        m.arg_ptr(2),                      // pbOutput
        static_cast<u32>(m.arg(3)),        // cbOutput
        m.arg_ptr(4),                      // pcbResult
        static_cast<u32>(m.arg(5)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptSetProperty(Marshaler& m, BcryptManager& mgr) {
    m.set_return(mgr.bcrypt_set_property(m, bcrypt_handle_arg(m.arg(0)),
        m.arg_ptr(1), m.arg_ptr(2), static_cast<u32>(m.arg(3)),
        static_cast<u32>(m.arg(4))));
}

static void impl_BCryptHash(Marshaler& m, BcryptManager& mgr) {
    m.set_return(mgr.bcrypt_hash(m, m.arg(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)),
        m.arg_ptr(3), static_cast<u32>(m.arg(4)), m.arg_ptr(5), static_cast<u32>(m.arg(6))));
}

static void impl_BCryptCreateHash(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_create_hash(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hAlgorithm
        m.arg_ptr(1),                      // phHash
        m.arg_ptr(2),                      // pbHashObject
        static_cast<u32>(m.arg(3)),        // cbHashObject
        m.arg_ptr(4),                      // pbSecret
        static_cast<u32>(m.arg(5)),        // cbSecret
        static_cast<u32>(m.arg(6)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptHashData(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_hash_data(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hHash
        m.arg_ptr(1),                      // pbInput
        static_cast<u32>(m.arg(2)),        // cbInput
        static_cast<u32>(m.arg(3)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptFinishHash(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_finish_hash(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hHash
        m.arg_ptr(1),                      // pbOutput
        static_cast<u32>(m.arg(2)),        // cbOutput
        static_cast<u32>(m.arg(3)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptDestroyHash(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_destroy_hash(bcrypt_handle_arg(m.arg(0)));
    m.set_return(rc);
}

static void impl_BCryptGenerateSymmetricKey(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_generate_symmetric_key(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hAlgorithm
        m.arg_ptr(1),                      // phKey
        m.arg_ptr(2),                      // pbKeyObject
        static_cast<u32>(m.arg(3)),        // cbKeyObject
        m.arg_ptr(4),                      // pbSecret
        static_cast<u32>(m.arg(5)),        // cbSecret
        static_cast<u32>(m.arg(6)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptEncrypt(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_encrypt(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hKey
        m.arg_ptr(1),                      // pbInput
        static_cast<u32>(m.arg(2)),        // cbInput
        m.arg_ptr(3),                      // pPaddingInfo
        m.arg_ptr(4),                      // pbIV
        static_cast<u32>(m.arg(5)),        // cbIV
        m.arg_ptr(6),                      // pbOutput
        static_cast<u32>(m.arg(7)),        // cbOutput
        m.arg_ptr(8),                      // pcbResult
        static_cast<u32>(m.arg(9)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptDecrypt(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_decrypt(
        m,
        bcrypt_handle_arg(m.arg(0)),        // hKey
        m.arg_ptr(1),                      // pbInput
        static_cast<u32>(m.arg(2)),        // cbInput
        m.arg_ptr(3),                      // pPaddingInfo
        m.arg_ptr(4),                      // pbIV
        static_cast<u32>(m.arg(5)),        // cbIV
        m.arg_ptr(6),                      // pbOutput
        static_cast<u32>(m.arg(7)),        // cbOutput
        m.arg_ptr(8),                      // pcbResult
        static_cast<u32>(m.arg(9)));       // dwFlags
    m.set_return(rc);
}

static void impl_BCryptDestroyKey(Marshaler& m, BcryptManager& mgr) {
    u32 rc = mgr.bcrypt_destroy_key(bcrypt_handle_arg(m.arg(0)));
    m.set_return(rc);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_bcrypt_thunks(BcryptManager& mgr) {
    DllExports exports;
    exports.entries.reserve(12);

    exports.entries.push_back(ExportEntry{
        "BCryptGenRandom", 0,
        [&mgr](Marshaler& m) { impl_BCryptGenRandom(m, mgr); },
        16
    });
    // bcryptprimitives.dll!ProcessPrng — system PRNG, resolved by
    // LoadLibraryW+GetProcAddress during allocator init.
    exports.entries.push_back(ExportEntry{
        "ProcessPrng", 0,
        [&mgr](Marshaler& m) { impl_ProcessPrng(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "BCryptOpenAlgorithmProvider", 0,
        [&mgr](Marshaler& m) { impl_BCryptOpenAlgorithmProvider(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "BCryptCloseAlgorithmProvider", 0,
        [&mgr](Marshaler& m) { impl_BCryptCloseAlgorithmProvider(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "BCryptGetProperty", 0,
        [&mgr](Marshaler& m) { impl_BCryptGetProperty(m, mgr); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "BCryptSetProperty", 0,
        [&mgr](Marshaler& m) { impl_BCryptSetProperty(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        BCRYPT_HASH_EXPORT_NAME, 0,
        [&mgr](Marshaler& m) { impl_BCryptHash(m, mgr); },
        BCRYPT_HASH_STDCALL_BYTES
    });
    exports.entries.push_back(ExportEntry{
        "BCryptCreateHash", 0,
        [&mgr](Marshaler& m) { impl_BCryptCreateHash(m, mgr); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "BCryptHashData", 0,
        [&mgr](Marshaler& m) { impl_BCryptHashData(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "BCryptFinishHash", 0,
        [&mgr](Marshaler& m) { impl_BCryptFinishHash(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "BCryptDestroyHash", 0,
        [&mgr](Marshaler& m) { impl_BCryptDestroyHash(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "BCryptGenerateSymmetricKey", 0,
        [&mgr](Marshaler& m) { impl_BCryptGenerateSymmetricKey(m, mgr); },
        28 // 7 args
    });
    exports.entries.push_back(ExportEntry{
        "BCryptEncrypt", 0,
        [&mgr](Marshaler& m) { impl_BCryptEncrypt(m, mgr); },
        40 // 10 args
    });
    exports.entries.push_back(ExportEntry{
        "BCryptDecrypt", 0,
        [&mgr](Marshaler& m) { impl_BCryptDecrypt(m, mgr); },
        40 // 10 args
    });
    exports.entries.push_back(ExportEntry{
        "BCryptDestroyKey", 0,
        [&mgr](Marshaler& m) { impl_BCryptDestroyKey(m, mgr); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi
