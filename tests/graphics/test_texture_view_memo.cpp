// SPDX-License-Identifier: LGPL-2.1-only
//
// A texture remembers its last shader-view resolution (MetalBackendTexture2D
// ::native_shader_handle), because a renderer binds the same views draw after
// draw. What must not change is which storage a bind reads. A WRITE_DISCARD
// rename (discard_storage) moves the texture to new storage and releases the
// old storage's views; after a few renames the parked storage is recycled, so
// an old parent comes back. Each round here writes new texels after a rename
// and reads them back through a swizzled view, which is a real MTLTexture
// view rather than the parent: a stale view would read the previous round's
// texels, and a released one faults (negative control: without the memo reset
// in discard_storage this test dies with SIGSEGV, and under NSZombieEnabled
// with "-[AGXG16XFamilyTexture retain]: message sent to deallocated
// instance").

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <string>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

TEST(TextureViewMemo, EveryRenameIsReadThroughAViewOfTheNewStorage) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    // out[i] = the (swizzled) texel 0 the kernel sees.
    const std::string msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(texture2d<float> tex [[texture(" +
        std::to_string(BACKEND_SRV_TEXTURE_BASE) + ")]],\n"
        "    device float4* out [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) + ")]],\n"
        "    uint3 id [[thread_position_in_grid]]) {\n"
        "    if (id.x == 0) out[0] = tex.read(uint2(0, 0));\n}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    pd.cache_key = 0x7E3D0001u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid());
    // Work that does not touch the texture: it moves the stream on so the GPU
    // can retire a parked storage.
    BackendComputePipelineDesc other_pd{};
    other_pd.compute_entry_point = "main0";
    other_pd.compute_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(device float* out [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) + ")]]) { out[0] = 0.0f; }\n";
    other_pd.cache_key = 0x7E3D0002u;
    auto other = device.create_compute_pipeline(other_pd);
    ASSERT_TRUE(other && other->valid());
    BackendBufferDesc scratch_desc{};
    scratch_desc.byte_width = 16;
    scratch_desc.unordered_access = true;
    auto scratch = device.create_buffer(scratch_desc, nullptr);
    ASSERT_TRUE(scratch);
    BackendDispatchCall unrelated{};
    unrelated.pipeline = other.get();
    unrelated.group_count_x = unrelated.group_count_y = 1;
    unrelated.group_count_z = 1;
    unrelated.buffers[BACKEND_UAV_BUFFER_BASE] = scratch.get();

    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.cpu_writable = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(texture && texture->valid());

    for (u32 round = 0; round < 8; ++round) {
        SCOPED_TRACE(round);
        if (round != 0) {
            // Rename, let the GPU retire the parked storage, rename again: the
            // second rename takes back the storage the last round read, so
            // this round's parent is the one the memo last saw -- with its
            // views released.
            ASSERT_TRUE(texture->discard_storage());
            ASSERT_TRUE(device.dispatch(unrelated));
            ASSERT_TRUE(device.wait_idle());
            ASSERT_TRUE(texture->discard_storage());
        }
        const u8 red = static_cast<u8>(0x20 + round * 0x18);
        std::array<u8, 4 * 4 * 4> texels{};
        for (usize i = 0; i < texels.size(); i += 4) {
            texels[i + 0] = red;
            texels[i + 1] = 0x77;  // dropped by the R001 swizzle
            texels[i + 2] = 0x99;
            texels[i + 3] = 0x10;
        }
        ASSERT_TRUE(texture->update(0, texels.data(), 4 * 4));

        BackendBufferDesc bd{};
        bd.byte_width = 16;
        bd.unordered_access = bd.shader_resource = true;
        const std::array<float, 4> poison{-1.0f, -1.0f, -1.0f, -1.0f};
        auto result = device.create_buffer(bd, poison.data());
        ASSERT_TRUE(result);
        BackendDispatchCall call{};
        call.pipeline = pipeline.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.textures[BACKEND_SRV_TEXTURE_BASE] = texture.get();
        auto& view = call.texture_views[BACKEND_SRV_TEXTURE_BASE];
        view.dimension = BackendTextureViewDimension::Texture2D;
        view.mip_levels = 1;
        view.array_size = 1;
        view.swizzle = BackendViewSwizzle::R001;
        call.buffers[BACKEND_UAV_BUFFER_BASE] = result.get();
        // Twice: the second dispatch is served by the memo.
        ASSERT_TRUE(device.dispatch(call));
        ASSERT_TRUE(device.dispatch(call));
        ASSERT_TRUE(device.wait_idle());
        std::array<float, 4> seen{};
        ASSERT_TRUE(result->read(0, seen.data(), sizeof(seen)));
        EXPECT_NEAR(seen[0], red / 255.0f, 1e-6f) << "a previous round's view";
        EXPECT_EQ(seen[1], 0.0f);
        EXPECT_EQ(seen[2], 0.0f);
        EXPECT_EQ(seen[3], 1.0f);
    }
}

}  // namespace
