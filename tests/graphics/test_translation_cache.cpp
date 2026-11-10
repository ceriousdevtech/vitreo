// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_translation_cache.cpp
/// @brief graphics::TranslationCache: entries round-trip, a damaged entry is
///        a miss and is removed, keys separate their inputs, and
///        compile_spirv_to_msl through the cache returns what it translated.

#include "graphics/shaders/translation_cache.hpp"
#include "graphics/shaders/compile_profile.hpp"
#include "graphics/shaders/glsl_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace vitreo::graphics {
namespace {

namespace fs = std::filesystem;

class TranslationCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("vitreo-translation-cache-" + std::to_string(::getpid()));
        fs::remove_all(root);
        TranslationCache::instance().reset_for_test(root.string());
    }
    void TearDown() override {
        TranslationCache::instance().reset_for_test("");
        fs::remove_all(root);
    }
    fs::path root;
};

std::vector<u8> bytes(const char* text) {
    return std::vector<u8>(text, text + std::strlen(text));
}

TEST_F(TranslationCacheTest, StoredEntryIsFoundAgain) {
    TranslationCache& cache = TranslationCache::instance();
    ASSERT_TRUE(cache.enabled());
    const auto key = TranslationKey(TranslationKind::SpirvToMsl).text("input").finish();
    std::vector<u8> out;
    EXPECT_FALSE(cache.lookup(key, out));
    cache.store(key, bytes("payload"));
    ASSERT_TRUE(cache.lookup(key, out));
    EXPECT_EQ(out, bytes("payload"));
    // Under this translator's own directory.
    EXPECT_EQ(cache.directory(), (root / TranslationCache::translator_hash()).string());
}

TEST_F(TranslationCacheTest, KeysSeparateKindsAndInputBoundaries) {
    const auto a = TranslationKey(TranslationKind::SpirvToMsl).text("ab").text("c").finish();
    const auto b = TranslationKey(TranslationKind::SpirvToMsl).text("a").text("bc").finish();
    const auto c = TranslationKey(TranslationKind::DxilToDxbc).text("ab").text("c").finish();
    const auto a2 = TranslationKey(TranslationKind::SpirvToMsl).text("ab").text("c").finish();
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a, a2);
}

TEST_F(TranslationCacheTest, DamagedEntryIsAMissAndIsRemoved) {
    TranslationCache& cache = TranslationCache::instance();
    const auto key = TranslationKey(TranslationKind::SpirvToMsl).text("torn").finish();
    cache.store(key, bytes("a whole payload"));
    fs::path file;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file()) file = e.path();
    }
    ASSERT_FALSE(file.empty());
    // A torn write: the header survives, the payload is cut short.
    fs::resize_file(file, fs::file_size(file) - 3);
    std::vector<u8> out;
    EXPECT_FALSE(cache.lookup(key, out));
    EXPECT_FALSE(fs::exists(file));
    // A flipped payload byte (checksum).
    cache.store(key, bytes("a whole payload"));
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(-1, std::ios::end);
        f.put('X');
    }
    EXPECT_FALSE(cache.lookup(key, out));
    EXPECT_FALSE(fs::exists(file));
}

TEST_F(TranslationCacheTest, OffMeansNoEntries) {
    TranslationCache& cache = TranslationCache::instance();
    cache.reset_for_test("");
    EXPECT_FALSE(cache.enabled());
    const auto key = TranslationKey(TranslationKind::SpirvToMsl).text("x").finish();
    cache.store(key, bytes("y"));
    std::vector<u8> out;
    EXPECT_FALSE(cache.lookup(key, out));
}

TEST_F(TranslationCacheTest, SpirvToMslHitReturnsTheTranslation) {
    glsl_compiler_init();
    static constexpr const char* kSource = R"(#version 450
layout(local_size_x = 8, local_size_y = 4) in;
layout(std430, binding = 0) buffer B { uint v[]; } b;
void main() { b.v[gl_GlobalInvocationID.x] = gl_GlobalInvocationID.y; })";
    std::vector<u32> spirv;
    ASSERT_TRUE(compile_glsl_to_spirv(kSource, ShaderStage::Compute, {}, spirv));
    namespace cp = compile_profile;
    const cp::Snapshot s0 = cp::snapshot();
    const CompiledShader first =
        compile_spirv_to_msl(spirv.data(), spirv.size(), ShaderStage::Compute, {});
    const cp::Snapshot s1 = cp::snapshot();
    const CompiledShader second =
        compile_spirv_to_msl(spirv.data(), spirv.size(), ShaderStage::Compute, {});
    const cp::Snapshot s2 = cp::snapshot();
    ASSERT_TRUE(first.valid());
    EXPECT_EQ(s1.cache[cp::MslMiss] - s0.cache[cp::MslMiss], 1u);
    EXPECT_EQ(s2.cache[cp::MslHit] - s1.cache[cp::MslHit], 1u);
    EXPECT_EQ(std::string(first.msl_text()), std::string(second.msl_text()));
    EXPECT_EQ(second.workgroup_size_x, 8u);
    EXPECT_EQ(second.workgroup_size_y, 4u);
    EXPECT_EQ(second.bindings.size(), first.bindings.size());
    EXPECT_EQ(second.spirv_binary, first.spirv_binary);
    // Another option is another entry.
    MslCompileOptions other{};
    other.buffer_size_buffer_index = 24;
    (void)compile_spirv_to_msl(spirv.data(), spirv.size(), ShaderStage::Compute, other);
    EXPECT_EQ(cp::snapshot().cache[cp::MslMiss] - s2.cache[cp::MslMiss], 1u);
}

}  // namespace
}  // namespace vitreo::graphics
