// Implementation of the shared streaming SHA-1 diagnostic.
// See write_sha1_debug.hpp for the contract.

#include "winapi/common/write_sha1_debug.hpp"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace vitreo::winapi::debug {

#ifdef __APPLE__

namespace {

struct WriteDigest {
    CC_SHA1_CTX ctx{};
    uint64_t    bytes      = 0;
    uint64_t    next_emit  = 1ULL << 20;  // first partial at 1 MiB
    std::string path;
    WriteDigest() { CC_SHA1_Init(&ctx); }
};

std::mutex& write_sha1_mutex() {
    static std::mutex m; return m;
}

std::unordered_map<Handle, std::unique_ptr<WriteDigest>>& write_sha1_map() {
    static std::unordered_map<Handle, std::unique_ptr<WriteDigest>> m; return m;
}

void emit_partial_locked(const WriteDigest& d) {
    // Snapshot context so we can keep updating after emitting.
    CC_SHA1_CTX snap = d.ctx;
    unsigned char dig[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(dig, &snap);
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1] = {};
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
    std::fprintf(stderr,
                 "[file-sha1] bytes=%llu sha1=%s path=%s (partial)\n",
                 static_cast<unsigned long long>(d.bytes),
                 hex,
                 d.path.c_str());
}

} // namespace

bool write_sha1_enabled() {
    static const bool on = (std::getenv("VITREO_WRITE_SHA1") != nullptr);
    return on;
}

void write_sha1_open(Handle h, std::string_view path) {
    if (!write_sha1_enabled()) return;
    std::lock_guard<std::mutex> lk(write_sha1_mutex());
    auto d = std::make_unique<WriteDigest>();
    d->path = std::string(path);
    std::fprintf(stderr, "[file-sha1] OPEN  handle=0x%x path=%s\n",
                 h, d->path.c_str());
    write_sha1_map()[h] = std::move(d);
}

void write_sha1_update(Handle h, const void* data, std::size_t n) {
    if (!write_sha1_enabled() || n == 0) return;
    std::lock_guard<std::mutex> lk(write_sha1_mutex());
    auto it = write_sha1_map().find(h);
    if (it == write_sha1_map().end()) return;
    auto& d = *it->second;
    CC_SHA1_Update(&d.ctx, data, static_cast<CC_LONG>(n));
    d.bytes += n;
    if (d.bytes >= d.next_emit) {
        emit_partial_locked(d);
        // Next emission at the next 1 MiB boundary past current total.
        while (d.next_emit <= d.bytes) d.next_emit += (1ULL << 20);
    }
}

void write_sha1_close(Handle h) {
    if (!write_sha1_enabled()) return;
    std::unique_ptr<WriteDigest> d;
    {
        std::lock_guard<std::mutex> lk(write_sha1_mutex());
        auto it = write_sha1_map().find(h);
        if (it == write_sha1_map().end()) return;
        d = std::move(it->second);
        write_sha1_map().erase(it);
    }
    unsigned char dig[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(dig, &d->ctx);
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1] = {};
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
    std::fprintf(stderr,
                 "[file-sha1] handle=0x%x bytes=%llu sha1=%s path=%s\n",
                 h,
                 static_cast<unsigned long long>(d->bytes),
                 hex,
                 d->path.c_str());
}

// ---------------------------------------------------------------------------
// Read-side mirror of the API (gated by VITREO_READ_SHA1=1).
// ---------------------------------------------------------------------------

namespace {

std::mutex& read_sha1_mutex() {
    static std::mutex m; return m;
}

std::unordered_map<Handle, std::unique_ptr<WriteDigest>>& read_sha1_map() {
    static std::unordered_map<Handle, std::unique_ptr<WriteDigest>> m; return m;
}

void emit_read_partial_locked(const WriteDigest& d) {
    CC_SHA1_CTX snap = d.ctx;
    unsigned char dig[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(dig, &snap);
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1] = {};
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
    std::fprintf(stderr,
                 "[file-sha1-read] bytes=%llu sha1=%s path=%s (partial)\n",
                 static_cast<unsigned long long>(d.bytes),
                 hex,
                 d.path.c_str());
}

} // namespace

bool read_sha1_enabled() {
    static const bool on = (std::getenv("VITREO_READ_SHA1") != nullptr);
    return on;
}

void read_sha1_open(Handle h, std::string_view path) {
    if (!read_sha1_enabled()) return;
    std::lock_guard<std::mutex> lk(read_sha1_mutex());
    auto d = std::make_unique<WriteDigest>();
    d->path = std::string(path);
    std::fprintf(stderr, "[file-sha1-read] OPEN  handle=0x%x path=%s\n",
                 h, d->path.c_str());
    read_sha1_map()[h] = std::move(d);
}

void read_sha1_update(Handle h, const void* data, std::size_t n) {
    if (!read_sha1_enabled() || n == 0) return;
    std::lock_guard<std::mutex> lk(read_sha1_mutex());
    auto it = read_sha1_map().find(h);
    if (it == read_sha1_map().end()) return;
    auto& d = *it->second;
    CC_SHA1_Update(&d.ctx, data, static_cast<CC_LONG>(n));
    d.bytes += n;
    if (d.bytes >= d.next_emit) {
        emit_read_partial_locked(d);
        while (d.next_emit <= d.bytes) d.next_emit += (1ULL << 20);
    }
}

void read_sha1_close(Handle h) {
    if (!read_sha1_enabled()) return;
    std::unique_ptr<WriteDigest> d;
    {
        std::lock_guard<std::mutex> lk(read_sha1_mutex());
        auto it = read_sha1_map().find(h);
        if (it == read_sha1_map().end()) return;
        d = std::move(it->second);
        read_sha1_map().erase(it);
    }
    if (d->bytes == 0) return;  // never read from -- skip noise
    unsigned char dig[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1_Final(dig, &d->ctx);
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1] = {};
    for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
    std::fprintf(stderr,
                 "[file-sha1-read] handle=0x%x bytes=%llu sha1=%s path=%s\n",
                 h,
                 static_cast<unsigned long long>(d->bytes),
                 hex,
                 d->path.c_str());
}

// ---------------------------------------------------------------------------
// Per-handle watch (VITREO_WATCH_PATH=<substring>).
// ---------------------------------------------------------------------------

namespace {

std::mutex& watch_mutex() { static std::mutex m; return m; }
std::unordered_map<Handle, std::string>& watch_map() {
    static std::unordered_map<Handle, std::string> m; return m;
}
const char* watch_substr() {
    static const char* s = std::getenv("VITREO_WATCH_PATH");
    return s;
}

} // namespace

bool watch_enabled() { return watch_substr() != nullptr; }

void watch_open(Handle h, std::string_view path) {
    if (!watch_enabled()) return;
    const char* needle = watch_substr();
    if (!needle || !*needle) return;
    std::string p(path);
    std::string folded_path = p;
    std::string folded_needle(needle);
    auto fold_ascii = [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    };
    std::transform(folded_path.begin(), folded_path.end(), folded_path.begin(),
                   fold_ascii);
    std::transform(folded_needle.begin(), folded_needle.end(),
                   folded_needle.begin(), fold_ascii);
    if (folded_path.find(folded_needle) == std::string::npos) return;
    std::lock_guard<std::mutex> lk(watch_mutex());
    watch_map()[h] = std::move(p);
    std::fprintf(stderr, "[watch] OPEN  handle=0x%x path=%s\n",
                 h, watch_map()[h].c_str());
}

bool is_watched(Handle h) {
    if (!watch_enabled()) return false;
    std::lock_guard<std::mutex> lk(watch_mutex());
    return watch_map().find(h) != watch_map().end();
}

const char* watched_path(Handle h) {
    if (!watch_enabled()) return nullptr;
    std::lock_guard<std::mutex> lk(watch_mutex());
    auto it = watch_map().find(h);
    if (it == watch_map().end()) return nullptr;
    return it->second.c_str();
}

void watch_close(Handle h) {
    if (!watch_enabled()) return;
    std::lock_guard<std::mutex> lk(watch_mutex());
    auto it = watch_map().find(h);
    if (it == watch_map().end()) return;
    std::fprintf(stderr, "[watch] CLOSE handle=0x%x path=%s\n",
                 h, it->second.c_str());
    watch_map().erase(it);
}

#else

bool write_sha1_enabled() { return false; }
void write_sha1_open(Handle, std::string_view) {}
void write_sha1_update(Handle, const void*, std::size_t) {}
void write_sha1_close(Handle) {}

bool read_sha1_enabled() { return false; }
void read_sha1_open(Handle, std::string_view) {}
void read_sha1_update(Handle, const void*, std::size_t) {}
void read_sha1_close(Handle) {}

bool watch_enabled() { return false; }
void watch_open(Handle, std::string_view) {}
bool is_watched(Handle) { return false; }
const char* watched_path(Handle) { return nullptr; }
void watch_close(Handle) {}

#endif

} // namespace vitreo::winapi::debug
