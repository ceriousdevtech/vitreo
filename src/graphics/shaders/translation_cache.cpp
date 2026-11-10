// SPDX-License-Identifier: LGPL-2.1-only

/// @file translation_cache.cpp
/// @brief TranslationCache (translation_cache.hpp).

#include "translation_cache.hpp"
#include "vitreo_translator_hash.hpp"  // generated: VITREO_TRANSLATOR_HASH

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

namespace vitreo::graphics {

namespace {

constexpr char kMagic[8] = {'V', 'T', 'R', 'C', 'A', 'C', 'H', '1'};

/// File: magic, u32 header version (1), u32 payload bytes, the 32-byte key,
/// the payload's 32-byte SHA-256, the payload.
constexpr usize kHeaderBytes = 8 + 4 + 4 + 32 + 32;

std::array<u8, 32> sha256(const void* data, usize size) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    const auto* p = static_cast<const u8*>(data);
    usize off = 0;
    while (off < size) {
        const usize n = std::min<usize>(size - off, usize{1} << 30);
        CC_SHA256_Update(&ctx, p + off, static_cast<CC_LONG>(n));
        off += n;
    }
    std::array<u8, 32> digest{};
    CC_SHA256_Final(digest.data(), &ctx);
    return digest;
}

std::string hex(const u8* p, usize n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (usize i = 0; i < n; ++i) {
        out += kDigits[p[i] >> 4];
        out += kDigits[p[i] & 15];
    }
    return out;
}

bool write_all(int fd, const u8* data, usize size) {
    while (size != 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<usize>(n);
    }
    return true;
}

}  // namespace

TranslationKey::TranslationKey(TranslationKind kind) {
    static constexpr char kTag[] = "vitreo-translation-cache\x01";
    m_material.insert(m_material.end(), kTag, kTag + sizeof kTag - 1);
    const char* translator = TranslationCache::translator_hash();
    bytes(translator, std::strlen(translator));
    u32_value(static_cast<u32>(kind));
}

TranslationKey& TranslationKey::bytes(const void* data, usize size) {
    const u64 n = size;
    const auto* len = reinterpret_cast<const u8*>(&n);
    m_material.insert(m_material.end(), len, len + 8);
    const auto* p = static_cast<const u8*>(data);
    m_material.insert(m_material.end(), p, p + size);
    return *this;
}

std::array<u8, 32> TranslationKey::finish() const {
    return sha256(m_material.data(), m_material.size());
}

const char* TranslationCache::translator_hash() { return VITREO_TRANSLATOR_HASH; }

TranslationCache& TranslationCache::instance() {
    static TranslationCache* cache = new TranslationCache();
    return *cache;
}

TranslationCache::TranslationCache() {
    std::string root;
    if (const char* forced = std::getenv("VITREO_SHADER_CACHE_DIR");
        forced != nullptr && forced[0] != '\0') {
        root = std::strcmp(forced, "off") == 0 ? std::string() : std::string(forced);
    } else if (const char* scope = std::getenv("VITREO_IPC_SCOPE");
               scope != nullptr && scope[0] != '\0') {
        root = std::string(scope) + "/cache/shader-translations";
    }
    open(root);
}

void TranslationCache::open(const std::string& root) {
    std::lock_guard lock(m_mutex);
    m_root = root;
    m_dir.clear();
    if (root.empty()) return;
    std::error_code ec;
    const std::string dir = root + "/" + translator_hash();
    std::filesystem::create_directories(dir, ec);
    if (ec) return;
    m_dir = dir;
    // Bound the cache once per process, off the creating threads.
    std::thread([this] { prune(); }).detach();
}

void TranslationCache::reset_for_test(const std::string& root) { open(root); }

std::string TranslationCache::directory() const {
    std::lock_guard lock(m_mutex);
    return m_dir;
}

std::string TranslationCache::path_of(const std::array<u8, 32>& key) const {
    const std::string name = hex(key.data(), key.size());
    return m_dir + "/" + name.substr(0, 2) + "/" + name + ".bin";
}

bool TranslationCache::lookup(const std::array<u8, 32>& key, std::vector<u8>& payload) {
    std::string path;
    {
        std::lock_guard lock(m_mutex);
        if (m_dir.empty()) return false;
        path = path_of(key);
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    std::vector<u8> file;
    bool read_ok = ::fstat(fd, &st) == 0 && st.st_size >= static_cast<off_t>(kHeaderBytes) &&
                   st.st_size < (off_t{1} << 31);
    if (read_ok) {
        file.resize(static_cast<usize>(st.st_size));
        usize at = 0;
        while (at < file.size()) {
            const ssize_t n = ::read(fd, file.data() + at, file.size() - at);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            at += static_cast<usize>(n);
        }
        read_ok = at == file.size();
    }
    ::close(fd);
    u32 version = 0;
    u32 size = 0;
    if (read_ok) {
        std::memcpy(&version, file.data() + 8, 4);
        std::memcpy(&size, file.data() + 12, 4);
    }
    const bool valid =
        read_ok && std::memcmp(file.data(), kMagic, 8) == 0 && version == 1 &&
        size == file.size() - kHeaderBytes && std::memcmp(file.data() + 16, key.data(), 32) == 0 &&
        sha256(file.data() + kHeaderBytes, size) ==
            [&] {
                std::array<u8, 32> d{};
                std::memcpy(d.data(), file.data() + 48, 32);
                return d;
            }();
    if (!valid) {
        // Torn, truncated or foreign: drop it, the caller translates again
        // and stores a whole one.
        (void)::unlink(path.c_str());
        return false;
    }
    payload.assign(file.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes), file.end());
    // Pruning removes the least recently written files first: an entry
    // still in use is rewritten-in-time at most once a day.
    if (std::time(nullptr) - st.st_mtimespec.tv_sec > 24 * 3600) {
        (void)::utimes(path.c_str(), nullptr);
    }
    return true;
}

void TranslationCache::store(const std::array<u8, 32>& key, const std::vector<u8>& payload) {
    std::string path;
    {
        std::lock_guard lock(m_mutex);
        if (m_dir.empty()) return;
        path = path_of(key);
    }
    const std::string dir = path.substr(0, path.rfind('/'));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;
    std::vector<u8> file(kHeaderBytes);
    std::memcpy(file.data(), kMagic, 8);
    const u32 version = 1;
    const u32 size = static_cast<u32>(payload.size());
    std::memcpy(file.data() + 8, &version, 4);
    std::memcpy(file.data() + 12, &size, 4);
    std::memcpy(file.data() + 16, key.data(), 32);
    const std::array<u8, 32> digest = sha256(payload.data(), payload.size());
    std::memcpy(file.data() + 48, digest.data(), 32);
    file.insert(file.end(), payload.begin(), payload.end());
    // A temporary in the same directory, then rename(2): atomic, so a
    // concurrent reader sees the old entry, the new one or none. No fsync:
    // a power loss can leave a torn file, which lookup's checksum refuses.
    std::string tmp = dir + "/.tmp-XXXXXX";
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) return;
    const bool ok = write_all(fd, file.data(), file.size());
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        (void)::unlink(tmp.c_str());
        return;
    }
    if (m_stores.fetch_add(1, std::memory_order_relaxed) % kPruneEvery == kPruneEvery - 1) {
        std::thread([this] { prune(); }).detach();
    }
}

void TranslationCache::prune() {
    namespace fs = std::filesystem;
    std::string root;
    {
        std::lock_guard lock(m_mutex);
        root = m_root;
    }
    if (root.empty()) return;
    struct Entry {
        fs::file_time_type time;
        u64 size;
        fs::path path;
    };
    std::vector<Entry> entries;
    u64 total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const u64 size = it->file_size(ec);
        if (ec) continue;
        const auto time = it->last_write_time(ec);
        if (ec) continue;
        // A temporary older than a minute was left by a process that died
        // mid-write.
        if (it->path().filename().string().starts_with(".tmp-")) {
            if (fs::file_time_type::clock::now() - time > std::chrono::minutes(1)) {
                fs::remove(it->path(), ec);
            }
            continue;
        }
        total += size;
        entries.push_back({time, size, it->path()});
    }
    if (total <= kMaxBytes) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (const Entry& e : entries) {
        if (total <= kMaxBytes / 4 * 3) break;
        fs::remove(e.path, ec);
        if (!ec) total -= e.size;
    }
}

}  // namespace vitreo::graphics
