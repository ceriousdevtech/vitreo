// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// On-disk, section-aligned image layouts that back SEC_IMAGE mappings.

#include "image_layout_file.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace vitreo::pe {

namespace {

/// Bump when the layout a given PE produces changes, so stale files from an
/// older mapper are never mapped.
constexpr char kLayoutVersion[] = "vitreo-image-layout-v1";

std::string layout_key(std::span<const u8> pe_data, u32 image_size) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, kLayoutVersion, sizeof(kLayoutVersion));
    CC_SHA256_Update(&ctx, &image_size, sizeof(image_size));
    constexpr usize kChunk = usize{1} << 30;
    for (usize off = 0; off < pe_data.size(); off += kChunk) {
        const usize n = std::min(kChunk, pe_data.size() - off);
        CC_SHA256_Update(&ctx, pe_data.data() + off, static_cast<CC_LONG>(n));
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return std::string(hex, CC_SHA256_DIGEST_LENGTH * 2);
}

int open_existing(const std::string& path, u32 image_size) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != static_cast<off_t>(image_size)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool write_all(int fd, const u8* src, usize size, off_t at) {
    while (size != 0) {
        const ssize_t n = ::pwrite(fd, src, size, at);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        src += n;
        size -= static_cast<usize>(n);
        at += n;
    }
    return true;
}

}  // namespace

std::string image_layout_cache_dir() {
    if (const char* off = std::getenv("VITREO_IMAGE_FILE_BACKING");
        off != nullptr && off[0] == '0') {
        return {};
    }
    if (const char* dir = std::getenv("VITREO_IMAGE_LAYOUT_CACHE_DIR");
        dir != nullptr && dir[0] != '\0') {
        return dir;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] != '/') return {};
    return std::string(home) + "/Library/Caches/Vitreo/image-layouts";
}

int open_image_layout_file(std::span<const u8> pe_data, u32 image_size,
                           std::span<const ImageLayoutCopy> copies) {
    if (image_size == 0) return -1;
    for (const auto& c : copies) {
        if (c.file_offset > pe_data.size() ||
            c.size > pe_data.size() - c.file_offset ||
            c.rva > image_size || c.size > usize{image_size} - c.rva) {
            return -1;
        }
    }
    const std::string dir = image_layout_cache_dir();
    if (dir.empty()) return -1;

    const std::string path = dir + "/" + layout_key(pe_data, image_size) + ".img";
    if (const int fd = open_existing(path, image_size); fd >= 0) return fd;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string tmp = dir + "/.tmp-XXXXXX";
    const int wfd = ::mkstemp(tmp.data());
    if (wfd < 0) return -1;
    bool ok = ::ftruncate(wfd, static_cast<off_t>(image_size)) == 0;
    for (const auto& c : copies) {
        if (!ok) break;
        ok = write_all(wfd, pe_data.data() + c.file_offset, c.size,
                       static_cast<off_t>(c.rva));
    }
    ok = ok && ::fchmod(wfd, 0444) == 0;
    ::close(wfd);
    // RENAME_EXCL: a concurrent loader that published first wins, and both
    // processes then map the same inode.
    if (!ok || ::renamex_np(tmp.c_str(), path.c_str(), RENAME_EXCL) != 0) {
        const bool lost_race = ok && errno == EEXIST;
        ::unlink(tmp.c_str());
        if (!lost_race) return -1;
    }
    return open_existing(path, image_size);
}

}  // namespace vitreo::pe
