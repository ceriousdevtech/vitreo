// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal.cpp
/// @brief The D3D11 front-end on the real Metal backend, through the D3D11
///        entry points a guest calls.
///
/// UE bakes its 32^3 colour-grading LUT once (CombineLUTs through
/// RasterizeToVolumeTexture): DrawInstanced(4, 32) of a full-slice quad, the
/// vertex shader passing SV_InstanceID on, a geometry shader routing each
/// instance's triangles to SV_RenderTargetArrayIndex of a Texture3D RTV that
/// covers every W slice, the pixel shader reading SV_RenderTargetArrayIndex
/// back. A LUT left black tints every tonemapped pixel black. The backend's
/// own layered rendering is tested in test_volume_layered_rt.cpp; this runs
/// the front-end's geometry-shader emulation on top of it.
///
/// Expected values follow from the D3D11.3 functional spec, not from a run:
/// 15.15 (SV_RenderTargetArrayIndex selects the RTV slice; a Texture3D RTV
/// covers W slices [FirstWSlice, FirstWSlice + WSize), WSize -1 = all), 13.4
/// (SV_InstanceID counts 0..n-1 per draw), and the float -> UNORM rule
/// (round to nearest): slice k holds B = round(k / 31 * 1023) = 33k, R and G
/// the texel's centre (u, v) = ((x + .5) / 32, (y + .5) / 32), alpha 3. The
/// slice-per-instance routing on Windows: tools/oracle_probe/
/// d3d11_volume_slice_gs_probe.expected. Shaders: volume_lut_shaders.h (fxc).
///
/// Split: the base fixture is d3d11_test_metal_fixture.hpp; the typed buffer
/// and virtual-texture suites live in test_d3d11_metal_typed_buffer_*.cpp and
/// test_d3d11_metal_vt_*.cpp.

#include <gtest/gtest.h>

#include "graphics/metal/metal_device.hpp"

#include "d3d11_test_metal_fixture.hpp"
#include "stencil_ref_shaders.h"
#include "volume_lut_shaders.h"

#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace vitreo::winapi::d3d11_test {
namespace {

// UE's bake: the layer is SV_InstanceID, carried by the VS to the GS.
TEST_F(D3D11MetalTest, ColourLutBakeThroughAGeometryShaderFillsEveryWSlice) {
    const GuestAddr vs = shader(12, lut_vs_instance, sizeof(lut_vs_instance));
    const GuestAddr gs = shader(13, lut_gs, sizeof(lut_gs));
    const GuestAddr ps = shader(15, lut_ps, sizeof(lut_ps));
    ASSERT_TRUE(vs != 0 && gs != 0 && ps != 0);
    GuestAddr volume = 0;
    GuestAddr rtv = 0;
    make_lut(volume, rtv);
    bind_bake(vs, gs, ps, rtv);
    context_call(17, {0});  // IASetInputLayout(NULL): no vertex input
    const u64 skips = d3d11_skip_events();
    const auto encoded = graphics::MetalDevice::instance().draw_encode_counts();
    context_call(21, {4, kSide, 0, 0});  // DrawInstanced(4, 32, 0, 0)
    context_call(33, {0, 0, 0});         // unbind the RTV
    expect_lut(volume);
    EXPECT_EQ(d3d11_skip_events(), skips)
        << "a skip / REJECT / failed line was counted for the bake";
    EXPECT_EQ(graphics::MetalDevice::instance().draw_encode_counts().dropped,
              encoded.dropped);
}

// The same bake with the layer from a per-instance vertex attribute
// (D3D11_INPUT_PER_INSTANCE_DATA, step rate 1), in reverse: instance i
// draws layer 31 - i. Slice k still holds B = 33k.
TEST_F(D3D11MetalTest, ColourLutBakeWithTheLayerFromAVertexAttribute) {
    const GuestAddr vs =
        shader(12, lut_vs_attribute, sizeof(lut_vs_attribute));
    const GuestAddr gs = shader(13, lut_gs, sizeof(lut_gs));
    const GuestAddr ps = shader(15, lut_ps, sizeof(lut_ps));
    ASSERT_TRUE(vs != 0 && gs != 0 && ps != 0);
    GuestAddr volume = 0;
    GuestAddr rtv = 0;
    make_lut(volume, rtv);
    bind_bake(vs, gs, ps, rtv);

    // { "LAYER", 0, R32_UINT (42), offset 0, PER_INSTANCE_DATA, step 1 }.
    context_call(17, {input_layout({{"LAYER", 0, 42, 0, 1, 1}},
                                   lut_vs_attribute,
                                   sizeof(lut_vs_attribute))});
    std::array<u32, kSide> layers{};
    for (u32 i = 0; i < kSide; ++i) layers[i] = kSide - 1 - i;
    vertex_buffer(layers.data(), sizeof(layers), 4);

    const u64 skips = d3d11_skip_events();
    context_call(21, {4, kSide, 0, 0});  // DrawInstanced(4, 32, 0, 0)
    context_call(33, {0, 0, 0});
    expect_lut(volume);
    EXPECT_EQ(d3d11_skip_events(), skips)
        << "a skip / REJECT / failed line was counted for the bake";
}

// UE's own form (RasterizeToVolumeTexture): the quad from a vertex buffer of
// FScreenVertex {float2 Position, float2 UV} through ATTRIBUTE0/ATTRIBUTE1,
// the layer SV_InstanceID, DrawInstanced(4, 32) of a TRIANGLESTRIP.
TEST_F(D3D11MetalTest, ColourLutBakeFromAScreenVertexBuffer) {
    const GuestAddr vs =
        shader(12, lut_vs_screen_vertex, sizeof(lut_vs_screen_vertex));
    const GuestAddr gs = shader(13, lut_gs, sizeof(lut_gs));
    const GuestAddr ps = shader(15, lut_ps, sizeof(lut_ps));
    ASSERT_TRUE(vs != 0 && gs != 0 && ps != 0);
    GuestAddr volume = 0;
    GuestAddr rtv = 0;
    make_lut(volume, rtv);
    bind_bake(vs, gs, ps, rtv);
    // R32G32_FLOAT (16), per-vertex.
    context_call(17, {input_layout({{"ATTRIBUTE", 0, 16, 0, 0, 0},
                                    {"ATTRIBUTE", 1, 16, 8, 0, 0}},
                                   lut_vs_screen_vertex,
                                   sizeof(lut_vs_screen_vertex))});
    // Position = UV, strip order (0,0) (1,0) (0,1) (1,1).
    const std::array<float, 16> quad{0, 0, 0, 0, 1, 0, 1, 0,
                                     0, 1, 0, 1, 1, 1, 1, 1};
    vertex_buffer(quad.data(), sizeof(quad), 16);

    const u64 skips = d3d11_skip_events();
    context_call(21, {4, kSide, 0, 0});  // DrawInstanced(4, 32, 0, 0)
    context_call(33, {0, 0, 0});
    expect_lut(volume);
    EXPECT_EQ(d3d11_skip_events(), skips)
        << "a skip / REJECT / failed line was counted for the bake";
}

// PSSpecifiedStencilRefSupported end to end (Learn D3D11_FEATURE_DATA_D3D11_OPTIONS2,
// D3D11.3 functional spec: a pixel shader's SV_StencilRef replaces the stencil
// reference of its own pixel, for the test and for REPLACE). A 4x4 D24S8
// target, stencil ALWAYS / REPLACE, API reference 0x77, one full-screen
// triangle. The control shader writes no stencil output, so every pixel takes
// the API reference; the SV_StencilRef shader writes x * 16 + y + 1 for pixel
// (x, y), which must reach the stencil plane. Expected values are WARP's, from
// tools/oracle_probe/d3d11_a09b_shader_probe.expected [SV_StencilRef]
// (listed in row-major order, as that probe prints them). The device reports
// the cap only because this path works.
TEST_F(D3D11MetalTest, StencilRefPixelShaderWritesTheStencilBuffer) {
    // D3D11_FEATURE_D3D11_OPTIONS2 (14), eight BOOLs.
    const GuestAddr options2 = put(std::array<u32, 8>{
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu});
    ASSERT_EQ(device_call(33, {14, options2, 32}), d3d11::S_OK);
    ASSERT_EQ(memory.read_u32(options2).value(), 1u)
        << "PSSpecifiedStencilRefSupported";
    // UnifiedMemoryArchitecture: 0 on the real Apple-silicon device too (the
    // discrete-class adapter profile, .docs/contracts/dxgi.md); the host fact
    // stays on the backend.
    EXPECT_EQ(memory.read_u32(options2 + 28).value(), 0u)
        << "UnifiedMemoryArchitecture, host unified="
        << graphics::MetalDevice::instance().has_unified_memory();

    const GuestAddr vs =
        shader(12, sref_vs_fullscreen, sizeof(sref_vs_fullscreen));
    const GuestAddr ps_control = shader(15, sref_ps_one, sizeof(sref_ps_one));
    const GuestAddr ps_stencil =
        shader(15, sref_ps_stencil_ref, sizeof(sref_ps_stencil_ref));
    ASSERT_TRUE(vs != 0 && ps_control != 0 && ps_stencil != 0)
        << "a device that reports the cap must create the shader";

    // D3D11_TEXTURE2D_DESC: 4x4, 1 mip, 1 slice, format, 1 sample, usage,
    // bind flags, CPU access, misc.
    constexpr u32 kR32G32B32A32Float = 2, kR24G8Typeless = 44,
                  kD24S8 = 45, kRenderTarget = 0x20, kDepthStencil = 0x40,
                  kStaging = 3, kCpuRead = 0x20000;
    const auto texture = [&](u32 format, u32 usage, u32 bind, u32 cpu) {
        const std::array<u32, 11> desc{4, 4, 1, 1, format, 1, 0,
                                       usage, bind, cpu, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(5, {put(desc), 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    const GuestAddr color = texture(kR32G32B32A32Float, 0, kRenderTarget, 0);
    const GuestAddr depth = texture(kR24G8Typeless, 0, kDepthStencil, 0);
    const GuestAddr staging =
        texture(kR24G8Typeless, kStaging, 0, kCpuRead);
    ASSERT_TRUE(color != 0 && depth != 0 && staging != 0);

    GuestAddr out = out_slot();
    ASSERT_EQ(device_call(9, {color, 0, out}), d3d11::S_OK);  // default RTV
    const GuestAddr rtv = memory.read_u64(out).value();
    // D3D11_DEPTH_STENCIL_VIEW_DESC { D24_UNORM_S8_UINT, TEXTURE2D (3),
    // Flags 0, MipSlice 0 }.
    out = out_slot();
    ASSERT_EQ(device_call(10, {depth, put(std::array<u32, 4>{kD24S8, 3, 0, 0}),
                               out}), d3d11::S_OK);
    const GuestAddr dsv = memory.read_u64(out).value();

    // D3D11_DEPTH_STENCIL_DESC: depth off; stencil on, masks 0xFF, front and
    // back { KEEP (1), KEEP (1), REPLACE (3), ALWAYS (8) }.
    const std::array<u32, 13> dss_desc{0, 0, 2, 1, 0x0000FFFFu,
                                       1, 1, 3, 8, 1, 1, 3, 8};
    out = out_slot();
    ASSERT_EQ(device_call(21, {put(dss_desc), out}), d3d11::S_OK);
    const GuestAddr dss = memory.read_u64(out).value();
    // D3D11_RASTERIZER_DESC: SOLID, CULL_NONE, depth clip on.
    const std::array<u32, 10> rs_desc{3, 1, 0, 0, 0, 0, 1, 0, 0, 0};
    out = out_slot();
    ASSERT_EQ(device_call(22, {put(rs_desc), out}), d3d11::S_OK);
    const GuestAddr rs = memory.read_u64(out).value();

    const auto stencil_after = [&](GuestAddr ps) {
        std::array<u32, 16> stencil{};
        const std::array<float, 4> black{0, 0, 0, 0};
        context_call(50, {rtv, put(black)});  // ClearRenderTargetView
        // ClearDepthStencilView(dsv, DEPTH | STENCIL, 1.0f, 0): the FLOAT,
        // the fourth argument, travels in XMM3 (x64 ABI); R9 is not read.
        cpu.sse.xmm[3].u64_[0] = 0;
        cpu.sse.xmm[3].u64_[1] = 0;
        const float one = 1.0f;
        std::memcpy(&cpu.sse.xmm[3].u32_[0], &one, sizeof(one));
        context_call(53, {dsv, 3, 0, 0});
        context_call(43, {rs});                         // RSSetState
        const std::array<float, 6> viewport{0, 0, 4, 4, 0, 1};
        context_call(44, {1, put(viewport)});           // RSSetViewports
        context_call(24, {4});         // IASetPrimitiveTopology TRIANGLELIST
        context_call(17, {0});         // IASetInputLayout(NULL)
        context_call(11, {vs, 0, 0});  // VSSetShader
        context_call(23, {0, 0, 0});   // GSSetShader(NULL)
        context_call(9, {ps, 0, 0});   // PSSetShader
        const std::array<u64, 1> rtvs{rtv};
        context_call(33, {1, put(rtvs), dsv});          // OMSetRenderTargets
        context_call(36, {dss, 0x77});  // OMSetDepthStencilState
        context_call(13, {3, 0});       // Draw(3, 0)
        context_call(33, {0, 0, 0});    // unbind
        context_call(47, {staging, depth});             // CopyResource
        const GuestAddr mapped = put(std::array<u64, 2>{0, 0});
        EXPECT_EQ(context_call(14, {staging, 0, 1 /* READ */, 0, mapped}),
                  d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        const u32 row_pitch = memory.read_u32(mapped + 8).value();
        EXPECT_NE(data, 0u);
        for (u32 i = 0; i < 16; ++i) {
            // D24_UNORM_S8_UINT: the stencil byte is the top byte.
            stencil[i] = memory.read_u32(data + (i / 4) * row_pitch +
                                         (i % 4) * 4).value() >> 24;
        }
        context_call(15, {staging, 0});  // Unmap
        return stencil;
    };

    // The two measured rows: "<label>: stencil (...):" then 16 hex bytes.
    const auto measured = [](const std::string& label) {
        std::array<u32, 16> row{};
        std::ifstream in(VITREO_ORACLE_PROBE_DIR
                         "/d3d11_a09b_shader_probe.expected");
        std::string text;
        bool found = false;
        while (std::getline(in, text)) {
            if (text.find(label + ": stencil") == std::string::npos) continue;
            const std::string tail = text.substr(text.rfind(':') + 1);
            std::istringstream values(tail);
            std::string value;
            usize i = 0;
            while (values >> value && i < row.size()) {
                row[i++] = static_cast<u32>(std::stoul(value, nullptr, 16));
            }
            found = i == row.size();
            break;
        }
        EXPECT_TRUE(found) << label
                           << " row of d3d11_a09b_shader_probe.expected";
        return row;
    };

    const u64 skips = d3d11_skip_events();
    EXPECT_EQ(stencil_after(ps_control), measured("control ps_one"))
        << "without SV_StencilRef every pixel takes the API reference (0x77)";
    EXPECT_EQ(stencil_after(ps_stencil), measured("ps_stencil_ref"))
        << "SV_StencilRef = x * 16 + y + 1 must reach the stencil plane";
    EXPECT_EQ(d3d11_skip_events(), skips)
        << "a skip / REJECT / failed line was counted for the scene";
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
