// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_d3d7_present_metal.cpp
/// @brief A rendered Direct3D 7 frame must reach the guest-visible
///        DirectDraw surface memory, which needs a real GPU round trip.

#include <gtest/gtest.h>

#include "winapi/graphics/ddraw/d3d7_present.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/ff_vertex_layout.hpp"
#include "graphics/shaders/ff_uniforms.hpp"
#include "graphics/shaders/ff_emulation.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::d3d7;

// End-to-end: render on the GPU, read back, land in surface memory.
TEST(D3D7Present, BackendClearReachesSurfaceMemory) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }
    constexpr u32 kSize = 4;
    graphics::BackendTexture2DDesc desc{};
    desc.width = kSize;
    desc.height = kSize;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    desc.render_target = true;
    desc.shader_resource = true;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());

    // Green, so a channel-order mistake shows up rather than cancelling out.
    ASSERT_TRUE(texture->clear_render_target({0.0f, 1.0f, 0.0f, 1.0f}));

    std::vector<u8> readback(kSize * kSize * 4u, 0);
    ASSERT_TRUE(texture->readback(0, readback.data(), kSize * 4u));

    std::vector<u8> surface(kSize * kSize * 4u, 0);
    ASSERT_TRUE(copy_bgra8_to_surface(readback.data(), kSize * 4u, kSize, kSize,
                                      surface.data(), kSize * 4u, 32));
    EXPECT_EQ(surface[0], 0u);    // B
    EXPECT_EQ(surface[1], 255u);  // G
    EXPECT_EQ(surface[2], 0u);    // R
    EXPECT_EQ(surface[3], 255u);  // A
}

// Differential: drive the shared fixed-function machinery directly, with no
// ddraw/D3D7 code in the path. Same geometry, same shaders, same backend calls.
// If this renders, the defect is in the D3D7 front-end; if it does not, it is
// in the shared FF path or in how these backend calls are being made.
TEST(FFDirectDraw, FixedFunctionTriangleRendersWithoutD3D7) {
    auto& device = graphics::MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    constexpr u32 kSize = 8;
    constexpr u32 kFvf = graphics::fvf::XYZRHW | graphics::fvf::DIFFUSE;

    graphics::FFPipelineState ff;
    ff.fvf_flags = kFvf;
    ff.lighting_enabled = false;
    ff.vertex_color_enable = true;
    ff.active_texture_stages = 0;
    for (auto& stage : ff.texture_stages) {
        stage.color_op = graphics::TexStageOp::Disable;
        stage.alpha_op = graphics::TexStageOp::Disable;
    }
    std::printf("[differential] ff: hash=%llu lighting=%d vcolor=%d stages=%u "
                "alpha=%d fog=%u specular=%d fvf=0x%x\n",
                (unsigned long long)ff.hash(), ff.lighting_enabled ? 1 : 0,
                ff.vertex_color_enable ? 1 : 0, ff.active_texture_stages,
                ff.alpha_test_enable ? 1 : 0, (unsigned)ff.fog_mode,
                ff.specular_enable ? 1 : 0, ff.fvf_flags);
    const auto shaders = graphics::generate_ff_shaders(ff);
    ASSERT_TRUE(shaders.valid());

    std::vector<graphics::BackendVertexAttribute> attributes;
    u32 stride = 0;
    const char* rejection = nullptr;
    ASSERT_TRUE(graphics::ff_build_vertex_layout(kFvf, attributes, stride,
                                                 rejection));
    ASSERT_EQ(stride, 20u);

    struct Vertex { float x, y, z, rhw; u32 color; };
    const Vertex tri[3] = {
        {0.0f, 0.0f, 0.5f, 1.0f, 0xFF00FF00u},
        {8.0f, 0.0f, 0.5f, 1.0f, 0xFF00FF00u},
        {0.0f, 8.0f, 0.5f, 1.0f, 0xFF00FF00u},
    };
    graphics::BackendBufferDesc vb_desc{};
    vb_desc.byte_width = sizeof(tri);
    vb_desc.vertex_buffer = true;
    auto vb = device.create_buffer(vb_desc, tri);
    ASSERT_NE(vb, nullptr);
    ASSERT_TRUE(vb->valid());

    graphics::BackendTexture2DDesc rt_desc{};
    rt_desc.width = kSize;
    rt_desc.height = kSize;
    rt_desc.mip_levels = 1;
    rt_desc.array_size = 1;
    rt_desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    rt_desc.render_target = true;
    rt_desc.shader_resource = true;
    auto color = device.create_texture_2d(rt_desc, nullptr, 0);
    ASSERT_NE(color, nullptr);
    ASSERT_TRUE(color->valid());
    ASSERT_TRUE(color->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    graphics::BackendPipelineDesc pipe{};
    pipe.vertex_shader_msl = shaders.vertex_msl;
    pipe.fragment_shader_msl = shaders.fragment_msl;
    pipe.vertex_entry_point = "ff_vertex";
    pipe.fragment_entry_point = "ff_fragment";
    pipe.vertex_attributes = attributes;
    pipe.vertex_buffer_strides[0] = stride;
    pipe.vertex_stride = stride;
    pipe.color_format = graphics::PixelFormat::BGRA8Unorm;
    pipe.color_write_mask = 0xF;
    pipe.color_write_masks[0] = 0xF;
    pipe.cache_key = shaders.state_hash;
    auto pipeline = device.create_pipeline(pipe);
    ASSERT_NE(pipeline, nullptr) << "pipeline creation failed";
    ASSERT_TRUE(pipeline->valid());

    graphics::FFUniformInputs inputs;
    inputs.viewport_width = static_cast<float>(kSize);
    inputs.viewport_height = static_cast<float>(kSize);
    std::vector<u8> vs_uniforms, fs_uniforms;
    graphics::pack_ff_vertex_uniforms(ff, inputs, vs_uniforms);
    graphics::pack_ff_fragment_uniforms(ff, inputs, fs_uniforms);

    graphics::BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = color.get();
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    ASSERT_TRUE(pass->valid());

    graphics::BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = graphics::BackendPrimitiveTopology::TriangleList;
    call.vertex_buffer = vb.get();
    call.vertex_buffers[0] = vb.get();
    call.vertex_count = 3;
    call.viewport = {0.0f, 0.0f, static_cast<float>(kSize),
                     static_cast<float>(kSize), 0.0f, 1.0f};
    call.vertex_constants = vs_uniforms.data();
    call.vertex_constants_size = vs_uniforms.size();
    call.fragment_constants = fs_uniforms.data();
    call.fragment_constants_size = fs_uniforms.size();
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    std::vector<u8> pixels(kSize * kSize * 4u, 0);
    ASSERT_TRUE(color->readback(0, pixels.data(), kSize * 4u));
    u32 green = 0, black = 0, other = 0;
    for (u32 i = 0; i < kSize * kSize; ++i) {
        const u8 b = pixels[i * 4 + 0], g = pixels[i * 4 + 1],
                 r = pixels[i * 4 + 2];
        if (b == 0 && g == 255 && r == 0) ++green;
        else if (b == 0 && g == 0 && r == 0) ++black;
        else ++other;
    }
    EXPECT_GT(green, 0u) << "shared FF path renders nothing either";
    EXPECT_GT(black, 0u) << "whole target painted; geometry ignored";
    EXPECT_EQ(other, 0u) << "unexpected colours";
}
