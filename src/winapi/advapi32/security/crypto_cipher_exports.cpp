// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_cipher_exports.cpp
/// @brief advapi32 legacy CryptoAPI ciphers: CryptEncrypt, CryptDecrypt,
/// CryptDeriveKey, CryptGetKeyParam, CryptGenRandom and SystemFunction036.
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

#include <array>
#include <cstring>
#include <mutex>
#include <stdlib.h> // arc4random_buf
#include <utility>
#include <variant>
#include <vector>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>

namespace vitreo::winapi {

using namespace capi;

namespace {

// ---------------------------------------------------------------------------
// CryptoAPI symmetric key derivation and bulk encryption.
//
// Measured against real Windows rather than assumed. For the Base provider,
// PROV_RSA_FULL: RC4 and RC2 derive 128-bit keys, DES 64-bit, 3DES 192-bit,
// and AES is NOT offered at all (CryptDeriveKey returns NTE_BAD_ALGID). Block
// ciphers run CBC with a zero IV and PKCS#5 padding, which is why 16 bytes of
// plaintext encrypt to 24.
//
// The derivation itself: when the required key is no longer than the hash, the
// key is simply the leading bytes of the digest. Verified bit-for-bit --
// RC4 over SHA-1("vitrum-probe-password") reproduces Windows' exact ciphertext
// c81c3df60c2aad98c1e8e752aa4e7103.
// ---------------------------------------------------------------------------

constexpr u32 kCalgRc2  = 0x00006602;
constexpr u32 kCalgRc4  = 0x00006801;
constexpr u32 kCalgDes  = 0x00006601;
constexpr u32 kCalg3Des = 0x00006603;

/// Key length in bytes for the Base provider, or 0 when unsupported.
[[nodiscard]] u32 crypt_alg_key_bytes(u32 alg_id) noexcept {
    switch (alg_id) {
        case kCalgRc4:  return 16;   // 128 bits
        case kCalgRc2:  return 16;   // 128 bits
        case kCalgDes:  return 8;    // 64 bits
        case kCalg3Des: return 24;   // 192 bits
        default:        return 0;
    }
}

/// Cipher block size in bytes; 0 marks a stream cipher.
[[nodiscard]] u32 crypt_alg_block_bytes(u32 alg_id) noexcept {
    return alg_id == kCalgRc4 ? 0u : 8u;
}

/// Derive @p want bytes of key material from a finished digest.
///
/// The short case is the leading bytes. The long case is the documented
/// expansion: two 64-byte buffers filled with 0x36 / 0x5C, XORed with the
/// hash, hashed again, and concatenated.
[[nodiscard]] std::vector<u8> crypt_derive_material(
        const std::vector<u8>& digest, u32 want, u32 hash_alg) {
    if (want <= digest.size()) {
        return std::vector<u8>(digest.begin(), digest.begin() + want);
    }
    std::array<u8, 64> b1{}, b2{};
    b1.fill(0x36);
    b2.fill(0x5C);
    for (usize i = 0; i < digest.size() && i < 64; ++i) {
        b1[i] = static_cast<u8>(b1[i] ^ digest[i]);
        b2[i] = static_cast<u8>(b2[i] ^ digest[i]);
    }
    std::vector<u8> out;
    const bool md5 = (hash_alg == 0x00008003u);
    if (md5) {
        std::array<u8, CC_MD5_DIGEST_LENGTH> h1{}, h2{};
        CC_MD5(b1.data(), 64, h1.data());
        CC_MD5(b2.data(), 64, h2.data());
        out.insert(out.end(), h1.begin(), h1.end());
        out.insert(out.end(), h2.begin(), h2.end());
    } else {
        std::array<u8, CC_SHA1_DIGEST_LENGTH> h1{}, h2{};
        CC_SHA1(b1.data(), 64, h1.data());
        CC_SHA1(b2.data(), 64, h2.data());
        out.insert(out.end(), h1.begin(), h1.end());
        out.insert(out.end(), h2.begin(), h2.end());
    }
    out.resize(want);
    return out;
}

/// One-shot symmetric transform. Returns false when the algorithm is not one
/// the Base provider offers, or when CommonCrypto rejects the request.
[[nodiscard]] bool crypt_transform(bool encrypt, u32 alg_id,
                                   const std::vector<u8>& key,
                                   const std::vector<u8>& in,
                                   bool final_block,
                                   std::vector<u8>& out) {
    CCAlgorithm cc{};
    switch (alg_id) {
        case kCalgRc4:  cc = kCCAlgorithmRC4;   break;
        case kCalgRc2:  cc = kCCAlgorithmRC2;   break;
        case kCalgDes:  cc = kCCAlgorithmDES;   break;
        case kCalg3Des: cc = kCCAlgorithm3DES;  break;
        default: return false;
    }
    // Only the final block carries padding, matching CryptEncrypt's Final flag.
    const bool block = crypt_alg_block_bytes(alg_id) != 0;
    CCOptions opts = 0;
    if (block && final_block) opts |= kCCOptionPKCS7Padding;
    const u8 iv[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // KP_IV defaults to zero
    out.assign(in.size() + 32, 0);
    usize moved = 0;
    const CCCryptorStatus st = CCCrypt(
        encrypt ? kCCEncrypt : kCCDecrypt, cc, opts,
        key.data(), key.size(), block ? iv : nullptr,
        in.data(), in.size(), out.data(), out.size(), &moved);
    if (st != kCCSuccess) return false;
    out.resize(moved);
    return true;
}

} // namespace

/// Registers CryptEncrypt, CryptDecrypt, CryptDeriveKey, CryptGetKeyParam,
/// CryptGenRandom, SystemFunction036.
void add_crypto_cipher_exports(DllExports& exports) {
    static CryptState* s_state = &capi::crypt_state();

    // CryptEncrypt / CryptDecrypt / CryptDeriveKey — register with correct
    // stdcall so a missing-export stub cannot smash the x86 stack.
    // Symmetric derive/encrypt is not implemented yet; fail honestly.
    // BOOL CryptEncrypt(HCRYPTKEY, HCRYPTHASH, BOOL Final, DWORD,
    //                   BYTE* pbData, DWORD* pdwDataLen, DWORD dwBufLen)
    exports.entries.push_back({"CryptEncrypt", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_KEY = 0x80090003u;
            constexpr u32 NTE_BAD_LEN = 0x80090004u;
            const u32 h_key = static_cast<u32>(m.arg(0));
            const bool final_block = m.arg(2) != 0;
            const GuestAddr data = m.arg_ptr(4);
            const GuestAddr len_ptr = m.arg_ptr(5);
            const u32 buf_len = static_cast<u32>(m.arg(6));

            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto it = s_state->keys.find(h_key);
            if (it == s_state->keys.end() || it->second.raw_key.empty()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const u32 in_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
            std::vector<u8> in(in_len);
            if (in_len && m.memory().read_bytes(data,
                    reinterpret_cast<Byte*>(in.data()), in_len).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            std::vector<u8> out;
            if (!crypt_transform(true, it->second.alg_id, it->second.raw_key,
                                 in, final_block, out)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            // A NULL data pointer is the size query: report the padded
            // length without writing. Callers size their buffer from it.
            if (data == 0) {
                if (len_ptr) (void)m.write_u32(len_ptr, static_cast<u32>(out.size()));
                m.set_return(1);
                return;
            }
            if (out.size() > buf_len) {
                if (len_ptr) (void)m.write_u32(len_ptr, static_cast<u32>(out.size()));
                set_last_error(NTE_BAD_LEN);
                m.set_return(0);
                return;
            }
            (void)m.memory().write_bytes(data,
                reinterpret_cast<const Byte*>(out.data()), out.size());
            if (len_ptr) (void)m.write_u32(len_ptr, static_cast<u32>(out.size()));
            VITREO_TRACE("[advapi32] CryptEncrypt(key=0x%x) %u -> %zu bytes\n",
                         h_key, in_len, out.size());
            m.set_return(1);
        }, 28});
    // BOOL CryptDecrypt(HCRYPTKEY, HCRYPTHASH, BOOL Final, DWORD,
    //                   BYTE* pbData, DWORD* pdwDataLen)
    exports.entries.push_back({"CryptDecrypt", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_KEY = 0x80090003u;
            const u32 h_key = static_cast<u32>(m.arg(0));
            const bool final_block = m.arg(2) != 0;
            const GuestAddr data = m.arg_ptr(4);
            const GuestAddr len_ptr = m.arg_ptr(5);

            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto it = s_state->keys.find(h_key);
            if (it == s_state->keys.end() || it->second.raw_key.empty()) {
                set_last_error(NTE_BAD_KEY);
                m.set_return(0);
                return;
            }
            const u32 in_len = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
            std::vector<u8> in(in_len);
            if (in_len && m.memory().read_bytes(data,
                    reinterpret_cast<Byte*>(in.data()), in_len).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            std::vector<u8> out;
            if (!crypt_transform(false, it->second.alg_id, it->second.raw_key,
                                 in, final_block, out)) {
                set_last_error(NTE_BAD_KEY);
                m.set_return(0);
                return;
            }
            (void)m.memory().write_bytes(data,
                reinterpret_cast<const Byte*>(out.data()), out.size());
            if (len_ptr) (void)m.write_u32(len_ptr, static_cast<u32>(out.size()));
            VITREO_TRACE("[advapi32] CryptDecrypt(key=0x%x) %u -> %zu bytes\n",
                         h_key, in_len, out.size());
            set_last_error(error_codes::ERROR_SUCCESS);
            m.set_return(1);
        }, 24});
    // CryptDeriveKey(HCRYPTPROV, ALG_ID, HCRYPTHASH, DWORD, HCRYPTKEY*)
    // — 5 args, stdcall=20. Learn: session-key derivation from a hash.
    exports.entries.push_back({"CryptDeriveKey", 0,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_ALGID = 0x80090008u;
            constexpr u32 NTE_BAD_HASH  = 0x80090002u;
            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 alg_id   = static_cast<u32>(m.arg(1));
            const u32 h_hash   = static_cast<u32>(m.arg(2));
            const u32 flags    = static_cast<u32>(m.arg(3));
            const GuestAddr out_key = m.arg_ptr(4);

            // A NULL hash handle is rejected before the algorithm is even
            // looked at: Windows returns ERROR_INVALID_PARAMETER for
            // hash=0 with a good algid, a bad algid, and a NULL provider
            // alike. Checking the algid first would report NTE_BAD_ALGID
            // for a case Windows calls a plain parameter error.
            if (h_hash == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const u32 want = crypt_alg_key_bytes(alg_id);
            if (want == 0) {
                // AES is genuinely absent from the Base provider on
                // Windows too -- verified, so this is the real answer.
                VITREO_TRACE("[advapi32] CryptDeriveKey alg=0x%x -> NTE_BAD_ALGID\n", alg_id);
                set_last_error(NTE_BAD_ALGID);
                m.set_return(0);
                return;
            }
            if (out_key == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto it = s_state->hashes.find(h_hash);
            if (it == s_state->hashes.end()) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            auto& hc = it->second;
            u32 digest_len = 0;
            switch (hc.alg_id) {
                case 0x00008003: digest_len = CC_MD5_DIGEST_LENGTH; break;
                case 0x00008004: digest_len = CC_SHA1_DIGEST_LENGTH; break;
                case 0x0000800c: digest_len = CC_SHA256_DIGEST_LENGTH; break;
                case 0x0000800e: digest_len = CC_SHA512_DIGEST_LENGTH; break;
                default: break;
            }
            if (digest_len == 0) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            // Deriving finalises the hash, exactly as HP_HASHVAL does.
            if (hc.finished_digest.empty()) {
                hc.finished_digest.resize(digest_len);
                if (auto* c = std::get_if<CC_MD5_CTX>(&hc.ctx)) {
                    CC_MD5_Final(hc.finished_digest.data(), c);
                } else if (auto* c = std::get_if<CC_SHA1_CTX>(&hc.ctx)) {
                    CC_SHA1_Final(hc.finished_digest.data(), c);
                } else if (auto* c = std::get_if<CC_SHA256_CTX>(&hc.ctx)) {
                    CC_SHA256_Final(hc.finished_digest.data(), c);
                } else if (auto* c = std::get_if<CC_SHA512_CTX>(&hc.ctx)) {
                    CC_SHA512_Final(hc.finished_digest.data(), c);
                }
            }

            KeyCtx kc;
            kc.provider = provider;
            kc.alg_id = alg_id;
            kc.exportable = (flags & 0x00000001u) != 0;  // CRYPT_EXPORTABLE
            kc.raw_key = crypt_derive_material(hc.finished_digest, want,
                                               hc.alg_id);
            const u32 handle = s_state->next_key++;
            s_state->keys.emplace(handle, std::move(kc));
            (void)m.write_u32(out_key, handle);
            VITREO_TRACE("[advapi32] CryptDeriveKey(alg=0x%x) -> key=0x%x (%u bytes)\n",
                         alg_id, handle, want);
            m.set_return(1);
        }, 20});

    // BOOL CryptGetKeyParam(HCRYPTKEY, DWORD dwParam, BYTE* pbData,
    //                       DWORD* pdwDataLen, DWORD dwFlags)
    //   — 5 args, stdcall=20.
    //
    // Was not registered at all, so it reached the generic missing-export
    // stub: that returns a TRUE-shaped 0xFFFFFFFF *and pops 0 stdcall
    // bytes*, so every caller both believed a bogus value and lost 20
    // bytes of stack. Values verified against Windows' Base provider.
    exports.entries.push_back({"CryptGetKeyParam", 0,
        [](Marshaler& m) {
            constexpr u32 KP_ALGID    = 7;
            constexpr u32 KP_BLOCKLEN = 8;
            constexpr u32 KP_KEYLEN   = 9;
            constexpr u32 KP_MODE     = 4;
            constexpr u32 NTE_BAD_KEY  = 0x80090003u;
            constexpr u32 NTE_BAD_TYPE = 0x8009000Au;
            const u32 h_key = static_cast<u32>(m.arg(0));
            const u32 param = static_cast<u32>(m.arg(1));
            const GuestAddr out = m.arg_ptr(2);
            const GuestAddr len_ptr = m.arg_ptr(3);

            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto it = s_state->keys.find(h_key);
            if (it == s_state->keys.end()) {
                set_last_error(NTE_BAD_KEY);
                m.set_return(0);
                return;
            }
            const u32 alg = it->second.alg_id;
            u32 value = 0;
            switch (param) {
                case KP_ALGID:    value = alg; break;
                case KP_KEYLEN:   value = crypt_alg_key_bytes(alg) * 8u; break;
                case KP_BLOCKLEN: value = crypt_alg_block_bytes(alg) * 8u; break;
                // A stream cipher reports mode 0; block ciphers default to
                // CBC (CRYPT_MODE_CBC == 1), which is what the transform
                // above actually performs.
                case KP_MODE:
                    value = crypt_alg_block_bytes(alg) != 0 ? 1u : 0u;
                    break;
                default:
                    set_last_error(NTE_BAD_TYPE);
                    m.set_return(0);
                    return;
            }
            constexpr u32 NEED = 4;
            const u32 cb = len_ptr ? m.read_u32(len_ptr).value_or(0) : 0;
            if (len_ptr) (void)m.write_u32(len_ptr, NEED);
            if (out == 0) {
                m.set_return(1);
                return;
            }
            if (cb < NEED) {
                set_last_error(error_codes::ERROR_MORE_DATA);
                m.set_return(0);
                return;
            }
            (void)m.write_u32(out, value);
            m.set_return(1);
        }, 20});

    // CryptGenRandom(HCRYPTPROV, DWORD, BYTE*) — 3 args, stdcall=12
    exports.entries.push_back({"CryptGenRandom", 57,
        [](Marshaler& m) {
            constexpr u32 NTE_BAD_UID = 0x80090001u;
            const u32 provider = static_cast<u32>(m.arg(0));
            const u32 cb = static_cast<u32>(m.arg(1));
            const GuestAddr buf = m.arg_ptr(2);
            {
                std::lock_guard<std::mutex> lock(s_state->mtx);
                if (!s_state->providers.contains(provider)) {
                    VITREO_TRACE(
                        "[advapi32] CryptGenRandom(hprov=0x%x, cb=%u, "
                        "buf=0x%llx) -> FALSE NTE_BAD_UID\n",
                        provider, cb,
                        static_cast<unsigned long long>(buf));
                    set_last_error(NTE_BAD_UID);
                    m.set_return(0);
                    return;
                }
            }
            if (cb == 0) {
                VITREO_TRACE(
                    "[advapi32] CryptGenRandom(hprov=0x%x, cb=0, "
                    "buf=0x%llx) -> TRUE\n",
                    provider, static_cast<unsigned long long>(buf));
                m.set_return(1); return;
            }
            if (buf == 0) {
                VITREO_TRACE(
                    "[advapi32] CryptGenRandom(hprov=0x%x, cb=%u, "
                    "buf=NULL) -> FALSE ERROR_INVALID_PARAMETER\n",
                    provider, cb);
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0); return;
            }
            std::vector<u8> rnd(cb);
            arc4random_buf(rnd.data(), cb);
            if (m.memory().write_bytes(
                    buf, reinterpret_cast<const Byte*>(rnd.data()), cb)
                    .is_err()) {
                VITREO_TRACE(
                    "[advapi32] CryptGenRandom(hprov=0x%x, cb=%u, "
                    "buf=0x%llx) -> FALSE ERROR_NOACCESS\n",
                    provider, cb,
                    static_cast<unsigned long long>(buf));
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            VITREO_TRACE(
                "[advapi32] CryptGenRandom(hprov=0x%x, cb=%u, "
                "buf=0x%llx) -> TRUE\n",
                provider, cb, static_cast<unsigned long long>(buf));
            m.set_return(1);
        }, 12});

    // SystemFunction036 (RtlGenRandom alias) — 2 args, stdcall=8
    // BOOLEAN RtlGenRandom(PVOID buf, ULONG len)
    exports.entries.push_back({"SystemFunction036", 58,
        [](Marshaler& m) {
            GuestAddr buf = m.arg_ptr(0);
            u32 cb = static_cast<u32>(m.arg(1));
            if (cb == 0) { m.set_return(1); return; }
            if (buf == 0) { m.set_return(0); return; }
            std::vector<u8> rnd(cb);
            arc4random_buf(rnd.data(), cb);
            Byte* host = m.memory().host_ptr_mut(buf, cb);
            if (host) std::memcpy(host, rnd.data(), cb);
            else (void)m.memory().write_bytes(buf,
                reinterpret_cast<const Byte*>(rnd.data()), cb);
            m.set_return(1);
        }, 8});
}

} // namespace vitreo::winapi
