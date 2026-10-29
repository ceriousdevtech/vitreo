// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_volume_layered_rt.cpp
/// @brief A draw into a Texture3D render-target view reaches every W slice
///        its SV_RenderTargetArrayIndex selects.
///
/// Learn D3D11_TEX3D_RTV: the view covers W slices [FirstWSlice,
/// FirstWSlice + WSize), and a vertex/geometry stage routes each primitive to
/// one of them with SV_RenderTargetArrayIndex. UE draws its colour-grading
/// LUT (32^3) as one instanced quad per slice this way. Vitreo attached only
/// the first slice, so all 32 slices were drawn into slice 0 and the rest of
/// the LUT stayed empty: every tonemapped pixel came out black.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

TEST(VolumeLayeredRenderTarget, ArrayIndexSelectsTheWSliceWithinTheView) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    constexpr unsigned kSide = 4, kDepth = 8, kFirst = 2, kSize = 4;

    BackendTexture3DDesc vd{};
    vd.width = vd.height = kSide;
    vd.depth = kDepth;
    vd.pixel_format = PixelFormat::RGBA8Unorm;
    vd.shader_resource = true;
    vd.render_target = true;
    auto volume = device.create_texture_3d(vd, nullptr, 0, 0);
    ASSERT_TRUE(volume);

    // Instance i writes array index i and colour red = i + 1.
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_out {
            float4 position [[position]];
            uint layer [[render_target_array_index]];
            float4 colour [[user(locn0)]];
        };
        vertex main0_out main0(uint vid [[vertex_id]],
                               uint iid [[instance_id]]) {
            const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
            main0_out out = {};
            out.position = float4(p[vid], 0.5, 1.0);
            out.layer = iid;
            out.colour = float4(float(iid + 1) / 255.0, 0, 0, 1);
            return out;
        }
    )";
    pd.fragment_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float4 colour [[user(locn0)]]; };
        fragment float4 main0(main0_in in [[stage_in]]) { return in.colour; }
    )";
    pd.vertex_entry_point = "main0";
    pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::RGBA8Unorm;
    pd.color_formats[0] = PixelFormat::RGBA8Unorm;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0x3D1A7E5Eu;
    auto pso = device.create_pipeline(pd);
    ASSERT_TRUE(pso && pso->valid());

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets_3d[0] = volume.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    pass_desc.color_depth_planes[0] = kFirst;
    pass_desc.color_array_sizes[0] = kSize;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    BackendDrawCall call{};
    call.pipeline = pso.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.instance_count = kSize;
    call.viewport = {0, 0, static_cast<float>(kSide),
                     static_cast<float>(kSide), 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    for (unsigned w = 0; w < kDepth; ++w) {
        std::vector<u8> slice(kSide * kSide * 4, 0xEE);
        ASSERT_TRUE(volume->readback_w_slice(0, w, slice.data(), kSide * 4));
        const bool in_view = w >= kFirst && w < kFirst + kSize;
        const u8 expected = in_view ? static_cast<u8>(w - kFirst + 1) : 0u;
        EXPECT_EQ(slice[0], expected) << "W slice " << w;
        EXPECT_EQ(slice[(kSide * kSide - 1) * 4], expected) << "W slice " << w;
    }
}

// UE's colour-grading LUT bake, at the backend: a 32^3 R10G10B10A2_UNORM
// Texture3D whose RTV covers every W slice (Learn D3D11_TEX3D_RTV: FirstWSlice
// 0, WSize 32), one full-target quad (a 4-vertex strip) per instance, each
// routed by SV_RenderTargetArrayIndex = instance to its own slice (D3D11.3
// 15.15: the index selects the RTV slice; 13.4: SV_InstanceID counts from 0
// per draw plus StartInstanceLocation), the pixel shader writing
// (u, v, layer / 31, 0). D3D11.3's float -> UNORM conversion rounds to
// nearest, so slice k holds B = 33k (1023 / 31 = 33) in every texel, and no
// slice is zero. Two forms: the D3D11 frontend's geometry-shader emulation
// issues one draw -- one render pass -- per instance (consecutive passes on
// the same attachments, which the queue folds and merges into one encoder),
// and a single DrawInstanced(4, 32).
TEST(VolumeLayeredRenderTarget, ColourGradingLutBakeFillsEverySliceOf32CubedRgb10A2) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    constexpr unsigned kSide = 32;

    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_out {
            float4 position [[position]];
            uint layer [[render_target_array_index]];
            float layer_value [[user(locn0), flat]];
        };
        vertex main0_out main0(uint vid [[vertex_id]],
                               uint iid [[instance_id]]) {
            // TRIANGLESTRIP: (-1,-1) (1,-1) (-1,1) (1,1).
            const float2 p[4] = {float2(-1, -1), float2(1, -1),
                                 float2(-1, 1), float2(1, 1)};
            main0_out out = {};
            out.position = float4(p[vid], 0.5, 1.0);
            out.layer = iid;
            out.layer_value = float(iid);
            return out;
        }
    )";
    pd.fragment_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float layer_value [[user(locn0), flat]]; };
        fragment float4 main0(main0_in in [[stage_in]],
                              float4 frag [[position]]) {
            return float4(frag.x / 32.0, frag.y / 32.0,
                          in.layer_value / 31.0, 0.0);
        }
    )";
    pd.vertex_entry_point = "main0";
    pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::RGB10A2Unorm;
    pd.color_formats[0] = PixelFormat::RGB10A2Unorm;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0x3D1A7E5Fu;
    auto pso = device.create_pipeline(pd);
    ASSERT_TRUE(pso && pso->valid());

    for (const bool per_instance_draws : {true, false}) {
        SCOPED_TRACE(per_instance_draws ? "one pass per instance"
                                        : "DrawInstanced(4, 32)");
        BackendTexture3DDesc vd{};
        vd.width = vd.height = vd.depth = kSide;
        vd.pixel_format = PixelFormat::RGB10A2Unorm;
        vd.shader_resource = true;
        vd.render_target = true;
        auto volume = device.create_texture_3d(vd, nullptr, 0, 0);
        ASSERT_TRUE(volume);

        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets_3d[0] = volume.get();
        pass_desc.color_formats[0] = PixelFormat::RGB10A2Unorm;
        pass_desc.color_depth_planes[0] = 0;
        pass_desc.color_array_sizes[0] = kSide;
        pass_desc.preserve_color_contents = true;
        BackendDrawCall call{};
        call.pipeline = pso.get();
        call.topology = BackendPrimitiveTopology::TriangleStrip;
        call.vertex_count = 4;
        call.viewport = {0, 0, static_cast<float>(kSide),
                         static_cast<float>(kSide), 0, 1};
        const unsigned draws = per_instance_draws ? kSide : 1u;
        for (unsigned d = 0; d < draws; ++d) {
            auto pass = device.create_render_pass_standalone(pass_desc);
            ASSERT_TRUE(pass && pass->valid());
            call.first_instance = per_instance_draws ? d : 0u;
            call.instance_count = per_instance_draws ? 1u : kSide;
            ASSERT_TRUE(pass->draw(call));
            ASSERT_TRUE(pass->end());
        }

        for (unsigned w = 0; w < kSide; ++w) {
            std::vector<u32> slice(kSide * kSide, 0xEEEEEEEEu);
            ASSERT_TRUE(volume->readback_w_slice(0, w, slice.data(), kSide * 4));
            usize wrong_b = 0;
            usize zero = 0;
            for (const u32 texel : slice) {
                wrong_b += ((texel >> 20) & 0x3FFu) != 33u * w ? 1u : 0u;
                zero += texel == 0 ? 1u : 0u;
            }
            EXPECT_EQ(wrong_b, 0u) << "W slice " << w << ": B must be "
                                   << 33u * w << ", texel 0 = 0x" << std::hex
                                   << slice[0];
            EXPECT_EQ(zero, 0u) << "W slice " << w;
            // Texel (x, y) is centred at (x + .5, y + .5): R = (x + .5) / 32
            // and G = (y + .5) / 32, to UNORM10.
            const auto unorm10 = [](double v) {
                return static_cast<u32>(v * 1023.0 + 0.5);
            };
            const u32 last = slice[kSide * kSide - 1];
            EXPECT_EQ(slice[0] & 0x3FFu, unorm10(0.5 / 32)) << "W slice " << w;
            EXPECT_EQ((last >> 10) & 0x3FFu, unorm10(31.5 / 32))
                << "W slice " << w;
            EXPECT_EQ(last >> 30, 0u) << "alpha 0, W slice " << w;
        }
    }
}
