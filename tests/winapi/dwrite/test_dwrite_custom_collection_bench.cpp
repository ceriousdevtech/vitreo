// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_guest_font_loader.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "capsule/vfs/host_filesystem.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

// Repeated CreateCustomFontCollection over the same guest-loader keys, then
// each face's GetFiles and a whole-file read of it through the local loader
// (the Chromium renderer + Skia pattern). Run with
// --gtest_also_run_disabled_tests --gtest_filter='*BenchRepeatedCustomCollection*'.
TEST_F(DWriteManagerTest, DISABLED_BenchRepeatedCustomCollection) {
    char pattern[] = "/tmp/vitreo-dwrite-bench-XXXXXX";
    char* dir = ::mkdtemp(pattern);
    ASSERT_NE(dir, nullptr);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{dir};
    std::filesystem::create_directories(std::filesystem::path(dir) / "Windows/Fonts");
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(dir)).is_ok());
    const char* env_iterations = std::getenv("VITREO_DWRITE_BENCH_ITERATIONS");
    const int iterations = env_iterations ? std::atoi(env_iterations) : 20;
    const std::u16string families[]{u"Arial", u"Georgia", u"Courier New",
                                    u"Times New Roman", u"Verdana", u"Trebuchet MS"};
    std::vector<std::vector<u8>> bytes;
    for (const auto& family : families) {
        bytes.push_back(system_font_bytes(family));
        ASSERT_FALSE(bytes.back().empty());
    }

    const auto run = [&](bool reuse, bool new_key_each_time) -> double {
        DWriteManager fresh{memory, true, &vfs, &gdi};
        fresh.set_method_thunks(method_thunks());
        fresh.set_font_reuse_enabled(reuse);
        GuestFontLoader loader(memory, cpu, fresh, true);
        (void)memory.unmap_region(GuestFontLoader::BASE, GuestFontLoader::REGION_SIZE);
        EXPECT_TRUE(loader.set_up(IID_ADDRESS));
        for (const auto& file : bytes) EXPECT_TRUE(loader.add_file(file, 1));
        constexpr GuestAddr O = GuestFontLoader::OUT;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            auto [hr, collection] =
                loader.create_collection(new_key_each_time ? 100 + i : 1);
            EXPECT_EQ(hr, 0u);
            EXPECT_EQ(loader.invoke(&DWriteManager::impl_GetFontFamilyCount, {collection}),
                      std::size(families));
            for (u32 index = 0; index < std::size(families); ++index) {
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_GetFontFamily,
                                        {collection, index, O}), 0u);
                const GuestAddr family = loader.read_ptr(O);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_FamilyGetFont, {family, 0, O}), 0u);
                const GuestAddr font = loader.read_ptr(O);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_FontCreateFontFace, {font, O}), 0u);
                const GuestAddr face = loader.read_ptr(O);
                (void)memory.write_u32(O + 0x10, 1);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_FaceGetFiles,
                                        {face, O + 0x10, O + 0x18}), 0u);
                const GuestAddr file = loader.read_ptr(O + 0x18);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_FileGetReferenceKey,
                                        {file, O + 0x20, O + 0x28}), 0u);
                const GuestAddr key = loader.read_ptr(O + 0x20);
                const u32 key_size = memory.read_u32(O + 0x28).value_or(0);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_FileGetLoader, {file, O}), 0u);
                const GuestAddr local = loader.read_ptr(O);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_LocalLoaderCreateStreamFromKey,
                                        {local, key, key_size, O}), 0u);
                const GuestAddr stream = loader.read_ptr(O);
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_StreamGetFileSize,
                                        {stream, O + 0x30}), 0u);
                const u64 size = memory.read_u64(O + 0x30).value_or(0);
                EXPECT_EQ(size, bytes[index].size());
                EXPECT_EQ(loader.invoke(&DWriteManager::impl_StreamReadFileFragment,
                                        {stream, O + 0x38, 0, size, O + 0x40}), 0u);
                (void)loader.invoke(&DWriteManager::impl_StreamReleaseFileFragment,
                                    {stream, loader.read_ptr(O + 0x40)});
                for (const GuestAddr object : {stream, local, file, face, font, family})
                    (void)loader.invoke(&DWriteManager::impl_Release, {object});
            }
            (void)loader.invoke(&DWriteManager::impl_Release, {collection});
        }
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        EXPECT_TRUE(loader.violations.empty());
        return ms / iterations;
    };
    for (const bool new_key : {false, true}) {
        const char* pattern_name = new_key ? "new-collection-key" : "same-collection-key";
        (void)run(false, new_key);  // warm CoreText and the published files
        (void)run(true, new_key);
        for (int pair = 0; pair < 5; ++pair) {
            const double legacy = run(false, new_key);
            const double reuse = run(true, new_key);
            std::printf("[dwrite-bench] %s pair %d legacy=%.3f ms/iter reuse=%.3f ms/iter "
                        "improvement=%.1f%%\n", pattern_name, pair, legacy, reuse,
                        100.0 * (legacy - reuse) / legacy);
        }
    }
}
