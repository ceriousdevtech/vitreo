// SPDX-License-Identifier: LGPL-2.1-only
//
// An arbitrary D3D sampler border colour on real Metal, which offers three.
//
// The translator samples through s# (transparent-black border) and its border
// twin (opaque white, sampler 8 + n) and blends `lo + (hi - lo) * border`;
// `hi - lo` is exactly the weight the filter gave the border. The shader is
// real fxc output (tools/oracle_probe/border_colour_shaders.hlsl) and every
// expected value is that formula's: fully outside, straddling the edge, fully
// inside, and a gather at the edge.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "border_colour_shaders.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr std::array<float, 4> kBorder{0.25f, 0.5f, 0.75f, 1.0f};

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
    pd.cache_key = 0xB0DE7E4Du;
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

/// Run the fxc shader, translated with or without the twin emulation.
std::vector<float> run(bool twins) {
    auto& device = MetalDevice::instance();
    const auto container =
        parse_dxbc(border_colour_main, sizeof(border_colour_main));
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.border_twin_samplers = twins ? 1u : 0u;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
    EXPECT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = twins ? 0xB0DE0001u : 0xB0DE0002u;
    auto pipeline = device.create_compute_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();
    if (!pipeline) return {};

    // 4x1 RGBA16Float: texel i = (i + 1, i + 10, i + 20, 1).
    std::vector<u16> texels;
    for (u32 i = 0; i < 4; ++i) {
        for (const float v : {i + 1.0f, i + 10.0f, i + 20.0f, 1.0f}) {
            const _Float16 h = static_cast<_Float16>(v);
            u16 bits = 0;
            std::memcpy(&bits, &h, sizeof(bits));
            texels.push_back(bits);
        }
    }
    BackendTexture2DDesc td{};
    td.width = 4;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA16Float;
    auto texture = device.create_texture_2d(td, texels.data(), 4 * 8);
    BackendBufferDesc bd{};
    bd.byte_width = 4 * 16;
    bd.unordered_access = bd.shader_resource = true;
    const std::vector<float> poison(16, -1.0f);
    auto results = device.create_buffer(bd, poison.data());
    EXPECT_TRUE(texture && results);
    if (!texture || !results) return {};

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.textures[BACKEND_SRV_TEXTURE_BASE] = texture.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].dimension =
        BackendTextureViewDimension::Texture2D;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].array_size = 1;
    BackendSamplerDesc sampler{};
    sampler.min_filter = sampler.mag_filter = BackendFilter::Linear;
    sampler.mipmaps_enabled = false;
    sampler.address_u = sampler.address_v = BackendAddressMode::Border;
    sampler.border_color = 0;  // transparent black: the twin's partner
    call.samplers[0] = sampler;
    call.sampler_bound[0] = true;
    std::array<float, 16 + 8 * 4> push{};
    std::copy(kBorder.begin(), kBorder.end(), push.begin() + 16);
    if (twins) {
        call.border_twin_mask = 1;
        call.constants = push.data();
        call.constants_size = sizeof(push);
    }
    call.buffers[BACKEND_UAV_BUFFER_BASE] = results.get();
    EXPECT_TRUE(device.dispatch(call));
    return read_floats(device, results.get(), 16);
}

TEST(BorderColourTwins, ArbitraryBorderIsExactForSampleAndGather) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    const auto f = run(true);
    ASSERT_EQ(f.size(), 16u);
    const auto expect_row = [&](u32 row, std::array<float, 4> want,
                                const char* what) {
        for (u32 c = 0; c < 4; ++c) {
            EXPECT_NEAR(f[row * 4 + c], want[c], 1e-3f)
                << what << " component " << c;
        }
    };
    expect_row(0, kBorder, "fully outside: the border colour");
    expect_row(1, {0.5f * 1.0f + 0.5f * 0.25f, 0.5f * 10.0f + 0.5f * 0.5f,
                   0.5f * 20.0f + 0.5f * 0.75f, 1.0f},
               "straddling the edge: half texel 0, half border");
    expect_row(2, {2.0f, 11.0f, 21.0f, 1.0f}, "inside: texel 1");
    // Gather order (u0,v1), (u1,v1), (u1,v0), (u0,v0) with u0 = -1, u1 = 0,
    // v0 = 0, v1 = 1: only (0, 0) is inside.
    expect_row(3, {0.25f, 0.25f, 1.0f, 0.25f}, "edge gather of red");
}

TEST(BorderColourTwins, WithoutTheTwinTheBorderIsTransparentBlack) {
    // Negative control: the same shader and sampler without the emulation
    // read Metal's transparent black, so the test above sees the twins' work.
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    const auto f = run(false);
    ASSERT_EQ(f.size(), 16u);
    for (u32 c = 0; c < 4; ++c) EXPECT_NEAR(f[c], 0.0f, 1e-3f);
}

}  // namespace
