// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_copy_buffer_region.cpp
/// @brief `copy_buffer_region` — the sub-range buffer copy.
///
/// `copy_buffer` copies a whole buffer. Three contracts need a sub-range:
/// D3D11 `CopySubresourceRegion` on a buffer, D3D11 `CopyStructureCount` (four
/// bytes of a UAV's hidden counter into an arbitrary offset of a destination
/// buffer, usually an indirect-args buffer), and D3D12 `CopyBufferRegion`.
///
/// The offsets are the whole point, so the test reads the destination back
/// *through the GPU*: a copy that ignored either offset, or that copied the
/// whole buffer, lands somewhere this can see. A test that only checked the
/// return value would pass against a backend that recorded nothing.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <cstdio>

using namespace vitreo::graphics;
using vitreo::u32;
using vitreo::u8;

namespace {

constexpr u32 kWords = 8;

[[nodiscard]] std::unique_ptr<BackendBuffer> make_buffer(
    GraphicsBackendDevice& device, const std::array<u32, kWords>& initial) {
    BackendBufferDesc desc{};
    desc.byte_width = sizeof(u32) * kWords;
    desc.shader_resource = true;
    desc.unordered_access = true;
    desc.cpu_writable = true;
    return device.create_buffer(desc, initial.data());
}

/// One thread per word: read the buffer through a t# binding and write the
/// value into the corresponding texel of a u# texture, which can be read back.
///
/// The binding indices come from the shared constants rather than from literals.
/// A previous test in this tree hardcoded them, sampled nothing, and still
/// reported the dispatch as successful — the constants exist so that a window
/// that moves cannot leave a test quietly reading the wrong slot.
[[nodiscard]] std::unique_ptr<BackendComputePipeline> readback_pipeline(
    GraphicsBackendDevice& device) {
    char source[1024];
    std::snprintf(source, sizeof(source), R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(
            const device uint* words [[buffer(%u)]],
            texture2d<float, access::write> u0 [[texture(%u)]],
            uint3 id [[thread_position_in_grid]]) {
            if (id.x < u0.get_width()) {
                u0.write(float4(float(words[id.x]) / 255.0, 0.0, 0.0, 1.0),
                         uint2(id.x, 0));
            }
        }
    )", static_cast<unsigned>(BACKEND_SRV_BUFFER_BASE),
        static_cast<unsigned>(BACKEND_UAV_TEXTURE_BASE));
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = source;
    return device.create_compute_pipeline(pd);
}

/// Every word of @p buffer, as the GPU sees it after everything recorded so far.
[[nodiscard]] ::testing::AssertionResult read_words(
    GraphicsBackendDevice& device, BackendBuffer* buffer,
    std::array<u32, kWords>& out) {
    auto pipeline = readback_pipeline(device);
    if (!pipeline || !pipeline->valid()) {
        return ::testing::AssertionFailure() << "readback pipeline";
    }
    BackendTexture2DDesc td{};
    td.width = kWords;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.unordered_access = true;
    td.render_target = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    if (!texture || !texture->valid()) {
        return ::testing::AssertionFailure() << "readback texture";
    }
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = kWords;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = buffer;
    call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
    if (!device.dispatch(call)) {
        return ::testing::AssertionFailure() << "dispatch";
    }
    std::array<u8, kWords * 4> pixels{};
    if (!texture->readback(0, pixels.data(), kWords * 4)) {
        return ::testing::AssertionFailure() << "readback";
    }
    for (u32 index = 0; index < kWords; ++index) {
        out[index] = pixels[index * 4];
    }
    return ::testing::AssertionSuccess();
}

} // namespace

// The offsets are real on both sides, and only the named range moves.
TEST(CopyBufferRegion, CopiesOnlyTheNamedRangeAtBothOffsets) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const std::array<u32, kWords> src_words{10, 11, 12, 13, 14, 15, 16, 17};
    const std::array<u32, kWords> dst_words{90, 91, 92, 93, 94, 95, 96, 97};
    auto src = make_buffer(device, src_words);
    auto dst = make_buffer(device, dst_words);
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);

    // Two words from src[3..4] into dst[2..3]. Deliberately different offsets:
    // an implementation that used one of them for both, or that ignored both
    // and copied from the start, produces a different and visible answer.
    ASSERT_TRUE(device.copy_buffer_region(dst.get(), 2 * sizeof(u32), src.get(),
                                          3 * sizeof(u32), 2 * sizeof(u32)));

    std::array<u32, kWords> read{};
    ASSERT_TRUE(read_words(device, dst.get(), read));
    const std::array<u32, kWords> expected{90, 91, 13, 14, 94, 95, 96, 97};
    for (u32 index = 0; index < kWords; ++index) {
        EXPECT_EQ(read[index], expected[index]) << "word " << index;
    }

    // The source is untouched.
    ASSERT_TRUE(read_words(device, src.get(), read));
    for (u32 index = 0; index < kWords; ++index) {
        EXPECT_EQ(read[index], src_words[index]) << "source word " << index;
    }
}

// A range that does not fit is refused rather than clamped: a short copy would
// leave the tail of the destination holding whatever was there before, which
// reads as a copy that worked.
TEST(CopyBufferRegion, RefusesRangesThatDoNotFitAndOverlappingSelfCopies) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const std::array<u32, kWords> words{0, 1, 2, 3, 4, 5, 6, 7};
    auto src = make_buffer(device, words);
    auto dst = make_buffer(device, words);
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);
    const vitreo::u64 size = sizeof(u32) * kWords;

    EXPECT_FALSE(device.copy_buffer_region(dst.get(), 0, src.get(), 0, 0))
        << "a zero-byte copy is a caller mistake, not a no-op to accept";
    EXPECT_FALSE(device.copy_buffer_region(dst.get(), 4, src.get(), 0, size))
        << "destination range runs past the end";
    EXPECT_FALSE(device.copy_buffer_region(dst.get(), 0, src.get(), 4, size))
        << "source range runs past the end";
    EXPECT_FALSE(device.copy_buffer_region(dst.get(), size + 4, src.get(), 0, 4))
        << "destination offset is past the end";
    EXPECT_FALSE(device.copy_buffer_region(dst.get(), 0, nullptr, 0, 4));
    EXPECT_FALSE(device.copy_buffer_region(nullptr, 0, src.get(), 0, 4));

    // Overlapping within one buffer has an undefined result, so it is refused
    // rather than producing one of the possible answers.
    EXPECT_FALSE(device.copy_buffer_region(src.get(), 0, src.get(), 4, 3 * 4))
        << "overlapping self-copy";
    // Non-overlapping within one buffer is fine.
    EXPECT_TRUE(device.copy_buffer_region(src.get(), 0, src.get(), 16, 8));
}

// -----------------------------------------------------------------------------
// fill_buffer_region: the source-less clear (a uniform-byte UAV clear).
// -----------------------------------------------------------------------------

namespace {

constexpr u32 kBytes = kWords * 4;

/// Every BYTE of @p buffer, one texel per byte, as the GPU sees it after
/// everything recorded so far: a fill that wrote only some bytes of each word,
/// or the wrong value, is visible here where a word compare could miss it.
[[nodiscard]] ::testing::AssertionResult read_bytes(
    GraphicsBackendDevice& device, BackendBuffer* buffer,
    std::array<u8, kBytes>& out) {
    char source[1024];
    std::snprintf(source, sizeof(source), R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(
            const device uchar* bytes [[buffer(%u)]],
            texture2d<float, access::write> u0 [[texture(%u)]],
            uint3 id [[thread_position_in_grid]]) {
            if (id.x < u0.get_width()) {
                u0.write(float4(float(bytes[id.x]) / 255.0, 0.0, 0.0, 1.0),
                         uint2(id.x, 0));
            }
        }
    )", static_cast<unsigned>(BACKEND_SRV_BUFFER_BASE),
        static_cast<unsigned>(BACKEND_UAV_TEXTURE_BASE));
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = source;
    auto pipeline = device.create_compute_pipeline(pd);
    if (!pipeline || !pipeline->valid()) {
        return ::testing::AssertionFailure() << "readback pipeline";
    }
    BackendTexture2DDesc td{};
    td.width = kBytes;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.unordered_access = true;
    td.render_target = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    if (!texture || !texture->valid()) {
        return ::testing::AssertionFailure() << "readback texture";
    }
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = kBytes;
    call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = buffer;
    call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
    if (!device.dispatch(call)) {
        return ::testing::AssertionFailure() << "dispatch";
    }
    std::array<u8, kBytes * 4> pixels{};
    if (!texture->readback(0, pixels.data(), kBytes * 4)) {
        return ::testing::AssertionFailure() << "readback";
    }
    for (u32 index = 0; index < kBytes; ++index) {
        out[index] = pixels[index * 4];
    }
    return ::testing::AssertionSuccess();
}

[[nodiscard]] std::array<u8, kBytes> bytes_of(
    const std::array<u32, kWords>& words) {
    std::array<u8, kBytes> out{};
    for (u32 i = 0; i < kBytes; ++i) {
        out[i] = static_cast<u8>(words[i / 4] >> (8 * (i % 4)));
    }
    return out;
}

} // namespace

// Every byte of the named range takes the value, and nothing outside it moves.
TEST(FillBufferRegion, FillsEveryByteOfOnlyTheNamedRange) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const std::array<u32, kWords> words{0x04030201u, 0x08070605u, 0x0c0b0a09u,
                                        0x100f0e0du, 0x14131211u, 0x18171615u,
                                        0x1c1b1a19u, 0x201f1e1du};
    auto dst = make_buffer(device, words);
    ASSERT_NE(dst, nullptr);

    // Bytes 8..19 (words 2..4): an offset and a length that are both
    // non-zero and different, so ignoring either is visible.
    ASSERT_TRUE(device.fill_buffer_region(dst.get(), 8, 12, 0xA5));

    std::array<u8, kBytes> read{};
    ASSERT_TRUE(read_bytes(device, dst.get(), read));
    std::array<u8, kBytes> expected = bytes_of(words);
    for (u32 i = 8; i < 20; ++i) expected[i] = 0xA5;
    for (u32 index = 0; index < kBytes; ++index) {
        EXPECT_EQ(read[index], expected[index]) << "byte " << index;
    }
}

// Refused, not clamped or rounded: the contract promises four-byte
// granularity, and a caller handed false writes the range another way.
TEST(FillBufferRegion, RefusesRangesThatDoNotFitOrAreNotFourByteGranular) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const std::array<u32, kWords> words{1, 2, 3, 4, 5, 6, 7, 8};
    auto dst = make_buffer(device, words);
    ASSERT_NE(dst, nullptr);

    EXPECT_FALSE(device.fill_buffer_region(dst.get(), 0, 0, 0))
        << "zero bytes";
    EXPECT_FALSE(device.fill_buffer_region(dst.get(), 4, kBytes, 0))
        << "range runs past the end";
    EXPECT_FALSE(device.fill_buffer_region(dst.get(), kBytes + 4, 4, 0))
        << "offset past the end";
    EXPECT_FALSE(device.fill_buffer_region(dst.get(), 2, 4, 0))
        << "offset not a multiple of four";
    EXPECT_FALSE(device.fill_buffer_region(dst.get(), 4, 6, 0))
        << "length not a multiple of four";
    EXPECT_FALSE(device.fill_buffer_region(nullptr, 0, 4, 0));

    // None of the refusals recorded anything.
    std::array<u8, kBytes> read{};
    ASSERT_TRUE(read_bytes(device, dst.get(), read));
    const std::array<u8, kBytes> expected = bytes_of(words);
    for (u32 index = 0; index < kBytes; ++index) {
        EXPECT_EQ(read[index], expected[index]) << "byte " << index;
    }
}
