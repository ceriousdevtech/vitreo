// SPDX-License-Identifier: LGPL-2.1-only
//
// Texture1D and Texture1DArray read by real fxc output on real Metal.
//
// The D3D11 frontend backs a 1D resource with a height-1 2D texture, and the
// translator declares 1D registers as 2D images and widens each address
// (Learn: ld takes a 1D array's slice from .y and the mip from .w; resinfo
// reports 0 in .yz for a Texture1D and the array size in .y for an array).
// The sampler's v axis is BORDER, so a sample that missed the row's centre
// would blend in the transparent-black border and read low.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "texture1d_shaders.h"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kWidth = 4;

/// One RGBA16Float row: texel i = (base + i, base + i + 10, base + i + 20, 1).
std::vector<u16> half_row(float base) {
    std::vector<u16> row;
    for (u32 i = 0; i < kWidth; ++i) {
        for (const float v : {base + i, base + i + 10.0f, base + i + 20.0f,
                              1.0f}) {
            const _Float16 h = static_cast<_Float16>(v);
            u16 bits = 0;
            std::memcpy(&bits, &h, sizeof(bits));
            row.push_back(bits);
        }
    }
    return row;
}

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
    pd.cache_key = 0x1D0E7E4Du;
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

TEST(Texture1DGpu, LoadSampleGradAndDimensionsMatchTheD3DContract) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    const auto container = parse_dxbc(texture1d_main, sizeof(texture1d_main));
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
    pd.cache_key = 0x1D000001u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();

    BackendTexture2DDesc td{};
    td.width = kWidth;
    td.height = 1;
    td.mip_levels = 1;
    td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA16Float;
    const auto row0 = half_row(0.0f);
    auto strip = device.create_texture_2d(td, row0.data(), kWidth * 8);
    td.array_size = 2;
    auto strips = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(strip && strips);
    const auto slice0 = half_row(0.0f);
    const auto slice1 = half_row(100.0f);
    ASSERT_TRUE(strips->update_slice(0, 0, slice0.data(), kWidth * 8));
    ASSERT_TRUE(strips->update_slice(0, 1, slice1.data(), kWidth * 8));

    BackendBufferDesc bd{};
    bd.byte_width = 6 * 16;
    bd.unordered_access = true;
    bd.shader_resource = true;
    const std::vector<float> poison(6 * 4, -1.0f);
    auto results = device.create_buffer(bd, poison.data());
    ASSERT_TRUE(results);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.textures[BACKEND_SRV_TEXTURE_BASE + 0] = strip.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 0].dimension =
        BackendTextureViewDimension::Texture2D;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 0].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 0].array_size = 1;
    call.textures[BACKEND_SRV_TEXTURE_BASE + 1] = strips.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 1].dimension =
        BackendTextureViewDimension::Texture2DArray;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 1].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 1].array_size = 2;
    BackendSamplerDesc border{};
    border.min_filter = border.mag_filter = BackendFilter::Linear;
    border.mipmaps_enabled = false;
    border.address_u = BackendAddressMode::Clamp;
    border.address_v = BackendAddressMode::Border;
    border.address_w = BackendAddressMode::Clamp;
    border.border_color = 0;  // transparent black
    call.samplers[0] = border;
    call.sampler_bound[0] = true;
    call.buffers[BACKEND_UAV_BUFFER_BASE + 0] = results.get();
    ASSERT_TRUE(device.dispatch(call));

    const auto f = read_floats(device, results.get(), 6 * 4);
    ASSERT_EQ(f.size(), 24u);
    const auto row = [&](u32 r) {
        return std::array<float, 4>{f[r * 4], f[r * 4 + 1], f[r * 4 + 2],
                                    f[r * 4 + 3]};
    };
    using V = std::array<float, 4>;
    EXPECT_EQ(row(0), (V{2, 12, 22, 1})) << "ld: texel 2";
    EXPECT_EQ(row(1), (V{101, 111, 121, 1})) << "ld: texel 1 of slice 1";
    EXPECT_EQ(row(2), (V{2, 12, 22, 1})) << "sample_l at texel 2's centre";
    EXPECT_EQ(row(3), (V{101.5f, 111.5f, 121.5f, 1}))
        << "sample_l between texels 1 and 2 of slice 1, no border blended";
    EXPECT_EQ(row(4), (V{4, 1, 4, 2}))
        << "resinfo: width, mips; array width, array size";
    EXPECT_EQ(row(5), (V{1.5f, 11.5f, 21.5f, 1})) << "sample_d, zero gradient";
}

}  // namespace
