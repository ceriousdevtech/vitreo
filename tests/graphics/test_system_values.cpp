// SPDX-License-Identifier: LGPL-2.1-only
//
// D3D11 system values beyond position/target/depth, from real fxc output
// (tools/oracle_probe/system_value_shaders.hlsl), on real Metal:
// SV_ClipDistance, SV_RenderTargetArrayIndex and SV_ViewportArrayIndex out of
// a vertex shader, and
// the pixel-shader inputs and outputs (SV_Coverage in/out, SV_PrimitiveID,
// SV_SampleIndex, SV_RenderTargetArrayIndex, SV_ViewportArrayIndex,
// SV_DepthGreaterEqual/LessEqual, SV_StencilRef).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "system_value_shaders.h"

#include <array>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::string translate(const unsigned char* bytecode, usize size,
                      ShaderStage stage) {
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl =
        compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? std::string(msl.msl_text()) : std::string{};
}

TEST(SystemValues, EveryServedSystemValueReachesItsMetalAttribute) {
    const std::string vs =
        translate(sv_vs_clip, sizeof(sv_vs_clip), ShaderStage::Vertex);
    EXPECT_NE(vs.find("[[clip_distance]]"), std::string::npos) << vs;
    EXPECT_NE(vs.find("[[render_target_array_index]]"), std::string::npos)
        << vs;
    EXPECT_NE(vs.find("[[user(locn1)]]"), std::string::npos)
        << "the clip register stays a varying for the pixel shader\n" << vs;

    const std::string ge =
        translate(sv_ps_ge, sizeof(sv_ps_ge), ShaderStage::Fragment);
    for (const char* attribute :
         {"[[sample_mask]]", "[[depth(greater)]]", "[[stencil]]",
          "[[primitive_id]]", "[[sample_id]]",
          "[[render_target_array_index]]", "[[viewport_array_index]]"}) {
        EXPECT_NE(ge.find(attribute), std::string::npos) << attribute << "\n"
                                                         << ge;
    }
    const std::string le =
        translate(sv_ps_le, sizeof(sv_ps_le), ShaderStage::Fragment);
    EXPECT_NE(le.find("[[depth(less)]]"), std::string::npos) << le;

    // And Metal takes them: the clip/layer VS feeding each pixel shader.
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    for (const std::string* fragment : {&ge, &le}) {
        BackendPipelineDesc pd{};
        pd.vertex_shader_msl = vs;
        pd.fragment_shader_msl = *fragment;
        pd.vertex_entry_point = pd.fragment_entry_point = "main0";
        pd.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0}};
        pd.vertex_stride = pd.vertex_buffer_strides[0] = 16;
        pd.color_format = PixelFormat::BGRA8Unorm;
        pd.depth_attachment = true;
        pd.sample_count = 4;
        pd.input_primitive_class = BackendPrimitiveClass::Triangle;
        pd.cache_key = fragment == &ge ? 0x5E0001u : 0x5E0002u;
        auto pipeline = device.create_pipeline(pd);
        EXPECT_TRUE(pipeline && pipeline->valid()) << *fragment;
    }
}

/// Draw two triangles through the fxc clip/layer VS into a layered target:
/// triangle A (vertex ids 0-2, leading layer 0) covers the left half, B
/// (ids 3-5, leading layer 1) the right. SV_ClipDistance0.x is the vertex's
/// x, so A -- entirely left of x = 0 -- is clipped away.
struct LayeredDraw {
    std::unique_ptr<BackendTexture2D> target;
    bool ok{false};
};

LayeredDraw draw_layered(u32 slices, u32 first_slice, u32 view_slices) {
    auto& device = MetalDevice::instance();
    LayeredDraw result;
    const std::string vs =
        translate(sv_vs_clip, sizeof(sv_vs_clip), ShaderStage::Vertex);
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = vs;
    pd.fragment_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 main0() { return float4(0.0, 1.0, 0.0, 1.0); }\n";
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0}};
    pd.vertex_stride = pd.vertex_buffer_strides[0] = 16;
    pd.color_format = PixelFormat::RGBA8Unorm;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0x5E0010u;
    auto pipeline = device.create_pipeline(pd);
    if (!pipeline || !pipeline->valid()) return result;

    // A: one triangle covering the whole left half (its right edge is the
    // vertical x = -0.05, its hypotenuse passes above the top-left corner);
    // B its mirror on the right. z = 0.5, w = 1.
    const std::array<float, 24> positions{
        -0.05f, -1.0f, 0.5f, 1.0f, -2.0f, -1.0f, 0.5f, 1.0f,
        -0.05f, 3.0f, 0.5f, 1.0f,  // A: ids 0, 1, 2 -> layers 0, 1, 0
        0.05f, -1.0f, 0.5f, 1.0f,  2.0f, -1.0f, 0.5f, 1.0f,
        0.05f, 3.0f, 0.5f, 1.0f,   // B: ids 3, 4, 5 -> layers 1, 0, 1
    };
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto vertices = device.create_buffer(vb, positions.data());
    BackendTexture2DDesc td{};
    td.width = td.height = 8;
    td.mip_levels = 1;
    td.array_size = slices;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    result.target = device.create_texture_2d(td, nullptr, 0);
    if (!vertices || !result.target) return result;
    for (u32 s = 0; s < slices; ++s) {
        (void)result.target->clear_render_target_level(
            {0.0f, 0.0f, 0.0f, 0.0f}, 0, s, 1);
    }
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = result.target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    pass_desc.color_slices[0] = first_slice;
    pass_desc.color_array_sizes[0] = view_slices;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    if (!pass) return result;
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 6;
    call.vertex_buffer = call.vertex_buffers[0] = vertices.get();
    call.viewport = {0, 0, 8, 8, 0, 1};
    result.ok = pass->draw(call) && pass->end();
    return result;
}

/// Green texels in the left and right halves of slice @p slice.
std::pair<u32, u32> green_halves(BackendTexture2D& target, u32 slice) {
    std::vector<u32> texels(64, 0);
    if (!target.readback_slice(0, slice, texels.data(), 32)) return {~0u, ~0u};
    u32 left = 0, right = 0;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            if (texels[y * 8 + x] == 0xFF00FF00u) (x < 4 ? left : right)++;
        }
    }
    return {left, right};
}

TEST(SystemValues, ClipDistanceClipsAndTheLeadingVertexPicksTheLayer) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    auto drawn = draw_layered(2, 0, 2);
    ASSERT_TRUE(drawn.ok);
    // A is clipped by SV_ClipDistance0.x < 0; B lands on its leading
    // vertex's layer, 1, and nowhere else.
    EXPECT_EQ(green_halves(*drawn.target, 0), (std::pair<u32, u32>{0, 0}));
    EXPECT_EQ(green_halves(*drawn.target, 1), (std::pair<u32, u32>{0, 32}));
}

TEST(SystemValues, TheLayerIsRelativeToTheViewsFirstSlice) {
    // D3D: SV_RenderTargetArrayIndex indexes the RTV, whose FirstArraySlice
    // is 1 here, so layer 1 is texture slice 2.
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    auto drawn = draw_layered(3, 1, 2);
    ASSERT_TRUE(drawn.ok);
    EXPECT_EQ(green_halves(*drawn.target, 0), (std::pair<u32, u32>{0, 0}));
    EXPECT_EQ(green_halves(*drawn.target, 1), (std::pair<u32, u32>{0, 0}));
    EXPECT_EQ(green_halves(*drawn.target, 2), (std::pair<u32, u32>{0, 32}));
}

TEST(SystemValues, TheViewportIndexPicksViewportAndScissorAndPastTheCountIsDisabled) {
    // The vs_viewport_index oracle scene at 16x16: triangles k = 0, 1, 2 go to
    // viewport k, coloured red, green, blue by SV_PrimitiveID. Two viewports
    // are set (left half; top of the right half), scissor 1 keeps the left
    // half of viewport 1, and Windows DROPS triangle 2 -- viewports past the
    // count are disabled (measured on Windows, WARP and hardware).
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl =
        translate(sv_vs_viewport, sizeof(sv_vs_viewport), ShaderStage::Vertex);
    pd.fragment_shader_msl = translate(sv_ps_primitive, sizeof(sv_ps_primitive),
                                       ShaderStage::Fragment);
    ASSERT_NE(pd.vertex_shader_msl.find("[[viewport_array_index]]"),
              std::string::npos) << pd.vertex_shader_msl;
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.vertex_attributes = {{0, 0, BackendVertexFormat::Float4, 0}};
    pd.vertex_stride = pd.vertex_buffer_strides[0] = 16;
    pd.color_format = PixelFormat::RGBA8Unorm;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0x5E0020u;
    auto pipeline = device.create_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid());

    std::array<float, 36> positions{};
    for (u32 t = 0; t < 3; ++t) {
        const std::array<float, 12> big{-1.0f, -1.0f, 0.5f, 1.0f, 3.0f, -1.0f,
                                        0.5f,  1.0f,  -1.0f, 3.0f, 0.5f, 1.0f};
        std::copy(big.begin(), big.end(), positions.begin() + t * 12);
    }
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto vertices = device.create_buffer(vb, positions.data());
    BackendTexture2DDesc td{};
    td.width = td.height = 16;
    td.mip_levels = 1;
    td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(vertices && target);
    ASSERT_TRUE(target->clear_render_target_level({0.0f, 0.0f, 0.0f, 0.0f}, 0,
                                                  0, 1));
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_TRUE(pass);
    const std::array<std::array<float, 6>, 2> viewports{{
        {0.0f, 0.0f, 8.0f, 16.0f, 0.0f, 1.0f},
        {8.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f},
    }};
    const std::array<std::array<int, 4>, 2> scissors{{
        {0, 0, 8, 16},
        {8, 0, 12, 8},
    }};
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 9;
    call.vertex_buffer = call.vertex_buffers[0] = vertices.get();
    call.viewport = viewports[0];
    call.viewports = viewports.data();
    call.scissors = scissors.data();
    call.viewport_count = 2;
    call.scissor_enabled = true;
    ASSERT_TRUE(pass->draw(call) && pass->end());

    std::vector<u32> texels(256, 0);
    ASSERT_TRUE(target->readback_slice(0, 0, texels.data(), 64));
    u32 red_left = 0, green_window = 0, other = 0;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const u32 texel = texels[y * 16 + x];
            if (x < 8 && texel == 0xFF0000FFu) {
                ++red_left;
            } else if (x >= 8 && x < 12 && y < 8 && texel == 0xFF00FF00u) {
                ++green_window;
            } else if (texel != 0u) {
                ++other;
            }
        }
    }
    EXPECT_EQ(red_left, 128u);   // triangle 2 did not overwrite viewport 0
    EXPECT_EQ(green_window, 32u);
    EXPECT_EQ(other, 0u);
}

}  // namespace
