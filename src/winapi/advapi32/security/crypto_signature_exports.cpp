// SPDX-License-Identifier: LGPL-2.1-only

/// @file crypto_signature_exports.cpp
/// @brief advapi32 legacy CryptoAPI signatures: CryptSignHashA/W and
/// CryptVerifySignatureA/W.
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

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>
#include <Security/Security.h>

namespace vitreo::winapi {

using namespace capi;

/// Registers CryptSignHashA/W, CryptVerifySignatureA/W.
void add_crypto_signature_exports(DllExports& exports) {
    static CryptState* s_state = &capi::crypt_state();
    // ALG_ID values
    constexpr u32 CALG_MD5     = 0x00008003;
    constexpr u32 CALG_SHA1    = 0x00008004;
    constexpr u32 CALG_SHA_256 = 0x0000800c;
    constexpr u32 CALG_SHA_512 = 0x0000800e;
    // Shared with CryptGetHashParam (crypto_hash_exports.cpp); defined in
    // crypto_support.cpp.
    const auto hash_finish = &capi::capi_hash_finish;

    // Finish a hash context into finished_digest (idempotent).
    auto finalize_hash = [hash_finish](HashCtx& hc) -> bool { return hash_finish(hc); };

    auto append_der_length = [](std::vector<u8>& out, usize length) {
        if (length < 0x80) {
            out.push_back(static_cast<u8>(length));
            return;
        }
        std::array<u8, sizeof(usize)> encoded{};
        usize count = 0;
        while (length != 0) {
            encoded[encoded.size() - 1 - count] =
                static_cast<u8>(length & 0xff);
            length >>= 8;
            ++count;
        }
        out.push_back(static_cast<u8>(0x80 | count));
        out.insert(out.end(), encoded.end() -
            static_cast<std::ptrdiff_t>(count), encoded.end());
    };
    auto append_der_integer = [append_der_length](
        std::vector<u8>& out, const std::vector<u8>& integer) {
        usize start = 0;
        while (start + 1 < integer.size() && integer[start] == 0) ++start;
        const bool pad = !integer.empty() && (integer[start] & 0x80) != 0;
        out.push_back(0x02);
        append_der_length(out, integer.size() - start + (pad ? 1 : 0));
        if (pad) out.push_back(0);
        out.insert(out.end(), integer.begin() +
            static_cast<std::ptrdiff_t>(start), integer.end());
    };
    auto rsa_private_der = [append_der_length, append_der_integer](
        const KeyCtx& key) {
        std::vector<u8> exponent = {
            static_cast<u8>(key.pubexp >> 24),
            static_cast<u8>(key.pubexp >> 16),
            static_cast<u8>(key.pubexp >> 8),
            static_cast<u8>(key.pubexp),
        };
        std::vector<u8> body;
        append_der_integer(body, std::vector<u8>{0});
        append_der_integer(body, key.modulus_be);
        append_der_integer(body, exponent);
        append_der_integer(body, key.private_exponent_be);
        append_der_integer(body, key.prime1_be);
        append_der_integer(body, key.prime2_be);
        append_der_integer(body, key.exponent1_be);
        append_der_integer(body, key.exponent2_be);
        append_der_integer(body, key.coefficient_be);
        std::vector<u8> der{0x30};
        append_der_length(der, body.size());
        der.insert(der.end(), body.begin(), body.end());
        return der;
    };

    // CryptSignHashA/W(HCRYPTHASH, DWORD, LPCSTR/LPCWSTR, DWORD,
    //                  BYTE*, DWORD*)
    auto sign_hash = [finalize_hash, rsa_private_der](Marshaler& m) {
        constexpr u32 NTE_BAD_HASH = 0x80090002u;
        constexpr u32 NTE_BAD_ALGID = 0x80090008u;
        constexpr u32 NTE_BAD_FLAGS = 0x80090009u;
        constexpr u32 NTE_NO_KEY = 0x8009000Du;
        constexpr u32 AT_KEYEXCHANGE = 1;
        constexpr u32 AT_SIGNATURE = 2;
        const u32 hash = static_cast<u32>(m.arg(0));
        const u32 key_spec = static_cast<u32>(m.arg(1));
        (void)m.arg(2); // Description is retained only for compatibility.
        const u32 flags = static_cast<u32>(m.arg(3));
        const GuestAddr signature = m.arg_ptr(4);
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
        if (key_spec != AT_KEYEXCHANGE && key_spec != AT_SIGNATURE) {
            set_last_error(NTE_BAD_ALGID);
            m.set_return(0);
            return;
        }

        KeyCtx key_ctx;
        HashCtx hash_ctx;
        {
            std::lock_guard<std::mutex> lock(s_state->mtx);
            const auto hash_it = s_state->hashes.find(hash);
            if (hash_it == s_state->hashes.end() ||
                !finalize_hash(hash_it->second)) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            const auto provider_it =
                s_state->providers.find(hash_it->second.provider);
            if (provider_it == s_state->providers.end()) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            const auto key_it = provider_it->second.user_keys.find(key_spec);
            if (key_it == provider_it->second.user_keys.end() ||
                key_it->second.private_exponent_be.empty()) {
                set_last_error(NTE_NO_KEY);
                m.set_return(0);
                return;
            }
            key_ctx = key_it->second;
            hash_ctx = hash_it->second;
        }

        const u32 required = static_cast<u32>(key_ctx.modulus_be.size());
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        if (m.write_u32(size_ptr, required).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        if (signature == 0) {
            m.set_return(1);
            return;
        }
        if (capacity < required) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            m.set_return(0);
            return;
        }

        const std::vector<u8> der = rsa_private_der(key_ctx);
        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        if (!attrs) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeRSA);
        CFDictionarySetValue(attrs, kSecAttrKeyClass, kSecAttrKeyClassPrivate);
        const CFIndex bits =
            static_cast<CFIndex>(key_ctx.modulus_be.size() * 8);
        CFNumberRef bits_num = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberCFIndexType, &bits);
        if (bits_num) {
            CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, bits_num);
            CFRelease(bits_num);
        }
        CFDataRef key_data = CFDataCreate(
            kCFAllocatorDefault, der.data(), static_cast<CFIndex>(der.size()));
        CFErrorRef error = nullptr;
        SecKeyRef private_key = key_data
            ? SecKeyCreateWithData(key_data, attrs, &error) : nullptr;
        if (key_data) CFRelease(key_data);
        CFRelease(attrs);
        if (!private_key) {
            if (error) CFRelease(error);
            set_last_error(NTE_NO_KEY);
            m.set_return(0);
            return;
        }

        CFStringRef algorithm = nullptr;
        std::vector<u8> sign_input = hash_ctx.finished_digest;
        switch (hash_ctx.alg_id) {
            case CALG_MD5: {
                static constexpr u8 prefix[] = {
                    0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86,
                    0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00,
                    0x04, 0x10,
                };
                sign_input.assign(prefix, prefix + sizeof(prefix));
                sign_input.insert(sign_input.end(),
                    hash_ctx.finished_digest.begin(),
                    hash_ctx.finished_digest.end());
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw;
                break;
            }
            case CALG_SHA1:
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1;
                break;
            case CALG_SHA_256:
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256;
                break;
            case CALG_SHA_512:
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512;
                break;
            default:
                CFRelease(private_key);
                set_last_error(NTE_BAD_ALGID);
                m.set_return(0);
                return;
        }

        CFDataRef digest = CFDataCreate(kCFAllocatorDefault,
            sign_input.data(), static_cast<CFIndex>(sign_input.size()));
        CFDataRef signed_data = digest ? SecKeyCreateSignature(
            private_key, algorithm, digest, &error) : nullptr;
        if (digest) CFRelease(digest);
        CFRelease(private_key);
        if (!signed_data ||
            CFDataGetLength(signed_data) != static_cast<CFIndex>(required)) {
            if (signed_data) CFRelease(signed_data);
            if (error) CFRelease(error);
            set_last_error(NTE_BAD_ALGID);
            m.set_return(0);
            return;
        }
        std::vector<u8> result(required);
        std::memcpy(result.data(), CFDataGetBytePtr(signed_data), required);
        CFRelease(signed_data);
        if (error) CFRelease(error);
        // CryptoAPI returns RSA signatures in little-endian order.
        std::reverse(result.begin(), result.end());
        if (m.memory().write_bytes(signature,
                result.data(), result.size()).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    };
    exports.entries.push_back({"CryptSignHashA", 0, sign_hash, 24});
    exports.entries.push_back({"CryptSignHashW", 0, sign_hash, 24});

    // CryptVerifySignatureA/W — 6 args, stdcall=24
    auto verify_signature = [finalize_hash](Marshaler& m, bool /*wide*/) {
        constexpr u32 NTE_BAD_HASH = 0x80090002u;
        constexpr u32 NTE_BAD_KEY = 0x80090003u;
        constexpr u32 NTE_BAD_SIGNATURE = 0x80090006u;
        constexpr u32 NTE_BAD_ALGID = 0x80090008u;

        const u32 h_hash = static_cast<u32>(m.arg(0));
        const GuestAddr sig_ptr = m.arg_ptr(1);
        const u32 sig_len = static_cast<u32>(m.arg(2));
        const u32 h_key = static_cast<u32>(m.arg(3));
        (void)m.arg(4);  // szDescription — ignored per Learn (must be NULL)
        const u32 flags = static_cast<u32>(m.arg(5));

        // A NULL key handle is a caller error, reported as
        // ERROR_INVALID_PARAMETER. NTE_BAD_KEY is reserved for a handle
        // that exists but is the wrong kind -- the two are distinguished
        // on Windows and callers branch on it.
        if (sig_ptr == 0 || sig_len == 0 || h_key == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        std::vector<u8> signature(sig_len);
        if (m.memory().read_bytes(sig_ptr, signature.data(), sig_len).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0);
            return;
        }
        // CryptoAPI RSA signatures are little-endian.
        std::reverse(signature.begin(), signature.end());

        KeyCtx key_copy;
        HashCtx hash_copy;
        {
            std::lock_guard<std::mutex> lock(s_state->mtx);
            auto key_it = s_state->keys.find(h_key);
            if (key_it == s_state->keys.end() ||
                key_it->second.blob_type != 0x06 /* PUBLICKEYBLOB */ ||
                key_it->second.modulus_be.empty()) {
                set_last_error(NTE_BAD_KEY);
                m.set_return(0);
                return;
            }
            auto hash_it = s_state->hashes.find(h_hash);
            if (hash_it == s_state->hashes.end()) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            if (!finalize_hash(hash_it->second)) {
                set_last_error(NTE_BAD_HASH);
                m.set_return(0);
                return;
            }
            key_copy = key_it->second;
            hash_copy = hash_it->second;
        }

        if (signature.size() != key_copy.modulus_be.size()) {
            VITREO_TRACE(
                "[advapi32] CryptVerifySignature: sig_len=%zu mod_len=%zu "
                "-> FALSE NTE_BAD_SIGNATURE\n",
                signature.size(), key_copy.modulus_be.size());
            set_last_error(NTE_BAD_SIGNATURE);
            m.set_return(0);
            return;
        }

        CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        if (!attrs) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeRSA);
        CFDictionarySetValue(attrs, kSecAttrKeyClass, kSecAttrKeyClassPublic);
        const CFIndex bits =
            static_cast<CFIndex>(key_copy.modulus_be.size() * 8);
        CFNumberRef bits_num = CFNumberCreate(
            kCFAllocatorDefault, kCFNumberCFIndexType, &bits);
        if (bits_num) {
            CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, bits_num);
            CFRelease(bits_num);
        }

        // Build PKCS#1 RSAPublicKey DER: SEQUENCE { INTEGER n, INTEGER e }
        auto encode_integer = [](const u8* data, usize len,
                                 std::vector<u8>& out) {
            // Strip leading zeros but keep one if high bit set needs 0x00 pad.
            usize start = 0;
            while (start + 1 < len && data[start] == 0) ++start;
            const bool pad = (data[start] & 0x80) != 0;
            const usize body = len - start;
            out.push_back(0x02);
            const usize total = body + (pad ? 1 : 0);
            if (total < 0x80) {
                out.push_back(static_cast<u8>(total));
            } else {
                out.push_back(0x81);
                out.push_back(static_cast<u8>(total));
            }
            if (pad) out.push_back(0x00);
            out.insert(out.end(), data + start, data + len);
        };

        std::vector<u8> der;
        der.reserve(key_copy.modulus_be.size() + 32);
        std::vector<u8> body;
        encode_integer(key_copy.modulus_be.data(),
                       key_copy.modulus_be.size(), body);
        u8 exp_be[4] = {
            static_cast<u8>((key_copy.pubexp >> 24) & 0xff),
            static_cast<u8>((key_copy.pubexp >> 16) & 0xff),
            static_cast<u8>((key_copy.pubexp >> 8) & 0xff),
            static_cast<u8>(key_copy.pubexp & 0xff),
        };
        encode_integer(exp_be, 4, body);
        der.push_back(0x30);
        if (body.size() < 0x80) {
            der.push_back(static_cast<u8>(body.size()));
        } else {
            der.push_back(0x81);
            der.push_back(static_cast<u8>(body.size()));
        }
        der.insert(der.end(), body.begin(), body.end());

        CFDataRef key_data = CFDataCreate(
            kCFAllocatorDefault, der.data(),
            static_cast<CFIndex>(der.size()));
        CFErrorRef err = nullptr;
        SecKeyRef pub = key_data
            ? SecKeyCreateWithData(key_data, attrs, &err)
            : nullptr;
        if (key_data) CFRelease(key_data);
        CFRelease(attrs);
        if (!pub) {
            if (err) CFRelease(err);
            VITREO_TRACE(
                "[advapi32] CryptVerifySignature: SecKeyCreateWithData "
                "failed -> FALSE NTE_BAD_KEY\n");
            set_last_error(NTE_BAD_KEY);
            m.set_return(0);
            return;
        }

        CFStringRef algorithm = nullptr;
        std::vector<u8> verify_input = hash_copy.finished_digest;
        switch (hash_copy.alg_id) {
            case 0x00008003: {  // CALG_MD5 — no named SecKey MD5 constant
                // DigestInfo for MD5 (PKCS #1):
                // 30 20 30 0c 06 08 2a 86 48 86 f7 0d 02 05 05 00 04 10 || H
                static constexpr u8 kMd5DigestInfoPrefix[] = {
                    0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86,
                    0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00,
                    0x04, 0x10,
                };
                verify_input.assign(
                    kMd5DigestInfoPrefix,
                    kMd5DigestInfoPrefix + sizeof(kMd5DigestInfoPrefix));
                verify_input.insert(verify_input.end(),
                                    hash_copy.finished_digest.begin(),
                                    hash_copy.finished_digest.end());
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw;
                break;
            }
            case 0x00008004:  // CALG_SHA1
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1;
                break;
            case 0x0000800c:  // CALG_SHA_256
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256;
                break;
            case 0x0000800e:  // CALG_SHA_512
                algorithm = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512;
                break;
            default:
                CFRelease(pub);
                set_last_error(NTE_BAD_ALGID);
                m.set_return(0);
                return;
        }
        (void)flags;

        CFDataRef digest_data = CFDataCreate(
            kCFAllocatorDefault, verify_input.data(),
            static_cast<CFIndex>(verify_input.size()));
        CFDataRef sig_data = CFDataCreate(
            kCFAllocatorDefault, signature.data(),
            static_cast<CFIndex>(signature.size()));
        bool ok = false;
        if (digest_data && sig_data) {
            ok = SecKeyVerifySignature(
                pub, algorithm, digest_data, sig_data, &err);
        }
        if (digest_data) CFRelease(digest_data);
        if (sig_data) CFRelease(sig_data);
        CFRelease(pub);
        if (err) CFRelease(err);

        VITREO_TRACE(
            "[advapi32] CryptVerifySignature(hash=0x%x key=0x%x "
            "sig_len=%u flags=0x%x) -> %s\n",
            h_hash, h_key, sig_len, flags, ok ? "TRUE" : "FALSE");
        if (!ok) {
            set_last_error(NTE_BAD_SIGNATURE);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    };

    exports.entries.push_back({"CryptVerifySignatureA", 0,
        [verify_signature](Marshaler& m) { verify_signature(m, false); },
        24});
    exports.entries.push_back({"CryptVerifySignatureW", 0,
        [verify_signature](Marshaler& m) { verify_signature(m, true); },
        24});
}

} // namespace vitreo::winapi
