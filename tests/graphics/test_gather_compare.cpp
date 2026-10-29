// SPDX-License-Identifier: LGPL-2.1-only
//
// gather4_c, gather4_po and gather4_po_c on real Metal, from fxc output
// (tools/oracle_probe/gather_family_shaders.hlsl).
//
// A 64x64 depth texture holds (x + 64y) / 4096 at texel (x, y) -- exact in
// float. At uv (0.375, 0.375) the gather footprint starts at texel (23, 23),
// and D3D returns it in the order (u0,v1), (u1,v1), (u1,v0), (u0,v0). The
// programmable offset is (+20, -20), sent as 20 and 44: -20 only if the 6 low
// bits are sign-extended, and both past Metal's native [-8, 7] range, so a
// lowering that relied on the native operand alone would wrap them.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "gather_family_shaders.h"

#include <array>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::vector<float> read_floats(GraphicsBackendDevice& device,
                               BackendBuffer* source, u32 count) {
    const std::string msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(const device float* words [[buffer(" +
        std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n"
        "    texture2d<float, access::write> out [[texture(" +
        std::to_string(BACKEND_UAV_TEXTURE_BASE) + ")]],\n"
        "    uint3 id [[thread_position_in_grid]]) {\n"
        "    if (id.x < out.get_width()) out.write(float4(words[id.x]), "
        "uint2(id.x, 0));\n}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    pd.cache_key = 0x6A7E7E4Du;
    auto pipeline = device.create_compute_pipeline(pd);
    BackendTexture2DDesc td{};
    td.width = count;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::R32Float;
    td.unordered_access = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    if (!pipeline || !pipeline->valid() || !texture) return {};
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = count;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = source;
    call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
    if (!device.dispatch(call)) return {};
    std::vector<float> out(count, 0.0f);
    if (!texture->readback(0, out.data(), count * sizeof(float))) return {};
    return out;
}

TEST(GatherFamilyGpu, CompareAndProgrammableOffsetsMatchTheD3DFootprint) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const auto container =
        parse_dxbc(gather_family_main, sizeof(gather_family_main));
    ASSERT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
    ASSERT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = 0x6A7E0001u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();

    constexpr u32 kSize = 64;
    const auto value = [](u32 x, u32 y) {
        return static_cast<float>(x + kSize * y) / 4096.0f;
    };
    std::vector<float> texels(kSize * kSize);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) texels[y * kSize + x] = value(x, y);
    }
    BackendTexture2DDesc td{};
    td.width = td.height = kSize;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::D32Float;
    td.shader_resource = true;
    auto depth = device.create_texture_2d(td, texels.data(), kSize * 4);
    ASSERT_TRUE(depth && depth->valid());

    // cbuffer Offsets { int2 dynamic_offset; float reference; }
    const float reference = value(23, 24);
    struct {
        i32 offset[2];
        float reference;
        float pad;
    } constants{{20, 44}, reference, 0.0f};
    BackendBufferDesc cbd{};
    cbd.byte_width = sizeof(constants);
    cbd.constant_buffer = true;
    auto cb = device.create_buffer(cbd, &constants);
    BackendBufferDesc rbd{};
    rbd.byte_width = 4 * 16;
    rbd.unordered_access = rbd.shader_resource = true;
    const std::vector<float> poison(16, -1.0f);
    auto results = device.create_buffer(rbd, poison.data());
    ASSERT_TRUE(cb && results);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.textures[BACKEND_SRV_TEXTURE_BASE] = depth.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].dimension =
        BackendTextureViewDimension::Texture2D;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].array_size = 1;
    BackendSamplerDesc less{};
    less.min_filter = less.mag_filter = BackendFilter::Nearest;
    less.mipmaps_enabled = false;
    less.address_u = less.address_v = BackendAddressMode::Clamp;
    less.comparison_enabled = true;
    less.comparison = BackendCompare::Less;
    BackendSamplerDesc point = less;
    point.comparison_enabled = false;
    call.samplers[0] = less;
    call.samplers[1] = point;
    call.sampler_bound[0] = call.sampler_bound[1] = true;
    call.buffers[BACKEND_CONSTANT_BUFFER_BASE] = cb.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = results.get();
    ASSERT_TRUE(device.dispatch(call));

    const auto f = read_floats(device, results.get(), 16);
    ASSERT_EQ(f.size(), 16u);
    using V = std::array<float, 4>;
    const auto row = [&](u32 r) {
        return V{f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]};
    };
    // The four texels of the footprint whose top-left is (u0, v0), in D3D's
    // gather order, and the LESS comparison of each against the reference.
    const auto footprint = [&](u32 u0, u32 v0) {
        return V{value(u0, v0 + 1), value(u0 + 1, v0 + 1),
                 value(u0 + 1, v0), value(u0, v0)};
    };
    const auto compared = [&](V texels4) {
        for (float& t : texels4) t = reference < t ? 1.0f : 0.0f;
        return texels4;
    };
    EXPECT_EQ(row(0), compared(footprint(23, 23))) << "gather4_c";
    EXPECT_EQ(row(1), footprint(43, 3)) << "gather4_po, offset (+20, -20)";
    EXPECT_EQ(row(2), compared(footprint(43, 3))) << "gather4_po_c";
    EXPECT_EQ(row(3), compared(footprint(24, 23)))
        << "gather4_c with an immediate offset (1, 0)";
    // The reference is texel (23, 24) itself, so the first footprint mixes
    // passes and fails -- a comparison that ignored the texels could not
    // produce it.
    EXPECT_EQ(row(0), (V{0, 1, 0, 0}));
}

}  // namespace
