// SPDX-License-Identifier: LGPL-2.1-only
/// @file crypt32.cpp
/// @brief CRYPT32.dll — "always-trust" certificate verification
///
/// Implements the Crypt32 surface a statically linked OpenSSL verify
/// callback needs.  Guest-side
/// structures (CERT_CONTEXT, CERT_CHAIN_CONTEXT, etc.) are allocated from
/// a dedicated scratch region at CRYPT32_SCRATCH_BASE.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "x509_parse.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonRandom.h>
#endif

/// @file dpapi_macos_keychain_commoncrypto.cpp
/// @brief DPAPI via macOS Keychain + CommonCrypto
///
/// Closed cut from crypt32.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// DPAPI via macOS Keychain + CommonCrypto
// =============================================================================

#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonRandom.h>

static constexpr const char* kDpapiKeychainService = "vitreo.dpapi.master";
static constexpr const char* kDpapiKeychainAccount = "master-key";
static constexpr const char* kDpapiKeyFileEnv = "VITREO_DPAPI_KEY_FILE";
static constexpr u32 kDpapiKeySize = 32; // AES-256
static constexpr u32 kDpapiIVSize  = 16; // AES block size

namespace {

class ScopedFileDescriptor {
public:
    explicit ScopedFileDescriptor(int fd = -1) noexcept : m_fd(fd) {}
    ~ScopedFileDescriptor() {
        if (m_fd >= 0) (void)::close(m_fd);
    }

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return m_fd; }

private:
    int m_fd;
};

enum class DpapiKeyFileResult : u8 {
    Loaded,
    Missing,
    Invalid,
};

[[nodiscard]] bool secure_dpapi_file(
    const struct stat& status, usize expected_size) {
    return S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() &&
           (status.st_mode & 0077) == 0 &&
           status.st_size == static_cast<off_t>(expected_size);
}

[[nodiscard]] bool read_exact_fd(int fd, u8* bytes, usize size) {
    usize offset = 0;
    while (offset < size) {
        const ssize_t count = ::read(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<usize>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

[[nodiscard]] bool write_exact_fd(int fd, const u8* bytes, usize size) {
    usize offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += static_cast<usize>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

[[nodiscard]] DpapiKeyFileResult load_dpapi_key_file(
    const std::string& path, std::vector<u8>& key) {
    ScopedFileDescriptor fd(
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0) {
        return errno == ENOENT
            ? DpapiKeyFileResult::Missing
            : DpapiKeyFileResult::Invalid;
    }

    struct stat status{};
    if (::fstat(fd.get(), &status) != 0 ||
        !secure_dpapi_file(status, kDpapiKeySize)) {
        return DpapiKeyFileResult::Invalid;
    }

    std::vector<u8> loaded(kDpapiKeySize);
    if (!read_exact_fd(fd.get(), loaded.data(), loaded.size())) {
        return DpapiKeyFileResult::Invalid;
    }
    key = std::move(loaded);
    return DpapiKeyFileResult::Loaded;
}

[[nodiscard]] bool store_dpapi_key_file(
    const std::string& path, const std::vector<u8>& key) {
    std::string temporary = path + ".tmp.XXXXXX";
    std::vector<char> mutable_path(temporary.begin(), temporary.end());
    mutable_path.push_back('\0');

    const int raw_fd = ::mkstemp(mutable_path.data());
    if (raw_fd < 0) return false;
    const std::string temporary_path(mutable_path.data());
    ScopedFileDescriptor fd(raw_fd);
    (void)::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);

    const bool written =
        ::fchmod(fd.get(), S_IRUSR | S_IWUSR) == 0 &&
        write_exact_fd(fd.get(), key.data(), key.size()) &&
        ::fsync(fd.get()) == 0;
    if (!written) {
        (void)::unlink(temporary_path.c_str());
        return false;
    }

    // link(2) publishes the fully written file only if the destination is
    // still absent; it never overwrites an existing key.
    if (::link(temporary_path.c_str(), path.c_str()) != 0) {
        (void)::unlink(temporary_path.c_str());
        return false;
    }
    (void)::unlink(temporary_path.c_str());
    return true;
}

[[nodiscard]] bool valid_dpapi_key_path(const char* path) {
    return path != nullptr && path[0] == '/' && path[1] != '\0';
}

} // namespace

bool Crypt32Manager::ensure_dpapi_key() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dpapi_key.empty()) return true;

    // Explicit env wins. Otherwise use the capsule-scoped development file
    // when RuntimeContext configured one. Either path avoids Keychain
    // identity prompts after an unsigned rebuild.
    const char* configured_path = std::getenv(kDpapiKeyFileEnv);
    std::string key_path;
    if (configured_path != nullptr) {
        if (!valid_dpapi_key_path(configured_path)) {
            VITREO_TRACE(
                "[crypt32] DPAPI: %s must name an absolute file\n",
                kDpapiKeyFileEnv);
            return false;
        }
        key_path = configured_path;
    } else if (!m_dpapi_key_file.empty()) {
        if (!valid_dpapi_key_path(m_dpapi_key_file.c_str())) {
            VITREO_TRACE(
                "[crypt32] DPAPI: capsule key path must be an absolute file\n");
            return false;
        }
        key_path = m_dpapi_key_file;
    }

    std::unique_ptr<ScopedFileDescriptor> key_file_lock;
    if (!key_path.empty()) {
        const std::string lock_path = key_path + ".lock";
        const int lock_fd = ::open(
            lock_path.c_str(),
            O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            S_IRUSR | S_IWUSR);
        if (lock_fd < 0) {
            VITREO_TRACE(
                "[crypt32] DPAPI: cannot open key-file lock '%s': %s\n",
                lock_path.c_str(), std::strerror(errno));
            return false;
        }
        key_file_lock =
            std::make_unique<ScopedFileDescriptor>(lock_fd);
        struct stat lock_status{};
        if (::fstat(lock_fd, &lock_status) != 0 ||
            !secure_dpapi_file(lock_status, 0) ||
            ::flock(lock_fd, LOCK_EX) != 0) {
            VITREO_TRACE(
                "[crypt32] DPAPI: insecure or unusable key-file lock '%s'\n",
                lock_path.c_str());
            return false;
        }

        const DpapiKeyFileResult file_result =
            load_dpapi_key_file(key_path, m_dpapi_key);
        if (file_result == DpapiKeyFileResult::Loaded) {
            VITREO_TRACE(
                "[crypt32] DPAPI: loaded development master key from '%s'\n",
                key_path.c_str());
            return true;
        }
        if (file_result == DpapiKeyFileResult::Invalid) {
            VITREO_TRACE(
                "[crypt32] DPAPI: key file '%s' must be a 32-byte, "
                "owner-only regular file\n",
                key_path.c_str());
            return false;
        }
        // Missing: fall through to Keychain and seed the file below.
    }

    // Try to load existing key from Keychain
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFStringRef service = CFStringCreateWithCString(kCFAllocatorDefault,
        kDpapiKeychainService, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(kCFAllocatorDefault,
        kDpapiKeychainAccount, kCFStringEncodingUTF8);
    CFDictionarySetValue(query, kSecAttrService, service);
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);

    if (status == errSecSuccess && result &&
        CFGetTypeID(result) == CFDataGetTypeID()) {
        CFDataRef data = static_cast<CFDataRef>(result);
        const u8* ptr = CFDataGetBytePtr(data);
        CFIndex len = CFDataGetLength(data);
        if (len == kDpapiKeySize) {
            m_dpapi_key.assign(ptr, ptr + len);
            CFRelease(result);
            CFRelease(service);
            CFRelease(account);
            if (!key_path.empty() &&
                !store_dpapi_key_file(key_path, m_dpapi_key)) {
                m_dpapi_key.clear();
                VITREO_TRACE(
                    "[crypt32] DPAPI: failed to seed development key file "
                    "'%s'\n",
                    key_path.c_str());
                return false;
            }
            VITREO_TRACE(
                "[crypt32] DPAPI: loaded master key from Keychain%s\n",
                key_path.empty() ? "" : " and seeded development key file");
            return true;
        }
        CFRelease(result);
        CFRelease(service);
        CFRelease(account);
        VITREO_TRACE(
            "[crypt32] DPAPI: Keychain item has invalid key length\n");
        return false;
    }
    if (status != errSecItemNotFound) {
        if (result) CFRelease(result);
        CFRelease(service);
        CFRelease(account);
        VITREO_TRACE(
            "[crypt32] DPAPI: Keychain lookup failed status=%d\n",
            static_cast<int>(status));
        return false;
    }

    // Generate a new key
    m_dpapi_key.resize(kDpapiKeySize);
    if (CCRandomGenerateBytes(m_dpapi_key.data(), kDpapiKeySize) != kCCSuccess) {
        m_dpapi_key.clear();
        CFRelease(service);
        CFRelease(account);
        VITREO_TRACE("[crypt32] DPAPI: CCRandomGenerateBytes failed\n");
        return false;
    }

    // Store in Keychain
    CFMutableDictionaryRef add = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(add, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(add, kSecAttrService, service);
    CFDictionarySetValue(add, kSecAttrAccount, account);
    CFDataRef key_data = CFDataCreate(kCFAllocatorDefault,
        m_dpapi_key.data(), kDpapiKeySize);
    CFDictionarySetValue(add, kSecValueData, key_data);
    const OSStatus add_status = SecItemAdd(add, nullptr);
    CFRelease(key_data);
    CFRelease(add);
    CFRelease(service);
    CFRelease(account);
    if (add_status != errSecSuccess) {
        m_dpapi_key.clear();
        VITREO_TRACE(
            "[crypt32] DPAPI: Keychain store failed status=%d\n",
            static_cast<int>(add_status));
        return false;
    }
    if (!key_path.empty() &&
        !store_dpapi_key_file(key_path, m_dpapi_key)) {
        m_dpapi_key.clear();
        VITREO_TRACE(
            "[crypt32] DPAPI: failed to seed development key file '%s'\n",
            key_path.c_str());
        return false;
    }
    VITREO_TRACE(
        "[crypt32] DPAPI: generated and stored new master key in Keychain%s\n",
        key_path.empty() ? "" : " and development key file");
    return true;
}
#else
bool Crypt32Manager::ensure_dpapi_key() { return false; }
#endif

// =============================================================================
// CryptProtectData / CryptUnprotectData / CryptProtectMemory
// =============================================================================
//
// Contracts: https://learn.microsoft.com/en-us/windows/win32/api/dpapi/
// (CryptProtectData, CryptUnprotectData, CryptProtectMemory,
// CryptUnprotectMemory). Measured behaviour, x86 and x64:
// tools/oracle_probe/crypt_dpapi_probe.c (+ .expected / .x86.expected).
//
// Blob format written by this implementation (version 2):
//   "VDP2" | u32 flags | u32 description bytes | description (UTF-16LE, NUL)
//   | 16-byte salt | 16-byte IV | AES-256-CBC ciphertext | HMAC-SHA256
// The encryption and MAC keys are derived from the capsule master key, the
// salt and the optional entropy, so a wrong or missing entropy and any
// modification fail the MAC (ERROR_INVALID_DATA), as Windows does. Blobs
// from the earlier format (16-byte IV + ciphertext, no entropy, no MAC) are
// still accepted by CryptUnprotectData so persisted guest data stays
// readable; they carry no integrity check.

namespace {

constexpr u8 kBlobMagic[4] = {'V', 'D', 'P', '2'};
constexpr u32 kErrorInvalidData = 13;
constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorPasswordRestriction = 1325;
constexpr u32 kNteBadKeyState = 0x8009000Bu;
constexpr u32 kCryptprotectUiForbidden = 0x1;
constexpr u32 kCryptprotectLocalMachine = 0x4;
constexpr u32 kCryptprotectCredSync = 0x8;
constexpr u32 kPromptOnUnprotect = 0x1;
constexpr u32 kPromptOnProtect = 0x2;

std::vector<u8> hmac_sha256(std::span<const u8> key, std::span<const std::span<const u8>> parts) {
    CCHmacContext context;
    CCHmacInit(&context, kCCHmacAlgSHA256, key.data(), key.size());
    for (auto part : parts) CCHmacUpdate(&context, part.data(), part.size());
    std::vector<u8> out(CC_SHA256_DIGEST_LENGTH);
    CCHmacFinal(&context, out.data());
    return out;
}

std::vector<u8> derive(std::span<const u8> master, std::string_view label,
                       std::span<const u8> salt, std::span<const u8> entropy) {
    const u8 length[4] = {static_cast<u8>(entropy.size()), static_cast<u8>(entropy.size() >> 8),
                          static_cast<u8>(entropy.size() >> 16), static_cast<u8>(entropy.size() >> 24)};
    const std::span<const u8> parts[] = {
        {reinterpret_cast<const u8*>(label.data()), label.size()}, salt, {length, 4}, entropy};
    return hmac_sha256(master, parts);
}

bool aes_cbc(CCOperation op, CCOptions options, std::span<const u8> key, const u8* iv,
             std::span<const u8> in, std::vector<u8>& out) {
    out.resize(in.size() + kCCBlockSizeAES128);
    size_t moved = 0;
    const CCCryptorStatus status = CCCrypt(op, kCCAlgorithmAES, options, key.data(), key.size(),
                                           iv, in.data(), in.size(), out.data(), out.size(), &moved);
    if (status != kCCSuccess) return false;
    out.resize(moved);
    return true;
}

bool read_blob(Marshaler& m, GuestAddr blob, std::vector<u8>& out) {
    const GuestAddr pointer_offset = m.cpu_is_64bit() ? 8 : 4;
    const u32 size = m.read_u32(blob).value_or(0);
    const GuestAddr data = static_cast<GuestAddr>(m.read_ptr(blob + pointer_offset).value_or(0));
    out.resize(size);
    return size == 0 ||
           m.memory().read_bytes(data, reinterpret_cast<Byte*>(out.data()), size).is_ok();
}

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 get_u32(std::span<const u8> in, usize at) {
    return static_cast<u32>(in[at]) | static_cast<u32>(in[at + 1]) << 8 |
           static_cast<u32>(in[at + 2]) << 16 | static_cast<u32>(in[at + 3]) << 24;
}

} // namespace

// CRYPTPROTECT_PROMPTSTRUCT { cbSize; dwPromptFlags; hwndApp; szPrompt }:
// a prompt the caller asked for cannot be shown.
static u32 prompt_error(Marshaler& m, GuestAddr prompt, u32 flags, u32 wanted,
                        const char* api) {
    if (prompt == 0) return 0;
    const u32 prompt_flags = m.read_u32(prompt + 4).value_or(0);
    if ((prompt_flags & wanted) == 0) return 0;
    if ((flags & kCryptprotectUiForbidden) == 0) {
        crypt32::report_unimplemented(api, "CRYPTPROTECT_PROMPTSTRUCT prompts (no UI)");
    }
    return kErrorPasswordRestriction;
}

// BOOL CryptProtectData(DATA_BLOB* pDataIn, LPCWSTR szDataDescr,
//     DATA_BLOB* pOptionalEntropy, PVOID pvReserved,
//     CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct, DWORD dwFlags, DATA_BLOB* pDataOut)
u32 Crypt32Manager::crypt_protect_data(Marshaler& m) {
#ifdef __APPLE__
    const GuestAddr in = m.arg_ptr(0);
    const GuestAddr description = m.arg_ptr(1);
    const GuestAddr entropy_blob = m.arg_ptr(2);
    const GuestAddr prompt = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    const GuestAddr out = m.arg_ptr(6);
    const GuestAddr pointer_offset = m.cpu_is_64bit() ? 8 : 4;
    if (in == 0 || out == 0) {
        set_last_error(kErrorInvalidParameter);
        return 0;
    }
    if (const u32 error = prompt_error(m, prompt, flags, kPromptOnProtect,
                                       "crypt32!CryptProtectData")) {
        set_last_error(error);
        return 0;
    }
    std::vector<u8> plaintext;
    std::vector<u8> entropy;
    if (!read_blob(m, in, plaintext) || (entropy_blob != 0 && !read_blob(m, entropy_blob, entropy))) {
        set_last_error(kErrorInvalidParameter);
        return 0;
    }
    std::vector<u8> text;  // UTF-16LE with its NUL
    for (GuestAddr p = description; p != 0; p += 2) {
        const u16 unit = m.read_u16(p).value_or(0);
        if (unit == 0 || text.size() > 0x10000) break;
        text.push_back(static_cast<u8>(unit));
        text.push_back(static_cast<u8>(unit >> 8));
    }
    text.push_back(0);
    text.push_back(0);
    if (!ensure_dpapi_key()) {
        set_last_error(kNteBadKeyState);
        return 0;
    }
    u8 salt[16];
    u8 iv[kDpapiIVSize];
    CCRandomGenerateBytes(salt, sizeof(salt));
    CCRandomGenerateBytes(iv, sizeof(iv));
    const auto enc_key = derive(m_dpapi_key, "vitreo.dpapi.v2.enc", salt, entropy);
    const auto mac_key = derive(m_dpapi_key, "vitreo.dpapi.v2.mac", salt, entropy);
    std::vector<u8> ciphertext;
    if (!aes_cbc(kCCEncrypt, kCCOptionPKCS7Padding, enc_key, iv, plaintext, ciphertext)) {
        set_last_error(kNteBadKeyState);
        return 0;
    }
    std::vector<u8> blob(kBlobMagic, kBlobMagic + 4);
    put_u32(blob, flags & kCryptprotectLocalMachine);
    put_u32(blob, static_cast<u32>(text.size()));
    blob.insert(blob.end(), text.begin(), text.end());
    blob.insert(blob.end(), salt, salt + sizeof(salt));
    blob.insert(blob.end(), iv, iv + sizeof(iv));
    blob.insert(blob.end(), ciphertext.begin(), ciphertext.end());
    const std::span<const u8> mac_input[] = {blob};
    const auto mac = hmac_sha256(mac_key, mac_input);
    blob.insert(blob.end(), mac.begin(), mac.end());

    const GuestAddr buffer = alloc_guest(static_cast<u32>(blob.size()));  // LocalAlloc
    if (buffer == 0 ||
        m.memory().write_bytes(buffer, reinterpret_cast<const Byte*>(blob.data()), blob.size()).is_err()) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    (void)m.write_u32(out, static_cast<u32>(blob.size()));
    (void)m.write_ptr(out + pointer_offset, buffer);
    set_last_error(0);
    return 1;
#else
    (void)m;
    return 0;
#endif
}

// BOOL CryptUnprotectData(DATA_BLOB* pDataIn, LPWSTR* ppszDataDescr,
//     DATA_BLOB* pOptionalEntropy, PVOID pvReserved,
//     CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct, DWORD dwFlags, DATA_BLOB* pDataOut)
u32 Crypt32Manager::crypt_unprotect_data(Marshaler& m) {
#ifdef __APPLE__
    const GuestAddr in = m.arg_ptr(0);
    const GuestAddr description_out = m.arg_ptr(1);
    const GuestAddr entropy_blob = m.arg_ptr(2);
    const GuestAddr prompt = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    const GuestAddr out = m.arg_ptr(6);
    const GuestAddr pointer_offset = m.cpu_is_64bit() ? 8 : 4;
    if (in == 0 || out == 0 || (flags & kCryptprotectCredSync) != 0) {
        set_last_error(kErrorInvalidParameter);
        return 0;
    }
    (void)m.write_u32(out, 0);
    (void)m.write_ptr(out + pointer_offset, 0);
    if (const u32 error = prompt_error(m, prompt, flags, kPromptOnUnprotect,
                                       "crypt32!CryptUnprotectData")) {
        set_last_error(error);
        return 0;
    }
    std::vector<u8> blob;
    std::vector<u8> entropy;
    if (!read_blob(m, in, blob) || (entropy_blob != 0 && !read_blob(m, entropy_blob, entropy))) {
        set_last_error(kErrorInvalidParameter);
        return 0;
    }
    if (blob.empty()) {
        set_last_error(kErrorInvalidParameter);
        return 0;
    }
    if (!ensure_dpapi_key()) {
        set_last_error(kNteBadKeyState);
        return 0;
    }

    std::vector<u8> plaintext;
    std::vector<u8> description;
    if (blob.size() >= 4 && std::equal(kBlobMagic, kBlobMagic + 4, blob.begin())) {
        // Version 2: every field is covered by the MAC.
        if (blob.size() < 12) { set_last_error(kErrorInvalidParameter); return 0; }
        const u32 text_size = get_u32(blob, 8);
        const usize header = 12 + static_cast<usize>(text_size);
        if (text_size > blob.size() || blob.size() < header + 32 + 32 + 16 ||
            (blob.size() - header - 32 - 32) % 16 != 0) {
            set_last_error(kErrorInvalidParameter);
            return 0;
        }
        const std::span<const u8> all(blob);
        const auto salt = all.subspan(header, 16);
        const auto iv = all.subspan(header + 16, 16);
        const auto body = all.subspan(header + 32, blob.size() - header - 32 - 32);
        const auto mac = all.subspan(blob.size() - 32);
        const auto enc_key = derive(m_dpapi_key, "vitreo.dpapi.v2.enc", salt, entropy);
        const auto mac_key = derive(m_dpapi_key, "vitreo.dpapi.v2.mac", salt, entropy);
        const std::span<const u8> mac_input[] = {all.first(blob.size() - 32)};
        const auto expected = hmac_sha256(mac_key, mac_input);
        u8 difference = 0;
        for (usize i = 0; i < 32; ++i) difference |= static_cast<u8>(expected[i] ^ mac[i]);
        if (difference != 0 ||
            !aes_cbc(kCCDecrypt, kCCOptionPKCS7Padding, enc_key, iv.data(), body, plaintext)) {
            set_last_error(kErrorInvalidData);
            return 0;
        }
        description.assign(blob.begin() + 12, blob.begin() + static_cast<isize>(header));
    } else {
        // Legacy (IV + ciphertext) blob from before version 2. A blob that
        // has version-2 framing (consistent description length and cipher
        // block sizes) but a damaged magic is corrupt data rather than a
        // non-DPAPI buffer: Windows reports ERROR_INVALID_DATA for any
        // modified byte of a real blob and ERROR_INVALID_PARAMETER for
        // garbage (crypt_dpapi_probe "tamper" / "garbage").
        bool v2_framing = false;
        if (blob.size() >= 12) {
            const u32 text_size = get_u32(blob, 8);
            const usize header = 12 + static_cast<usize>(text_size);
            v2_framing = text_size <= blob.size() && blob.size() >= header + 32 + 32 + 16 &&
                         (blob.size() - header - 32 - 32) % 16 == 0;
        }
        if (blob.size() <= kDpapiIVSize || (blob.size() - kDpapiIVSize) % 16 != 0 ||
            !aes_cbc(kCCDecrypt, kCCOptionPKCS7Padding, m_dpapi_key, blob.data(),
                     std::span<const u8>(blob).subspan(kDpapiIVSize), plaintext)) {
            set_last_error(v2_framing ? kErrorInvalidData : kErrorInvalidParameter);
            return 0;
        }
        description = {0, 0};
    }

    // Windows hands back a LocalAlloc'd pbData even for an empty plaintext
    // (crypt_dpapi_probe "empty": pbData non-NULL, cbData 0).
    const GuestAddr buffer = alloc_guest(static_cast<u32>(plaintext.size()));  // LocalAlloc
    if (buffer == 0 || (!plaintext.empty() &&
                        m.memory().write_bytes(buffer, reinterpret_cast<const Byte*>(plaintext.data()),
                                               plaintext.size()).is_err())) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (description_out != 0) {
        const GuestAddr text = alloc_guest(static_cast<u32>(description.size()));
        if (text == 0 || m.memory().write_bytes(text, reinterpret_cast<const Byte*>(description.data()),
                                                description.size()).is_err()) {
            free_guest(buffer);
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        (void)m.write_ptr(description_out, text);
    }
    (void)m.write_u32(out, static_cast<u32>(plaintext.size()));
    (void)m.write_ptr(out + pointer_offset, buffer);
    set_last_error(0);
    return 1;
#else
    (void)m;
    return 0;
#endif
}

// BOOL CryptProtectMemory / CryptUnprotectMemory(LPVOID pDataIn, DWORD cbDataIn,
//     DWORD dwFlags): AES-256-CBC in place (deterministic, like Windows:
// crypt_dpapi_probe "mem_det"), cbDataIn a non-zero multiple of 16. The
// same-process key lives only in this process; the cross-process and
// same-logon keys are derived from the capsule master key.
u32 Crypt32Manager::crypt_protect_memory(Marshaler& m, bool protect) {
#ifdef __APPLE__
    constexpr u32 kSameProcess = 0x0;
    constexpr u32 kCrossProcess = 0x1;
    constexpr u32 kSameLogon = 0x2;
    constexpr u32 kUndocumented4 = 0x4;  // protect works, unprotect is ACCESS_DENIED (measured)
    const GuestAddr data = m.arg_ptr(0);
    const u32 size = m.arg32(1);
    const u32 flags = m.arg32(2);
    if (size == 0 || size % 16 != 0 ||
        (flags != kSameProcess && flags != kCrossProcess && flags != kSameLogon &&
         flags != kUndocumented4)) {
        set_last_error(kErrorInvalidParameter);
        return 0;
    }
    if (!protect && flags == kUndocumented4) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return 0;
    }
    std::vector<u8> key;
    if (flags == kSameProcess || flags == kUndocumented4) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_process_memory_key.empty()) {
            m_process_memory_key.resize(32);
            CCRandomGenerateBytes(m_process_memory_key.data(), m_process_memory_key.size());
        }
        key = m_process_memory_key;
    } else {
        if (!ensure_dpapi_key()) {
            set_last_error(kNteBadKeyState);
            return 0;
        }
        key = derive(m_dpapi_key, flags == kCrossProcess ? "vitreo.protectmemory.cross"
                                                         : "vitreo.protectmemory.logon", {}, {});
    }
    std::vector<u8> in(size);
    if (m.memory().read_bytes(data, reinterpret_cast<Byte*>(in.data()), size).is_err()) {
        crypt32::raise_access_violation(m, data, false);
        return 0;
    }
    const u8 iv[16] = {};
    std::vector<u8> result;
    if (!aes_cbc(protect ? kCCEncrypt : kCCDecrypt, 0, key, iv, in, result) || result.size() != size) {
        set_last_error(kNteBadKeyState);
        return 0;
    }
    if (m.memory().write_bytes(data, reinterpret_cast<const Byte*>(result.data()), size).is_err()) {
        crypt32::raise_access_violation(m, data, true);
        return 0;
    }
    return 1;
#else
    (void)m;
    (void)protect;
    return 0;
#endif
}

} // namespace vitreo::winapi
