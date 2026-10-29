// SPDX-License-Identifier: LGPL-2.1-only
//
// Area 03 view contracts.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../graphics/rgb32_buffer_shaders.h"
#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"
namespace sm51_blobs {
#include "../../oracle_data/d3d12_sm51/d3d12_sm51_shaders.h"
}  // namespace sm51_blobs

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Area 03 (views) and area 04 (state objects) contracts. Every expected value
// is a line of tools/oracle_probe/d3d11_a03_views_probe.expected,
// d3d11_a03_rgb32_buffer_probe.expected, d3d11_a03_cross_device_probe.expected
// or d3d11_a04_state_objects_probe.expected (Windows, WARP, FL 11_0).

class D3D11ViewContractTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr VIEW_DESC = DESC + 0x100;
    static constexpr u32 NEG1 = 0xFFFFFFFFu;

    /// CreateTexture1D/2D/3D (@p slot 4/5/6) from its descriptor words.
    GuestAddr make_texture(usize slot, std::initializer_list<u32> desc) {
        write_words(DESC, desc);
        const u32 hr = device_call(slot, {DESC, 0, OUT});
        EXPECT_EQ(hr, d3d11::S_OK) << "texture";
        return memory.read_u64(OUT).value();
    }
    /// A 2D texture: Width 8, Height 8.
    GuestAddr tex2d(u32 mips, u32 array, u32 format, u32 samples, u32 bind,
                    u32 misc = 0) {
        return make_texture(5, {8, 8, mips, array, format, samples, 0, 0, bind,
                                0, misc});
    }
    GuestAddr buffer(u32 bytes, u32 bind, u32 misc = 0, u32 stride = 0) {
        write_words(DESC, {bytes, 0, bind, 0, misc, stride});
        EXPECT_EQ(device_call(3, {DESC, 0, OUT}), d3d11::S_OK) << "buffer";
        return memory.read_u64(OUT).value();
    }
    /// Create* view (@p slot 7/8/9/10) with @p desc, or a NULL desc when
    /// empty. Returns the HRESULT; the view is `made`.
    u32 view(usize slot, GuestAddr resource, std::vector<u32> desc,
             bool validate_only = false) {
        EXPECT_TRUE(memory.write_u64(OUT, POISON).is_ok());
        GuestAddr desc_address = 0;
        if (!desc.empty()) {
            for (u32 i = 0; i < desc.size(); ++i) {
                EXPECT_TRUE(memory.write_u32(VIEW_DESC + 4 * i, desc[i]).is_ok());
            }
            desc_address = VIEW_DESC;
        }
        const u32 hr = device_call(
            slot, {resource, desc_address, validate_only ? 0u : OUT});
        made = validate_only ? 0 : memory.read_u64(OUT).value();
        return hr;
    }
    /// GetDesc words of the last view made, through @p method.
    std::vector<u32> view_desc(void (D3D11Manager::*method)(Marshaler&, usize),
                               u32 words) {
        for (u32 i = 0; i < 8; ++i) {
            EXPECT_TRUE(memory.write_u32(OUT2 + 4 * i, 0xCDCDCDCDu).is_ok());
        }
        set_arg(0, made);
        set_arg(1, OUT2);
        (d3d11.*method)(marshaler, 8);
        std::vector<u32> out;
        for (u32 i = 0; i < words; ++i) {
            out.push_back(memory.read_u32(OUT2 + 4 * i).value());
        }
        return out;
    }
    std::vector<u32> srv_desc() {
        return view_desc(&D3D11Manager::impl_ShaderResourceViewMethod, 6);
    }
    std::vector<u32> uav_desc() {
        return view_desc(&D3D11Manager::impl_UnorderedAccessViewMethod, 5);
    }
    std::vector<u32> rtv_desc() {
        return view_desc(&D3D11Manager::impl_RenderTargetViewMethod, 5);
    }
    std::vector<u32> dsv_desc() {
        return view_desc(&D3D11Manager::impl_DepthStencilViewMethod, 6);
    }

    GuestAddr made{0};
};

// W5/W6/W7/W13/W14/W15: the view format is fully qualified and must support
// the view's use (check_format_support); every refusal is E_INVALIDARG.
TEST_F(D3D11ViewContractTest, ViewFormatsAreFullyQualifiedAndSupported) {
    const GuestAddr typeless = tex2d(1, 1, 27, 1, 0xA8);  // SR|RT|UA
    // "srv/rtv/uav TYPELESS res NULL desc | as TYPELESS | as UNKNOWN".
    EXPECT_EQ(view(7, typeless, {}), d3d11::E_INVALIDARG);
    EXPECT_EQ(made, 0u) << "failure writes NULL";
    EXPECT_EQ(view(7, typeless, {27, 4, 0, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, typeless, {0, 4, 0, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(9, typeless, {}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typeless, {}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, typeless, {29, 4, 0, 1}), d3d11::S_OK);
    // "uav TYPELESS res as UNORM_SRGB": no sRGB typed UAV.
    EXPECT_EQ(view(8, typeless, {29, 4, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typeless, {28, 4, 0}), d3d11::S_OK);
    // Typed-cast and cross-family SRVs: E_INVALIDARG, not E_NOTIMPL.
    const GuestAddr unorm = tex2d(1, 1, 28, 1, 0x28);
    EXPECT_EQ(view(7, unorm, {29, 4, 0, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, unorm, {87, 4, 0, 1}), d3d11::E_INVALIDARG);
    // D* formats are not shader-readable: "srv R24G8_TL res as
    // D24_UNORM_S8_UINT", "rtv R32_TL(RT) res as D32_FLOAT".
    const GuestAddr r24g8 = tex2d(1, 1, 44, 1, 0x48);  // SR|DS
    EXPECT_EQ(view(7, r24g8, {45, 4, 0, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, r24g8, {46, 4, 0, 1}), d3d11::S_OK);
    const GuestAddr r32 = tex2d(1, 1, 39, 1, 0x28);  // SR|RT
    EXPECT_EQ(view(9, r32, {40, 4, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(9, r32, {41, 4, 0}), d3d11::S_OK);
    // Typed buffers: "srv buf typed R32_TYPELESS / R8G8B8A8_UNORM_SRGB /
    // UNKNOWN", "uav buf typed UNKNOWN / R32_TYPELESS (no RAW) /
    // R8G8B8A8_UNORM_SRGB".
    const GuestAddr typed = buffer(256, 0x88);  // SR|UA
    EXPECT_EQ(view(7, typed, {39, 1, 0, 4}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, typed, {29, 1, 0, 4}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, typed, {0, 1, 0, 4}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typed, {0, 1, 0, 4, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typed, {39, 1, 0, 4, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typed, {29, 1, 0, 4, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typed, {28, 1, 0, 4, 0}), d3d11::S_OK);
}

// W3/W4: a NULL-desc UAV of an array or a cube covers every slice as
// TEXTURE2DARRAY, and GetDesc reports a TEXTURE2DARRAY window.
TEST_F(D3D11ViewContractTest, UavNullDescCoversEverySliceOfArraysAndCubes) {
    const GuestAddr array = tex2d(3, 4, 28, 1, 0xA8);
    ASSERT_EQ(view(8, array, {}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{28, 5, 0, 0, 4}));
    ASSERT_EQ(view(8, array, {28, 5, 1, 1, NEG1}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{28, 5, 1, 1, 3}));
    const GuestAddr cube = tex2d(3, 6, 28, 1, 0xA8, 0x4);
    ASSERT_EQ(view(8, cube, {}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{28, 5, 0, 0, 6}));
    ASSERT_EQ(view(8, cube, {28, 5, 0, 2, NEG1}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{28, 5, 0, 2, 4}));
    const GuestAddr single = tex2d(1, 1, 28, 1, 0xA8);
    ASSERT_EQ(view(8, single, {28, 5, 0, 0, NEG1}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{28, 5, 0, 0, 1}));
}

// W8: functional spec §5.3.9.5 -- R32 UAVs over 32-bit typeless textures.
TEST_F(D3D11ViewContractTest, UavCastsThirtyTwoBitTypelessTexturesToR32) {
    const GuestAddr rgba8 = tex2d(1, 1, 27, 1, 0xA8);
    for (const u32 format : {41u, 42u, 43u}) {
        ASSERT_EQ(view(8, rgba8, {format, 4, 0}), d3d11::S_OK) << format;
        EXPECT_EQ(uav_desc()[0], format);
    }
    EXPECT_EQ(view(8, rgba8, {36, 4, 0}), d3d11::E_INVALIDARG)
        << "R16G16_UINT is not the resource's family nor an R32 cast";
    for (const u32 resource_format : {33u, 23u, 90u}) {
        const GuestAddr typeless = tex2d(1, 1, resource_format, 1, 0x88);
        EXPECT_EQ(view(8, typeless, {42, 4, 0}), d3d11::S_OK)
            << resource_format;
    }
    // Not a 64-bit typeless texture, not a typed one, and only a UAV casts.
    EXPECT_EQ(view(8, tex2d(1, 1, 9, 1, 0x88), {42, 4, 0}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, tex2d(1, 1, 28, 1, 0x88), {42, 4, 0}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, tex2d(1, 1, 33, 1, 0x88), {42, 4, 0, 1}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(view(9, rgba8, {42, 4, 0}), d3d11::E_INVALIDARG);
}

// W9/W10: MS-array ArraySize -1 resolves; a cube array starts at any face
// and NumCubes -1 takes every whole cube left.
TEST_F(D3D11ViewContractTest, MultisampleArrayAndCubeArrayWindows) {
    const GuestAddr ms_array = tex2d(1, 3, 28, 4, 0x28);
    ASSERT_EQ(view(7, ms_array, {28, 7, 1, NEG1}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 7, 1, 2, 0, 0}));
    const GuestAddr ms = tex2d(1, 1, 28, 4, 0x28);
    ASSERT_EQ(view(7, ms, {28, 7, 0, NEG1}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 7, 0, 1, 0, 0}));

    const GuestAddr cube_array = tex2d(3, 12, 28, 1, 0x28, 0x4);
    const auto cube = [&](u32 first, u32 cubes) {
        return view(7, cube_array, {28, 10, 0, NEG1, first, cubes});
    };
    ASSERT_EQ(cube(1, 1), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 10, 0, 3, 1, 1}));
    ASSERT_EQ(cube(3, NEG1), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 10, 0, 3, 3, 1}));
    ASSERT_EQ(cube(0, NEG1), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 10, 0, 3, 0, 2}));
    ASSERT_EQ(cube(6, NEG1), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 10, 0, 3, 6, 1}));
    EXPECT_EQ(cube(7, NEG1), d3d11::E_INVALIDARG);
    EXPECT_EQ(cube(7, 2), d3d11::E_INVALIDARG);
    EXPECT_EQ(cube(12, 1), d3d11::E_INVALIDARG);
    EXPECT_EQ(cube(0, 3), d3d11::E_INVALIDARG);
    EXPECT_EQ(cube(0, 0), d3d11::E_INVALIDARG);
}

// W11/W12/W16: a structured buffer's NULL desc, its UNKNOWN-only format, and
// unknown BUFFEREX flags accepted and echoed.
TEST_F(D3D11ViewContractTest, StructuredNullDescAndBufferExFlags) {
    const GuestAddr structured = buffer(256, 0x88, 0x40, 8);
    ASSERT_EQ(view(7, structured, {}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{0, 1, 0, 32, 0, 0}));
    ASSERT_EQ(view(8, structured, {}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{0, 1, 0, 32, 0}));
    EXPECT_EQ(view(7, structured, {41, 1, 0, 4}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, structured, {41, 1, 0, 4, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, structured, {41, 1, 0, 4, 4}), d3d11::E_INVALIDARG)
        << "R32_FLOAT with COUNTER";
    ASSERT_EQ(view(8, structured, {0, 1, 0, 32, 4}), d3d11::S_OK);
    EXPECT_EQ(uav_desc(), (std::vector<u32>{0, 1, 0, 32, 4}));

    const GuestAddr typed = buffer(256, 0x88);
    EXPECT_EQ(view(7, typed, {}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, typed, {}), d3d11::E_INVALIDARG);
    ASSERT_EQ(view(7, typed, {41, 11, 0, 4, 2}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{41, 11, 0, 4, 2, 0}));
}

// W17: R32G32B32 typed buffer SRVs alias three R32 texels per element; no
// UAV takes the format.
TEST_F(D3D11ViewContractTest, Rgb32TypedBufferShaderResourceViews) {
    const GuestAddr typed = buffer(256, 0x88);
    ASSERT_EQ(view(7, typed, {6, 1, 0, 21}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{6, 1, 0, 21, 0, 0}));
    ASSERT_FALSE(backend.state.texture_buffer_creations.empty());
    auto made_buffer = backend.state.texture_buffer_creations.back();
    EXPECT_EQ(made_buffer.pixel_format, graphics::PixelFormat::R32Float);
    EXPECT_EQ(made_buffer.first_element, 0u);
    EXPECT_EQ(made_buffer.num_elements, 63u);
    EXPECT_EQ(view(7, typed, {6, 1, 0, 22}), d3d11::E_INVALIDARG);
    ASSERT_EQ(view(7, typed, {7, 1, 1, 4}), d3d11::S_OK);
    made_buffer = backend.state.texture_buffer_creations.back();
    EXPECT_EQ(made_buffer.pixel_format, graphics::PixelFormat::R32Uint);
    EXPECT_EQ(made_buffer.first_element, 3u);
    EXPECT_EQ(made_buffer.num_elements, 12u);
    ASSERT_EQ(view(7, typed, {8, 1, 2, 3}), d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().pixel_format,
              graphics::PixelFormat::R32Sint);
    for (const u32 format : {6u, 7u, 8u}) {
        EXPECT_EQ(view(8, typed, {format, 1, 0, 4, 0}), d3d11::E_INVALIDARG)
            << format;
    }
}

// Shader model 5.1 is D3D12's (Learn, "Shader Model 5.1"): CreateComputeShader
// refuses cs_5_1 with E_INVALIDARG and a NULL shader, and accepts the same
// HLSL built as cs_5_0 (tools/oracle_probe/d3d11_sm51_rejected_probe.expected,
// FL 11_0; identical at 11_1 and on x86).
TEST_F(D3D11ViewContractTest, ComputeShaderModel51IsRefused) {
    using sm51_blobs::sm51_cs_space0_50;
    using sm51_blobs::sm51_cs_space0_51;
    constexpr GuestAddr BYTECODE = 0x7000000;
    ASSERT_TRUE(memory.map_region(BYTECODE, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_LE(sizeof(sm51_cs_space0_51), 0x2000u);
    ASSERT_TRUE(memory.write_bytes(BYTECODE, sm51_cs_space0_51,
                                   sizeof(sm51_cs_space0_51)).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT, 0xA5A5A5A5A5A5A5A5ull).is_ok());
    EXPECT_EQ(device_call(18, {BYTECODE, sizeof(sm51_cs_space0_51), 0, OUT}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    ASSERT_TRUE(memory.write_bytes(BYTECODE, sm51_cs_space0_50,
                                   sizeof(sm51_cs_space0_50)).is_ok());
    EXPECT_EQ(device_call(18, {BYTECODE, sizeof(sm51_cs_space0_50), 0, OUT}),
              d3d11::S_OK);
    EXPECT_NE(memory.read_u64(OUT).value(), 0u);
}

// W17, the shader side: a Dispatch with an R32G32B32 view bound runs the
// translation that reads three texels per element.
TEST_F(D3D11ViewContractTest, Rgb32ViewsSelectTheThreeTexelTranslation) {
    constexpr GuestAddr BYTECODE = SO_BASE + 0x400;  // < 0x600 bytes
    ASSERT_LT(sizeof(rgb32_cs_load), 0x600u);
    ASSERT_TRUE(memory.write_bytes(BYTECODE, rgb32_cs_load,
                                   sizeof(rgb32_cs_load)).is_ok());
    ASSERT_EQ(device_call(18, {BYTECODE, sizeof(rgb32_cs_load), 0, OUT}),
              d3d11::S_OK);  // CreateComputeShader
    const GuestAddr shader = memory.read_u64(OUT).value();
    const GuestAddr source = buffer(96, 0x8);
    std::array<GuestAddr, 3> views{};
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_EQ(view(7, source, {6u + i, 1, 0, 3}), d3d11::S_OK);
        views[i] = made;
    }
    const GuestAddr output = buffer(384, 0x80, 0x20);
    ASSERT_EQ(view(8, output, {39, 1, 0, 96, 1}), d3d11::S_OK);
    const GuestAddr uav = made;
    const auto context_call = [&](usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, context);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        d3d11.impl_ContextMethod(marshaler, slot);
    };
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(memory.write_u64(OUT2 + 8 * i, views[i]).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(OUT2 + 0x20, uav).is_ok());
    context_call(69, {shader, 0, 0});           // CSSetShader
    context_call(67, {0, 3, OUT2});             // CSSetShaderResources
    context_call(68, {0, 1, OUT2 + 0x20, 0});   // CSSetUnorderedAccessViews
    context_call(41, {6, 1, 1});                // Dispatch
    const std::string msl = backend.state.last_compute_msl;
    ASSERT_FALSE(msl.empty());
    // The three-texel read: the element index scaled by 3, and GetDimensions
    // divided by it.
    EXPECT_NE(msl.find("* 3u"), std::string::npos) << msl;
    EXPECT_NE(msl.find("/ 3u"), std::string::npos) << msl;
    // Without an R32G32B32 view the plain translation serves the dispatch.
    ASSERT_EQ(view(7, source, {41, 1, 0, 24}), d3d11::S_OK);
    ASSERT_TRUE(memory.write_u64(OUT2, made).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT2 + 8, made).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT2 + 16, made).is_ok());
    context_call(67, {0, 3, OUT2});
    context_call(41, {6, 1, 1});
    EXPECT_NE(backend.state.last_compute_msl, msl);
}

// W17, the draw stages: a vertex and a pixel shader reading an R32G32B32
// view are compiled with the three-texel read, the pixel variant keyed apart
// from the plain one.
TEST_F(D3D11ViewContractTest, Rgb32ViewsReachTheVertexAndPixelTranslations) {
    constexpr GuestAddr VS_CODE = SO_BASE + 0x400;
    constexpr GuestAddr PS_CODE = SO_BASE + 0x700;
    ASSERT_LE(sizeof(rgb32_vs_load), 0x300u);
    ASSERT_LE(sizeof(rgb32_ps_load), 0x300u);
    ASSERT_TRUE(memory.write_bytes(VS_CODE, rgb32_vs_load,
                                   sizeof(rgb32_vs_load)).is_ok());
    ASSERT_TRUE(memory.write_bytes(PS_CODE, rgb32_ps_load,
                                   sizeof(rgb32_ps_load)).is_ok());
    ASSERT_EQ(device_call(12, {VS_CODE, sizeof(rgb32_vs_load), 0, OUT}),
              d3d11::S_OK);
    const GuestAddr vs = memory.read_u64(OUT).value();
    ASSERT_EQ(device_call(15, {PS_CODE, sizeof(rgb32_ps_load), 0, OUT}),
              d3d11::S_OK);
    const GuestAddr ps = memory.read_u64(OUT).value();
    const GuestAddr target = tex2d(1, 1, 28, 1, 0x20);
    ASSERT_EQ(view(9, target, {}), d3d11::S_OK);
    const GuestAddr rtv = made;
    const GuestAddr source = buffer(96, 0x8);
    ASSERT_EQ(view(7, source, {6, 1, 0, 3}), d3d11::S_OK);
    const GuestAddr rgb32 = made;
    ASSERT_EQ(view(7, source, {2, 1, 0, 3}), d3d11::S_OK);
    const GuestAddr rgba32 = made;
    const auto context_call = [&](usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, context);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        d3d11.impl_ContextMethod(marshaler, slot);
    };
    ASSERT_TRUE(memory.write_u64(OUT2, rgb32).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT2 + 8, rtv).is_ok());
    write_words(OUT2 + 0x10, {0, 0, 0x41000000u, 0x41000000u, 0,
                              0x3F800000u});  // 8x8 viewport
    context_call(11, {vs, 0, 0});           // VSSetShader
    context_call(9, {ps, 0, 0});            // PSSetShader
    context_call(25, {0, 1, OUT2});         // VSSetShaderResources
    context_call(8, {0, 1, OUT2});          // PSSetShaderResources
    context_call(33, {1, OUT2 + 8, 0});     // OMSetRenderTargets
    context_call(24, {4});                  // IASetPrimitiveTopology
    context_call(44, {1, OUT2 + 0x10});     // RSSetViewports
    context_call(13, {3, 0});               // Draw
    const std::string vertex = backend.state.last_pipeline_vertex_msl;
    const std::string fragment = backend.state.last_pipeline_fragment_msl;
    ASSERT_FALSE(vertex.empty());
    ASSERT_FALSE(fragment.empty());
    EXPECT_NE(vertex.find("* 3u"), std::string::npos) << vertex;
    EXPECT_NE(fragment.find("* 3u"), std::string::npos) << fragment;
    // An R32G32B32A32 view takes the plain translation again.
    ASSERT_TRUE(memory.write_u64(OUT2, rgba32).is_ok());
    context_call(25, {0, 1, OUT2});
    context_call(8, {0, 1, OUT2});
    context_call(13, {3, 0});
    EXPECT_EQ(backend.state.last_pipeline_vertex_msl.find("* 3u"),
              std::string::npos);
    EXPECT_EQ(backend.state.last_pipeline_fragment_msl.find("* 3u"),
              std::string::npos);
}

// What a draw derives from its bound SRVs (the R32G32B32 variant, the bounds
// guard's size table) is a function of the views at those addresses, and an
// address is reused once its view is released (the object free list). A
// view released and re-created at the same address with a different format
// and element count must change both, although the bound addresses did not.
TEST_F(D3D11ViewContractTest, SrvSummaryInvalidatesOnViewCreateAndRelease) {
    constexpr GuestAddr VS_CODE = SO_BASE + 0x400;
    constexpr GuestAddr PS_CODE = SO_BASE + 0x700;
    ASSERT_TRUE(memory.write_bytes(VS_CODE, rgb32_vs_load,
                                   sizeof(rgb32_vs_load)).is_ok());
    ASSERT_TRUE(memory.write_bytes(PS_CODE, rgb32_ps_load,
                                   sizeof(rgb32_ps_load)).is_ok());
    ASSERT_EQ(device_call(12, {VS_CODE, sizeof(rgb32_vs_load), 0, OUT}),
              d3d11::S_OK);
    const GuestAddr vs = memory.read_u64(OUT).value();
    ASSERT_EQ(device_call(15, {PS_CODE, sizeof(rgb32_ps_load), 0, OUT}),
              d3d11::S_OK);
    const GuestAddr ps = memory.read_u64(OUT).value();
    const GuestAddr target = tex2d(1, 1, 28, 1, 0x20);
    ASSERT_EQ(view(9, target, {}), d3d11::S_OK);
    const GuestAddr rtv = made;
    const GuestAddr typed = buffer(96, 0x8);
    const GuestAddr raw = buffer(64, 0x8, 0x20);  // ALLOW_RAW_VIEWS
    ASSERT_EQ(view(7, typed, {6, 1, 0, 3}), d3d11::S_OK);  // R32G32B32_FLOAT
    const GuestAddr t0 = made;
    ASSERT_EQ(view(7, raw, {39, 11, 0, 16, 1}), d3d11::S_OK);  // raw, 16
    const GuestAddr t1 = made;
    const auto context_call = [&](usize slot,
                                  std::initializer_list<u64> args) {
        set_arg(0, context);
        usize i = 1;
        for (const u64 a : args) set_arg(i++, a);
        d3d11.impl_ContextMethod(marshaler, slot);
    };
    const auto release_view = [&](GuestAddr view_address) {
        set_arg(0, view_address);
        d3d11.impl_ShaderResourceViewMethod(marshaler, 2);  // Release
        return static_cast<u32>(cpu.gpr.rax);
    };
    // PS t0/t1 and VS t0 from OUT2; the size table's t1 entry (the guard's
    // push member at kGuardSizeTableOffset, one u32 per t# slot).
    const auto bind_and_draw = [&](GuestAddr v0, GuestAddr v1) {
        EXPECT_TRUE(memory.write_u64(OUT2, v0).is_ok());
        EXPECT_TRUE(memory.write_u64(OUT2 + 8, v1).is_ok());
        context_call(25, {0, 1, OUT2});  // VSSetShaderResources
        context_call(8, {0, 2, OUT2});   // PSSetShaderResources
        if (v0 == 0) return;
        context_call(13, {3, 0});        // Draw
    };
    const auto t1_size_words = [&]() -> u32 {
        const auto& push = backend.state.last_draw_fragment_constants;
        const usize at = graphics::kGuardSizeTableOffset + 4;
        if (push.size() < at + 4) return ~0u;
        u32 words = 0;
        std::memcpy(&words, push.data() + at, 4);
        return words;
    };
    ASSERT_TRUE(memory.write_u64(OUT2 + 0x10, rtv).is_ok());
    write_words(OUT2 + 0x20, {0, 0, 0x41000000u, 0x41000000u, 0,
                              0x3F800000u});  // 8x8 viewport
    context_call(11, {vs, 0, 0});            // VSSetShader
    context_call(9, {ps, 0, 0});             // PSSetShader
    context_call(33, {1, OUT2 + 0x10, 0});   // OMSetRenderTargets
    context_call(24, {4});                   // IASetPrimitiveTopology
    context_call(44, {1, OUT2 + 0x20});      // RSSetViewports

    bind_and_draw(t0, t1);
    EXPECT_NE(backend.state.last_pipeline_fragment_msl.find("* 3u"),
              std::string::npos);
    EXPECT_EQ(t1_size_words(), 16u);

    // Unbind (the bindings hold references), release, and re-create each
    // view at its old address.
    bind_and_draw(0, 0);
    EXPECT_EQ(release_view(t0), 0u);
    ASSERT_EQ(view(7, typed, {2, 1, 0, 3}), d3d11::S_OK);  // R32G32B32A32
    ASSERT_EQ(made, t0) << "the free list did not reuse the address";
    EXPECT_EQ(release_view(t1), 0u);
    ASSERT_EQ(view(7, raw, {39, 11, 0, 8, 1}), d3d11::S_OK);  // raw, 8
    ASSERT_EQ(made, t1) << "the free list did not reuse the address";

    bind_and_draw(t0, t1);
    EXPECT_EQ(backend.state.last_pipeline_fragment_msl.find("* 3u"),
              std::string::npos);
    EXPECT_EQ(t1_size_words(), 8u);
}

// W8 over a Texture3D (D3D11.3 5.3.9.5): the R32 cast of a 32-bit typeless
// volume binds as a view of that format, one mip; a W-slice window binds its
// whole mip and hands the shader (WSize << 16) | FirstWSlice in the u# entry
// of the size table (Metal views a volume whole in W). Corrected: the cast
// used to be dropped. Winoracle: d3d11_view_line_probe [vol] "R32_UINT cast
// mip0 rmw", "mip1 mark", "mip0 W{1,1} mark".
TEST_F(D3D11ViewContractTest, VolumeUavFormatCastBindsThroughAView) {
    constexpr GuestAddr BYTECODE = SO_BASE + 0x400;
    ASSERT_LT(sizeof(cs_lifetime), 0x600u);
    ASSERT_TRUE(memory.write_bytes(BYTECODE, cs_lifetime,
                                   sizeof(cs_lifetime)).is_ok());
    ASSERT_EQ(device_call(18, {BYTECODE, sizeof(cs_lifetime), 0, OUT}),
              d3d11::S_OK);
    const GuestAddr shader = memory.read_u64(OUT).value();
    const GuestAddr volume =
        make_texture(6, {8, 8, 4, 2, 27, 0, 0x88, 0, 0});  // RGBA8_TL SR|UA
    const auto dispatch_with = [&](u32 format, u32 mip = 0, u32 first_w = 0,
                                   u32 w_size = NEG1) {
        EXPECT_EQ(view(8, volume, {format, 8, mip, first_w, w_size}),
                  d3d11::S_OK)
            << format;
        EXPECT_TRUE(memory.write_u64(OUT2, made).is_ok());
        set_arg(0, context);
        set_arg(1, shader);
        set_arg(2, 0);
        set_arg(3, 0);
        d3d11.impl_ContextMethod(marshaler, 69);  // CSSetShader
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 1);
        set_arg(3, OUT2);
        set_arg(4, 0);
        d3d11.impl_ContextMethod(marshaler, 68);  // CSSetUnorderedAccessViews
        set_arg(0, context);
        set_arg(1, 1);
        set_arg(2, 1);
        set_arg(3, 1);
        d3d11.impl_ContextMethod(marshaler, 41);  // Dispatch
        return backend.state.last_dispatch_has_uav_3d;
    };
    EXPECT_TRUE(dispatch_with(28)) << "the storage's own format binds";
    EXPECT_EQ(backend.state.last_dispatch_uav_3d_format,
              graphics::PixelFormat::RGBA8Unorm);
    EXPECT_EQ(backend.state.last_dispatch_uav_3d_mips, 1u) << "one mip";
    EXPECT_TRUE(dispatch_with(42)) << "an R32_UINT cast binds as R32Uint";
    EXPECT_EQ(backend.state.last_dispatch_uav_3d_format,
              graphics::PixelFormat::R32Uint);
    EXPECT_TRUE(dispatch_with(42, 1)) << "mip 1, whole";
    EXPECT_EQ(backend.state.last_dispatch_uav_3d_mips, 1u);
    constexpr usize kWindowWord =
        graphics::kGuardSizeTableOffset / 4 + graphics::kGuardSizeSrvSlots;
    ASSERT_GT(backend.state.last_dispatch_constants.size(), kWindowWord);
    EXPECT_EQ(backend.state.last_dispatch_constants[kWindowWord], 0u)
        << "a whole mip has no window";
    EXPECT_TRUE(dispatch_with(42, 0, 1, 1)) << "a W window binds its mip";
    EXPECT_EQ(backend.state.last_dispatch_uav_3d_mips, 1u);
    EXPECT_EQ(backend.state.last_dispatch_constants[kWindowWord],
              (1u << graphics::kVolumeUavWindowSizeShift) | 1u);
}

// W19: multisample dimensions over one-sample resources. RTV and DSV render
// as the single-sample view; an SRV binds as the 2D mip, read through a
// shader variant (corrected: the SRVs failed loudly with E_NOTIMPL before;
// Windows S_OK, d3d11_a03_views_probe "srv tex2d as 2DMS").
TEST_F(D3D11ViewContractTest, MultisampleDimensionsOverOneSampleResources) {
    const GuestAddr single = tex2d(1, 1, 28, 1, 0x28);
    ASSERT_EQ(view(9, single, {28, 6, 0}), d3d11::S_OK);
    EXPECT_EQ(rtv_desc(), (std::vector<u32>{28, 6, 0, 0, 0}));
    const GuestAddr array = tex2d(1, 4, 28, 1, 0x28);
    ASSERT_EQ(view(9, array, {28, 7, 1, 2}), d3d11::S_OK);
    EXPECT_EQ(rtv_desc(), (std::vector<u32>{28, 7, 1, 2, 0}));
    const GuestAddr depth = tex2d(1, 1, 40, 1, 0x40);
    ASSERT_EQ(view(10, depth, {40, 5, 0}), d3d11::S_OK);
    EXPECT_EQ(dsv_desc(), (std::vector<u32>{40, 5, 0, 0, 0, 0}));
    ASSERT_EQ(view(10, depth, {40, 6, 0, 0, 1}), d3d11::S_OK);
    EXPECT_EQ(dsv_desc(), (std::vector<u32>{40, 6, 0, 0, 1, 0}));
    ASSERT_EQ(view(7, single, {28, 6, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 6, 0, 0, 0, 0}));
    ASSERT_EQ(view(7, array, {28, 7, 1, NEG1}), d3d11::S_OK);
    EXPECT_EQ(srv_desc(), (std::vector<u32>{28, 7, 1, 3, 0, 0}));
    // A multisampled resource still takes only the MS dimensions.
    EXPECT_EQ(view(9, tex2d(1, 1, 28, 4, 0x28), {28, 4, 0}),
              d3d11::E_INVALIDARG);
}

// W20: READ_ONLY_STENCIL needs a format with stencil.
TEST_F(D3D11ViewContractTest, ReadOnlyStencilNeedsAStencilFormat) {
    EXPECT_EQ(view(10, tex2d(1, 1, 40, 1, 0x40), {40, 3, 2, 0}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(view(10, tex2d(1, 1, 55, 1, 0x40), {55, 3, 2, 0}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(view(10, tex2d(1, 1, 39, 1, 0x40), {40, 3, 2, 0}),
              d3d11::E_INVALIDARG);
    ASSERT_EQ(view(10, tex2d(1, 1, 45, 1, 0x40), {45, 3, 3, 0}), d3d11::S_OK);
    EXPECT_EQ(dsv_desc()[2], 3u);
}

// W21: a validate-only UAV (ppView NULL) runs the full validation.
TEST_F(D3D11ViewContractTest, ValidateOnlyUavRunsTheFullValidation) {
    const GuestAddr volume =
        make_texture(6, {8, 8, 8, 3, 28, 0, 0x88, 0, 0});  // SR|UA
    EXPECT_EQ(view(8, volume, {28, 8, 1, 0, 5}, true), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, volume, {28, 8, 1, 1, NEG1}, true), 1u);
    EXPECT_EQ(view(8, tex2d(1, 1, 27, 1, 0x88), {}, true),
              d3d11::E_INVALIDARG);
}

// W22: a buffer RTV is validated like Windows validates it and renders
// (corrected: the valid one failed loudly with E_NOTIMPL). Winoracle:
// d3d11_a03_views_probe "rtv buf SR|RT BUFFER R32_FLOAT {2,4}" -> desc
// {41, 1, 2, 4}, GetResource the buffer.
TEST_F(D3D11ViewContractTest, BufferRenderTargetViewsValidateAndDescribe) {
    const GuestAddr target = buffer(256, 0x28);  // SR|RT
    ASSERT_EQ(view(9, target, {41, 1, 2, 4}), d3d11::S_OK);
    EXPECT_EQ(rtv_desc(), (std::vector<u32>{41, 1, 2, 4, 0}));
    set_arg(0, made);
    set_arg(1, OUT2);
    d3d11.impl_RenderTargetViewMethod(marshaler, 7);  // GetResource
    EXPECT_EQ(memory.read_u64(OUT2).value(), target);
    EXPECT_EQ(view(9, target, {41, 1, 0, 65}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(9, target, {}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(9, target, {27, 1, 0, 4}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(9, buffer(256, 0x8), {41, 1, 0, 4}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(10, target, {}), d3d11::E_INVALIDARG) << "dsv buf";
}

// W23: creating a view of another device's resource is E_OUTOFMEMORY; a
// validate-only call judges the description (d3d11_a03_cross_device_probe).
TEST_F(D3D11ViewContractTest, ViewsOfAnotherDevicesResource) {
    const GuestAddr texture = tex2d(1, 1, 41, 1, 0xA8);
    const GuestAddr structured = buffer(256, 0x88, 0x40, 16);
    const GuestAddr depth = tex2d(1, 1, 40, 1, 0x40);
    const GuestAddr first = device;
    create_default_device();
    device = memory.read_u64(DEVICE_OUT).value();
    ASSERT_NE(device, first);
    EXPECT_EQ(view(7, texture, {}), 0x8007000Eu);
    EXPECT_EQ(view(8, texture, {}), 0x8007000Eu);
    EXPECT_EQ(view(9, texture, {}), 0x8007000Eu);
    EXPECT_EQ(view(10, depth, {}), 0x8007000Eu);
    EXPECT_EQ(view(7, structured, {}), 0x8007000Eu);
    EXPECT_EQ(view(8, structured, {}), 0x8007000Eu);
    EXPECT_EQ(view(7, texture, {}, true), 1u);
    EXPECT_EQ(view(8, texture, {}, true), 1u);
    device = first;
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
