// SPDX-License-Identifier: LGPL-2.1-only
//
// ShaderCache identity: a cached translation is only valid for the Vitreo
// build that produced it and for the compile options it was produced under.
// Keying on the input bytecode alone let a new build with a changed
// translator reuse stale MSL. The build is named by the LC_UUID of the image
// that holds the cache code (Mach-O loader.h, uuid_command).

#include "graphics/shaders/shader_cache.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>

#include <dlfcn.h>
#include <mach-o/loader.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using vitreo::u8;
using vitreo::graphics::ShaderCache;
using vitreo::graphics::host_build_uuid;

namespace {

std::string hex(const u8* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// Read the LC_UUID of this test image independently of the implementation.
std::string expected_uuid_hex() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&expected_uuid_hex), &info) == 0) return {};
    const auto* mh = static_cast<const mach_header_64*>(info.dli_fbase);
    if (mh->magic != MH_MAGIC_64) return {};
    const auto* cmd = reinterpret_cast<const u8*>(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; ++i) {
        const auto* lc = reinterpret_cast<const load_command*>(cmd);
        if (lc->cmd == LC_UUID) {
            return hex(reinterpret_cast<const uuid_command*>(lc)->uuid, 16);
        }
        cmd += lc->cmdsize;
    }
    return {};
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        char tmpl[] = "/tmp/vitreo-ws4-shader-cache-XXXXXX";
        path = ::mkdtemp(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

const std::vector<u8> kInput = {0x44, 0x58, 0x42, 0x43, 1, 2, 3, 4, 5, 6, 7, 8};
const std::vector<u8> kMsl = {'k', 'e', 'r', 'n', 'e', 'l', ' ', 'v', 'o', 'i', 'd', 0};

}  // namespace

TEST(ShaderCacheIdentity, BuildUuidIsThisImage) {
    const auto& id = host_build_uuid();
    ASSERT_TRUE(id.ok);
    EXPECT_EQ(hex(id.bytes.data(), 16), expected_uuid_hex());
}

TEST(ShaderCacheIdentity, SaltChangesKey) {
    EXPECT_NE(ShaderCache::hash_input(kInput.data(), kInput.size(), "a"),
              ShaderCache::hash_input(kInput.data(), kInput.size(), "b"));
    EXPECT_EQ(ShaderCache::hash_input(kInput.data(), kInput.size(), "a"),
              ShaderCache::hash_input(kInput.data(), kInput.size(), "a"));
}

TEST(ShaderCacheIdentity, UuidInKey) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(kInput.data(), static_cast<CC_LONG>(kInput.size()), digest);
    const std::string plain = hex(digest, sizeof digest);
    EXPECT_NE(ShaderCache::hash_input(kInput.data(), kInput.size(), ""), plain);
    EXPECT_NE(ShaderCache::hash_input(kInput.data(), kInput.size()), plain);

    // The documented encoding: "vitreo-shader-cache\x01" || uuid || u64 salt
    // length (little-endian) || salt || data.
    std::vector<u8> buf;
    const char tag[] = "vitreo-shader-cache\x01";
    buf.insert(buf.end(), tag, tag + sizeof tag - 1);
    const auto& id = host_build_uuid();
    buf.insert(buf.end(), id.bytes.begin(), id.bytes.end());
    const std::string salt = "opts";
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<u8>(salt.size() >> (8 * i)));
    buf.insert(buf.end(), salt.begin(), salt.end());
    buf.insert(buf.end(), kInput.begin(), kInput.end());
    CC_SHA256(buf.data(), static_cast<CC_LONG>(buf.size()), digest);
    EXPECT_EQ(ShaderCache::hash_input(kInput.data(), kInput.size(), salt),
              hex(digest, sizeof digest));
}

TEST(ShaderCacheIdentity, StoreLookupRoundTrip) {
    TempDir dir;
    {
        ShaderCache cache(dir.path.string());
        cache.store(kInput.data(), kInput.size(), "s", kMsl);
    }
    ShaderCache fresh(dir.path.string());
    std::vector<u8> out;
    ASSERT_TRUE(fresh.lookup(kInput.data(), kInput.size(), "s", out));
    EXPECT_EQ(out, kMsl);
}

TEST(ShaderCacheIdentity, SaltFreeOverloadsRoundTrip) {
    TempDir dir;
    {
        ShaderCache cache(dir.path.string());
        cache.store(kInput.data(), kInput.size(), kMsl);
    }
    ShaderCache fresh(dir.path.string());
    std::vector<u8> out;
    ASSERT_TRUE(fresh.lookup(kInput.data(), kInput.size(), out));
    EXPECT_EQ(out, kMsl);
    // Salt-free means the empty salt, not "no identity".
    out.clear();
    EXPECT_TRUE(fresh.lookup(kInput.data(), kInput.size(), "", out));
}

TEST(ShaderCacheIdentity, OtherSaltMisses) {
    TempDir dir;
    ShaderCache cache(dir.path.string());
    cache.store(kInput.data(), kInput.size(), "a", kMsl);
    std::vector<u8> out;
    EXPECT_FALSE(cache.lookup(kInput.data(), kInput.size(), "b", out));
    ShaderCache fresh(dir.path.string());
    EXPECT_FALSE(fresh.lookup(kInput.data(), kInput.size(), "b", out));
}

TEST(ShaderCacheIdentity, PathUnderBuildDir) {
    TempDir dir;
    ShaderCache cache(dir.path.string());
    cache.store(kInput.data(), kInput.size(), "s", kMsl);
    const std::string h = ShaderCache::hash_input(kInput.data(), kInput.size(), "s");
    const auto expected =
        dir.path / expected_uuid_hex() / h.substr(0, 2) / (h + ".msl");
    EXPECT_TRUE(std::filesystem::exists(expected)) << expected;
}

TEST(ShaderCacheIdentity, OtherBuildDirectoryIgnoredAndCleared) {
    TempDir dir;
    // A file from another build, at the hash this build would use, in a
    // sibling uuid directory: must not be read, and clear() removes it.
    const std::string h = ShaderCache::hash_input(kInput.data(), kInput.size(), "s");
    const auto other = dir.path / std::string(32, '0') / h.substr(0, 2);
    std::filesystem::create_directories(other);
    std::ofstream(other / (h + ".msl")) << "stale";
    ShaderCache cache(dir.path.string());
    std::vector<u8> out;
    EXPECT_FALSE(cache.lookup(kInput.data(), kInput.size(), "s", out));
    cache.clear();
    EXPECT_FALSE(std::filesystem::exists(dir.path / std::string(32, '0')));
}

TEST(ShaderCacheIdentity, WriteIsAtomic) {
    TempDir dir;
    ShaderCache cache(dir.path.string());
    cache.store(kInput.data(), kInput.size(), "s", kMsl);
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir.path)) {
        EXPECT_EQ(e.path().filename().string().rfind(".tmp-", 0), std::string::npos)
            << e.path();
    }
    // A pre-planted empty file (what a torn non-atomic write leaves) is a miss.
    const std::string h = ShaderCache::hash_input(kInput.data(), kInput.size(), "z");
    const auto sub = dir.path / expected_uuid_hex() / h.substr(0, 2);
    std::filesystem::create_directories(sub);
    std::ofstream(sub / (h + ".msl")).close();
    ShaderCache fresh(dir.path.string());
    std::vector<u8> out;
    EXPECT_FALSE(fresh.lookup(kInput.data(), kInput.size(), "z", out));
}
