// SPDX-License-Identifier: LGPL-2.1-only
//
// UAV counters, pixel-shader UAVs, view descriptions and packed depth-stencil copies.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../graphics/uav_counter_shaders.h"
#include "../../graphics/pixel_uav_shaders.h"

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// 11.6: a shader moves the hidden counter. The GPU behaviour is proven by
// UavCounterTest (vitreo_graphics_metal_tests) on real fxc output; these check
// the frontend puts each counter at the binding the translator chose, and
// refuses to run a shader whose counter has nothing bound.
// ---------------------------------------------------------------------------

class D3D11UavCounterTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr CS_PAGE = SO_BASE + 0x1000;

    void SetUp() override {
        D3D11SurfaceTest::SetUp();
        ASSERT_TRUE(memory.map_region(CS_PAGE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    GuestAddr compute_shader(const unsigned char* bytecode, usize size) {
        EXPECT_TRUE(memory.write_bytes(CS_PAGE, bytecode, size).is_ok());
        EXPECT_EQ(device_call(18, {CS_PAGE, size, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    /// A structured (8-byte stride) or ALLOW_RAW_VIEWS buffer with UAV bind.
    GuestAddr uav_buffer(u32 misc, u32 stride) {
        write_words(DESC, {70 * 8, 0, 0x80, 0, misc, stride});
        EXPECT_EQ(device_call(3, {DESC, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    /// CreateUnorderedAccessView over a buffer: {Format, BUFFER, First,
    /// Count, Flags}. Returns the HRESULT; the view is at OUT.
    u32 buffer_uav(GuestAddr buffer, u32 format, u32 count, u32 flags) {
        write_words(DESC, {format, 1, 0, count, flags});
        return device_call(8, {buffer, DESC, OUT});
    }

    void dispatch_with_u1(GuestAddr shader, GuestAddr view) {
        context_call(69, {shader, 0, 0});  // CSSetShader
        ASSERT_TRUE(memory.write_u64(OUT2, view).is_ok());
        context_call(68, {1, 1, OUT2, 0});  // CSSetUnorderedAccessViews(u1)
        context_call(41, {1, 1, 1});        // Dispatch
    }
};

TEST_F(D3D11UavCounterTest, AppendShaderGetsTheViewsCounterAtItsBinding) {
    const GuestAddr shader = compute_shader(uav_counter_append_main,
                                            sizeof(uav_counter_append_main));
    ASSERT_NE(shader, 0u);
    const GuestAddr buffer = uav_buffer(0x40 /* STRUCTURED */, 8);
    ASSERT_EQ(buffer_uav(buffer, 0, 70, 0x2 /* APPEND */), d3d11::S_OK);
    const GuestAddr append = memory.read_u64(OUT).value();

    const unsigned before = backend.state.dispatches;
    dispatch_with_u1(shader, append);
    ASSERT_EQ(backend.state.dispatches, before + 1);
    const auto& bound = backend.state.last_dispatch_buffers;
    const auto* counter =
        bound[graphics::BACKEND_UAV_COUNTER_BUFFER_BASE + 0];
    ASSERT_NE(counter, nullptr) << "u1's counter at binding 0";
    EXPECT_NE(counter, bound[graphics::BACKEND_UAV_BUFFER_BASE + 1])
        << "the counter is not the buffer";
    for (u32 k = 1; k < graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS; ++k) {
        EXPECT_EQ(bound[graphics::BACKEND_UAV_COUNTER_BUFFER_BASE + k],
                  nullptr);
    }

    // The same buffer through a view with no counter: the shader would move
    // a counter through an unbound Metal buffer, so nothing is dispatched.
    ASSERT_EQ(buffer_uav(buffer, 0, 70, 0), d3d11::S_OK);
    const GuestAddr plain = memory.read_u64(OUT).value();
    dispatch_with_u1(shader, plain);
    EXPECT_EQ(backend.state.dispatches, before + 1);
    dispatch_with_u1(shader, 0);
    EXPECT_EQ(backend.state.dispatches, before + 1) << "nothing bound";
    dispatch_with_u1(shader, append);
    EXPECT_EQ(backend.state.dispatches, before + 2);
}

// ---------------------------------------------------------------------------
// Pixel-stage UAVs through OMSetRenderTargetsAndUnorderedAccessViews. The GPU
// behaviour of each blob is proven by PixelUavTest
// (vitreo_graphics_metal_tests); these check the frontend creates the shaders
// Windows creates (measured: tools/oracle_probe/d3d11_stage_uav_probe.expected)
// and puts every u# at the fragment binding its shape needs.
// ---------------------------------------------------------------------------

class D3D11PixelUavTest : public D3D11UavCounterTest {
protected:
    GuestAddr pixel_shader(const unsigned char* bytecode, usize size) {
        EXPECT_TRUE(memory.write_bytes(CS_PAGE, bytecode, size).is_ok());
        EXPECT_EQ(device_call(15, {CS_PAGE, size, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    /// A 16x16 texture of @p format with @p bind, and its default view from
    /// Create{RenderTarget,UnorderedAccess}View (@p view_slot 9 or 8).
    GuestAddr texture_view(u32 format, u32 bind, usize view_slot) {
        write_words(DESC, {16, 16, 1, 1, format, 1, 0, 0, bind, 0, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        const GuestAddr resource = memory.read_u64(OUT).value();
        EXPECT_EQ(device_call(view_slot, {resource, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }

    /// RWBuffer<uint>: a DEFAULT buffer with UAV bind and an R32_UINT view.
    GuestAddr typed_buffer_view(u32 elements) {
        write_words(DESC, {elements * 4, 0, 0x80, 0, 0, 0});
        EXPECT_EQ(device_call(3, {DESC, 0, OUT}), d3d11::S_OK);
        const GuestAddr buffer = memory.read_u64(OUT).value();
        EXPECT_EQ(buffer_uav(buffer, 42 /* R32_UINT */, elements, 0),
                  d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }
};

// The Satisfactory failure: CreatePixelShader of a ps_5_0 that uses
// store_uav_typed returned E_FAIL. Measured on Windows at feature level 11_0,
// every one of these returns S_OK.
TEST_F(D3D11PixelUavTest, PixelShadersUsingUavsAreCreated) {
    for (const auto& [blob, size] :
         {std::pair{pixel_uav_ps_typed_texture,
                    sizeof(pixel_uav_ps_typed_texture)},
          std::pair{pixel_uav_ps_typed_buffer,
                    sizeof(pixel_uav_ps_typed_buffer)},
          std::pair{pixel_uav_ps_raw_structured,
                    sizeof(pixel_uav_ps_raw_structured)},
          std::pair{pixel_uav_ps_early_coherent,
                    sizeof(pixel_uav_ps_early_coherent)}}) {
        EXPECT_NE(pixel_shader(blob, size), 0u);
    }
}

// Measured: at feature level 11_0 CreateVertexShader of a vs_5_0 that writes
// a UAV returns E_INVALIDARG and NULLs the out-pointer; with a NULL
// out-pointer (validation only) it is E_INVALIDARG too.
TEST_F(D3D11PixelUavTest, VertexShaderUavIsInvalidBelowFeatureLevel11_1) {
    ASSERT_TRUE(memory.write_bytes(CS_PAGE, pixel_uav_vs_uav,
                                   sizeof(pixel_uav_vs_uav)).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT, POISON).is_ok());
    EXPECT_EQ(device_call(12, {CS_PAGE, sizeof(pixel_uav_vs_uav), 0, OUT}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    EXPECT_EQ(device_call(12, {CS_PAGE, sizeof(pixel_uav_vs_uav), 0, 0}),
              d3d11::E_INVALIDARG);
}

// Learn (OMSetRenderTargetsAndUnorderedAccessViews): for the pixel stage the
// UAVs share the render targets' slots and UAVStartSlot is the absolute u#.
// RTV at slot 0, then u1 = RWTexture2D<float4>, u2 = RWBuffer<uint>. The
// texture reaches the fragment texture UAV at u1; the typed buffer reaches it
// as a WRITABLE texture buffer at u2 (the shader declares a read_write
// texture_buffer there), not as a word buffer.
TEST_F(D3D11PixelUavTest, DrawBindsOmUavsAtTheirShapesFragmentSlots) {
    const GuestAddr rtv = texture_view(28 /* RGBA8_UNORM */, 0x20, 9);
    const GuestAddr image =
        texture_view(2 /* R32G32B32A32_FLOAT */, 0x80, 8);
    const usize creations_before = backend.state.texture_buffer_creations.size();
    const GuestAddr counts = typed_buffer_view(64);
    ASSERT_NE(rtv, 0u);
    ASSERT_NE(image, 0u);
    ASSERT_NE(counts, 0u);
    ASSERT_EQ(backend.state.texture_buffer_creations.size(),
              creations_before + 1);
    const auto& made = backend.state.texture_buffer_creations.back();
    EXPECT_TRUE(made.shader_write) << "a UAV's texture view must be writable";
    EXPECT_EQ(made.pixel_format, graphics::PixelFormat::R32Uint);
    EXPECT_EQ(made.first_element, 0u);
    EXPECT_EQ(made.num_elements, 64u);

    const GuestAddr vs = make_vertex_shader();
    ASSERT_TRUE(memory.write_u64(OUT2, rtv).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT2 + 8, image).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT2 + 16, counts).is_ok());
    // OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, NULL, 1, 2, uavs,
    // NULL).
    context_call(34, {1, OUT2, 0, 1, 2, OUT2 + 8, 0});
    context_call(11, {vs, 0, 0});
    context_call(24, {4});  // TRIANGLELIST

    for (const auto& [blob, size] :
         {std::pair{pixel_uav_ps_typed_texture,
                    sizeof(pixel_uav_ps_typed_texture)},
          std::pair{pixel_uav_ps_typed_buffer,
                    sizeof(pixel_uav_ps_typed_buffer)}}) {
        const GuestAddr ps = pixel_shader(blob, size);
        ASSERT_NE(ps, 0u);
        context_call(9, {ps, 0, 0});
        const unsigned draws = backend.state.draws;
        context_call(13, {3, 0});  // Draw(3, 0)
        ASSERT_EQ(backend.state.draws, draws + 1);
        EXPECT_TRUE(backend.state.last_draw_uav_texture_bound[1]);
        EXPECT_FALSE(backend.state.last_draw_uav_buffer_bound[1]);
        EXPECT_NE(backend.state.last_draw_uav_texture_buffers[2], nullptr);
        EXPECT_FALSE(backend.state.last_draw_uav_buffer_bound[2])
            << "a typed buffer is not bound as a word buffer";
        EXPECT_FALSE(backend.state.last_draw_uav_texture_bound[0])
            << "slot 0 is the render target";
    }
}

// The compute stage had the same gap: a RWBuffer<T> UAV bound only its words
// at the buffer window, while the kernel reads a texture_buffer at the UAV
// texture window.
TEST_F(D3D11PixelUavTest, DispatchBindsTypedBufferUavAsATextureBuffer) {
    const GuestAddr shader = compute_shader(uav_counter_append_main,
                                            sizeof(uav_counter_append_main));
    ASSERT_NE(shader, 0u);
    const GuestAddr structured = uav_buffer(0x40 /* STRUCTURED */, 8);
    ASSERT_EQ(buffer_uav(structured, 0, 70, 0x2 /* APPEND */), d3d11::S_OK);
    const GuestAddr append = memory.read_u64(OUT).value();
    const GuestAddr counts = typed_buffer_view(64);
    context_call(69, {shader, 0, 0});  // CSSetShader
    ASSERT_TRUE(memory.write_u64(OUT2, append).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT2 + 8, counts).is_ok());
    context_call(68, {1, 2, OUT2, 0});  // CSSetUnorderedAccessViews(u1, u2)
    const unsigned before = backend.state.dispatches;
    context_call(41, {1, 1, 1});        // Dispatch
    ASSERT_EQ(backend.state.dispatches, before + 1);
    EXPECT_NE(backend.state.last_dispatch_texture_buffers
                  [graphics::BACKEND_UAV_TEXTURE_BASE + 2],
              nullptr);
    EXPECT_EQ(backend.state.last_dispatch_buffers
                  [graphics::BACKEND_UAV_BUFFER_BASE + 2],
              nullptr)
        << "a typed buffer is not bound as a word buffer";
    EXPECT_NE(backend.state.last_dispatch_buffers
                  [graphics::BACKEND_UAV_BUFFER_BASE + 1],
              nullptr);
}

TEST_F(D3D11UavCounterTest, RawBufferViewsAreServed) {
    // Learn (D3D11_BUFFER_UAV_FLAG_RAW): R32_TYPELESS over an ALLOW_RAW_VIEWS
    // buffer, counted in 4-byte elements -- the raw SRV's rule.
    const GuestAddr raw = uav_buffer(0x20 /* ALLOW_RAW_VIEWS */, 0);
    EXPECT_EQ(buffer_uav(raw, 39, 140, 0x1), d3d11::S_OK);
    EXPECT_NE(memory.read_u64(OUT).value(), 0u);
    EXPECT_EQ(buffer_uav(raw, 39, 141, 0x1), d3d11::E_INVALIDARG)
        << "one word past the end";
    EXPECT_EQ(buffer_uav(raw, 39, 4, 0x1 | 0x2), d3d11::E_INVALIDARG)
        << "a raw view has no counter";
    EXPECT_EQ(buffer_uav(raw, 42, 4, 0x1), d3d11::E_INVALIDARG)
        << "R32_UINT is not the raw format";
}

// ---------------------------------------------------------------------------
// View descriptors and resources, line for line against
// tools/oracle_probe/d3d11_view_desc_probe.expected (and the window, raw
// alignment and 1D-array UAV probes beside it).
// ---------------------------------------------------------------------------

class D3D11ViewDescTest : public D3D11SurfaceTest {
protected:
    GuestAddr make(usize slot, std::initializer_list<u32> desc) {
        write_words(DESC, desc);
        EXPECT_EQ(device_call(slot, {DESC, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }
    /// Create a view (7 SRV, 8 UAV, 9 RTV); returns the HRESULT, view at OUT.
    u32 view(usize slot, GuestAddr resource, std::initializer_list<u32> desc) {
        write_words(DESC, desc);
        return device_call(slot, {resource, desc.size() == 0 ? 0 : DESC, OUT});
    }
    std::vector<u32> desc_words(usize slot, GuestAddr view, u32 count) {
        for (u32 i = 0; i < 8; ++i) {
            EXPECT_TRUE(memory.write_u32(OUT2 + 4 * i, 0xCDCDCDCDu).is_ok());
        }
        set_arg(0, view);
        set_arg(1, OUT2);
        if (slot == 7) d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
        if (slot == 8) d3d11.impl_UnorderedAccessViewMethod(marshaler, 8);
        if (slot == 9) d3d11.impl_RenderTargetViewMethod(marshaler, 8);
        std::vector<u32> words;
        for (u32 i = 0; i < count; ++i) {
            words.push_back(memory.read_u32(OUT2 + 4 * i).value());
        }
        EXPECT_EQ(memory.read_u32(OUT2 + 4 * count).value(), 0xCDCDCDCDu)
            << "nothing written past the DESC";
        return words;
    }
    GuestAddr resource_of(usize slot, GuestAddr view) {
        EXPECT_TRUE(memory.write_u64(OUT2, POISON).is_ok());
        set_arg(0, view);
        set_arg(1, OUT2);
        if (slot == 7) d3d11.impl_ShaderResourceViewMethod(marshaler, 7);
        if (slot == 8) d3d11.impl_UnorderedAccessViewMethod(marshaler, 7);
        if (slot == 9) d3d11.impl_RenderTargetViewMethod(marshaler, 7);
        return memory.read_u64(OUT2).value();
    }
    using W = std::vector<u32>;
};

TEST_F(D3D11ViewDescTest, BufferViewsReportTheirWindowAndResource) {
    // D3D11_BUFFER_DESC {ByteWidth, Usage, Bind(SRV|UAV), CPU, Misc, Stride}.
    const GuestAddr typed = make(3, {256, 0, 0x88, 0, 0, 0});
    const GuestAddr raw = make(3, {256, 0, 0x88, 0, 0x20, 0});
    const GuestAddr structured = make(3, {256, 0, 0x88, 0, 0x40, 8});

    ASSERT_EQ(view(7, typed, {41, 1, 2, 3}), d3d11::S_OK);
    GuestAddr v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(7, v, 6), (W{41, 1, 2, 3, 0, 0}));
    EXPECT_EQ(resource_of(7, v), typed) << "a buffer view has a resource";

    ASSERT_EQ(view(7, raw, {39, 11, 4, 5, 1}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(7, v, 6), (W{39, 11, 4, 5, 1, 0}));

    ASSERT_EQ(view(8, structured, {0, 1, 1, 6, 2}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(8, v, 5), (W{0, 1, 1, 6, 2}));
    EXPECT_EQ(resource_of(8, v), structured);

    // Raw views: FirstElement a multiple of 4 (d3d11_raw_view_alignment).
    EXPECT_EQ(view(8, raw, {39, 1, 3, 7, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, raw, {39, 1, 4, 7, 1}), d3d11::S_OK);
    EXPECT_EQ(view(8, raw, {39, 1, 61, 3, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, raw, {39, 1, 60, 4, 1}), d3d11::S_OK);
    EXPECT_EQ(view(7, raw, {39, 11, 2, 4, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(7, raw, {39, 11, 60, 4, 1}), d3d11::S_OK);
}

TEST_F(D3D11ViewDescTest, OneDimensionalArrayViewsKeepTheirWindow) {
    // D3D11_TEXTURE1D_DESC {Width, Mips, ArraySize, Format, Usage,
    // Bind(SRV|RTV|UAV), CPU, Misc}.
    const GuestAddr strips = make(4, {16, 3, 4, 41, 0, 0xA8, 0, 0});
    ASSERT_NE(strips, 0u);

    ASSERT_EQ(view(7, strips, {41, 3, 1, 0xFFFFFFFFu, 1, 2}), d3d11::S_OK);
    GuestAddr v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(7, v, 6), (W{41, 3, 1, 2, 1, 2}))
        << "MipLevels -1 resolved";
    EXPECT_EQ(resource_of(7, v), strips);
    ASSERT_EQ(view(7, strips, {41, 2, 0, 2}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(7, v, 6), (W{41, 2, 0, 2, 0, 0}));

    ASSERT_EQ(view(8, strips, {41, 3, 1, 2, 2}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(8, v, 5), (W{41, 3, 1, 2, 2}));
    // d3d11_uav_1d_array_probe: -1 is the rest, 0 and overruns refused.
    EXPECT_EQ(view(8, strips, {41, 3, 0, 3, 2}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, strips, {41, 3, 0, 4, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(8, strips, {41, 3, 0, 0, 0}), d3d11::E_INVALIDARG);
    ASSERT_EQ(view(8, strips, {41, 3, 0, 2, 0xFFFFFFFFu}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(8, v, 5), (W{41, 3, 0, 2, 2}));

    ASSERT_EQ(view(9, strips, {41, 3, 2, 1, 0xFFFFFFFFu}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(9, v, 5), (W{41, 3, 2, 1, 3}));
    ASSERT_EQ(view(9, strips, {41, 2, 1}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(9, v, 5), (W{41, 2, 1, 0, 0}))
        << "a TEXTURE1D view is one slice, not the array";
    ASSERT_EQ(device_call(9, {strips, 0, OUT}), d3d11::S_OK);
    v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(9, v, 5), (W{41, 3, 0, 0, 4})) << "NULL desc";
}

TEST_F(D3D11ViewDescTest, VolumeRenderTargetResolvesItsWWindow) {
    // D3D11_TEXTURE3D_DESC {W, H, D, Mips, Format, Usage, Bind(RTV), CPU,
    // Misc}.
    const GuestAddr volume = make(6, {8, 8, 8, 2, 41, 0, 0x20, 0, 0});
    ASSERT_NE(volume, 0u);
    ASSERT_EQ(view(9, volume, {41, 8, 1, 1, 0xFFFFFFFFu}), d3d11::S_OK);
    const GuestAddr v = memory.read_u64(OUT).value();
    EXPECT_EQ(desc_words(9, v, 5), (W{41, 8, 1, 1, 3}));
}

TEST_F(D3D11ViewDescTest, ArrayWindowsResolveMinusOneAndRefuseOverruns) {
    // d3d11_view_window_probe: 8x8, 3 mips, 4 slices, SRV|RTV.
    const GuestAddr array =
        make(5, {8, 8, 3, 4, 41, 1, 0, 0, 0x28, 0, 0});
    ASSERT_NE(array, 0u);
    ASSERT_EQ(view(9, array, {41, 5, 1, 1, 0xFFFFFFFFu}), d3d11::S_OK);
    EXPECT_EQ(desc_words(9, memory.read_u64(OUT).value(), 5),
              (W{41, 5, 1, 1, 3}));
    ASSERT_EQ(view(7, array, {41, 5, 0, 1, 2, 0xFFFFFFFFu}), d3d11::S_OK);
    EXPECT_EQ(desc_words(7, memory.read_u64(OUT).value(), 6),
              (W{41, 5, 0, 1, 2, 2}));
    EXPECT_EQ(view(7, array, {41, 5, 0, 1, 1, 5}), d3d11::E_INVALIDARG)
        << "past the last slice: malformed, not unsupported";
}

// ---------------------------------------------------------------------------
// 11.14: GPU -> staging copies of packed depth+stencil, in the layout
// tools/oracle_probe/d3d11_packed_depth_readback_probe.expected measured.
// ---------------------------------------------------------------------------

class D3D11PackedDepthTest : public D3D11SurfaceTest {
protected:
    /// A 4x2 texture of @p format: DEFAULT with DEPTH_STENCIL bind, or
    /// STAGING with CPU read.
    GuestAddr depth_texture(u32 format, bool staging) {
        write_words(DESC, {4, 2, 1, 1, format, 1, 0, staging ? 3u : 0u,
                           staging ? 0u : 0x40u, staging ? 0x20000u : 0u, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }
    /// Map(READ) subresource 0; returns the data pointer, pitch in *pitch.
    GuestAddr map(GuestAddr texture, u32& pitch) {
        context_call(14, {texture, 0, 1, 0, OUT2});
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        pitch = memory.read_u32(OUT2 + 8).value();
        return memory.read_u64(OUT2).value();
    }
    static u32 d24(u32 x, u32 y) {
        const double depth = static_cast<double>(x + 4 * y) / 16.0;
        return static_cast<u32>(std::lrint(depth * 16777215.0)) |
               ((x + 16u * y) << 24);
    }
};

TEST_F(D3D11PackedDepthTest, CopyResourcePacksD24UnormS8IntoOneWord) {
    const GuestAddr gpu = depth_texture(45, false);
    const GuestAddr staging = depth_texture(45, true);
    ASSERT_NE(gpu, 0u);
    ASSERT_NE(staging, 0u);
    context_call(47, {staging, gpu});  // CopyResource
    u32 pitch = 0;
    const GuestAddr data = map(staging, pitch);
    ASSERT_NE(data, 0u);
    EXPECT_EQ(pitch, 16u) << "four bytes a texel, as Windows reports";
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            EXPECT_EQ(memory.read_u32(data + y * pitch + x * 4).value(),
                      d24(x, y))
                << "texel " << x << "," << y;
        }
    }
    // The measured 0.25 / 0x5A word is this rule's (x=0,y=1)-like case:
    // unorm24(0.25) = 0x400000.
    EXPECT_EQ(d24(0, 1) & 0x00FFFFFFu, 0x400000u);
}

TEST_F(D3D11PackedDepthTest, RegionCopyPacksD32FloatS8X24AtItsOffset) {
    const GuestAddr gpu = depth_texture(20, false);
    const GuestAddr staging = depth_texture(20, true);
    ASSERT_NE(gpu, 0u);
    ASSERT_NE(staging, 0u);
    // Learn (CopySubresourceRegion): a depth-stencil resource copies whole
    // subresources only -- pSrcBox NULL, DstX/Y/Z 0 -- and Windows drops a
    // boxed depth copy (a06 probe: CPY D32 region with a box). This test
    // used to copy a (1..3, 0..1) box; the oracle says that is dropped.
    write_words(DESC + 0x40, {1, 0, 0, 3, 2, 1});
    context_call(46, {staging, 0, 0, 0, 0, gpu, 0, DESC + 0x40});
    u32 pitch = 0;
    GuestAddr data = map(staging, pitch);
    ASSERT_NE(data, 0u);
    EXPECT_EQ(memory.read_u32(data).value(), 0u) << "the boxed copy is dropped";
    context_call(15, {staging, 0});  // Unmap
    context_call(46, {staging, 0, 0, 0, 0, gpu, 0, 0});
    data = map(staging, pitch);
    ASSERT_NE(data, 0u);
    EXPECT_EQ(pitch, 32u) << "eight bytes a texel";
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const GuestAddr texel = data + y * pitch + x * 8;
            const u32 depth_bits = memory.read_u32(texel).value();
            float depth = 0.0f;
            std::memcpy(&depth, &depth_bits, 4);
            EXPECT_FLOAT_EQ(depth, static_cast<float>(x + 4 * y) / 16.0f);
            EXPECT_EQ(memory.read_u32(texel + 4).value(), x + 16 * y)
                << "stencil, then three zero bytes";
        }
    }
}

// CreateDepthStencilView across every shape, line for line against
// tools/oracle_probe/d3d11_dsv_probe.expected.
TEST_F(D3D11ViewDescTest, DepthStencilViewsFollowTheMeasuredShapes) {
    // D32_FLOAT (40) with DEPTH_STENCIL bind (0x40).
    const GuestAddr single = make(5, {16, 16, 2, 1, 40, 1, 0, 0, 0x40, 0, 0});
    const GuestAddr array = make(5, {16, 16, 2, 4, 40, 1, 0, 0, 0x40, 0, 0});
    const GuestAddr ms = make(5, {16, 16, 1, 1, 40, 4, 0, 0, 0x40, 0, 0});
    const GuestAddr ms_array = make(5, {16, 16, 1, 2, 40, 4, 0, 0, 0x40, 0, 0});
    const GuestAddr line = make(4, {16, 2, 1, 40, 0, 0x40, 0, 0});
    const GuestAddr lines = make(4, {16, 2, 4, 40, 0, 0x40, 0, 0});
    ASSERT_NE(single, 0u);
    ASSERT_NE(array, 0u);
    ASSERT_NE(ms, 0u);
    ASSERT_NE(ms_array, 0u);
    ASSERT_NE(line, 0u);
    ASSERT_NE(lines, 0u);

    // {Format, Dimension, Flags, union[3]}; `null` passes no desc.
    const auto dsv = [&](GuestAddr res, std::vector<u32> desc, bool null) {
        write_words(DESC, {40, desc[0], 0, desc[1], desc[2], desc[3]});
        return device_call(10, {res, null ? 0 : DESC, OUT});
    };
    const auto check = [&](const char* label, GuestAddr res,
                           std::vector<u32> desc, bool null, W want) {
        ASSERT_EQ(dsv(res, desc, null), d3d11::S_OK) << label;
        const GuestAddr v = memory.read_u64(OUT).value();
        for (u32 i = 0; i < 8; ++i) {
            ASSERT_TRUE(memory.write_u32(OUT2 + 4 * i, 0xCDCDCDCDu).is_ok());
        }
        set_arg(0, v);
        set_arg(1, OUT2);
        d3d11.impl_DepthStencilViewMethod(marshaler, 8);
        W got;
        for (u32 i = 0; i < 6; ++i) {
            got.push_back(memory.read_u32(OUT2 + 4 * i).value());
        }
        EXPECT_EQ(got, want) << label;
    };
    constexpr u32 all = 0xFFFFFFFFu;
    check("2d NULL", single, {0, 0, 0, 0}, true, {40, 3, 0, 0, 0, 0});
    check("2d as 2DARRAY {1,0,1}", single, {4, 1, 0, 1}, false,
          {40, 4, 0, 1, 0, 1});
    check("2d as 2DARRAY {0,0,-1}", single, {4, 0, 0, all}, false,
          {40, 4, 0, 0, 0, 1});
    check("array NULL", array, {0, 0, 0, 0}, true, {40, 4, 0, 0, 0, 4});
    check("array as 2D {1}", array, {3, 1, 0, 0}, false, {40, 3, 0, 1, 0, 0});
    check("array 2DARRAY {0,1,-1}", array, {4, 0, 1, all}, false,
          {40, 4, 0, 0, 1, 3});
    EXPECT_EQ(dsv(array, {4, 0, 3, 2}, false), d3d11::E_INVALIDARG);
    EXPECT_EQ(dsv(array, {4, 0, 1, 0}, false), d3d11::E_INVALIDARG);
    check("ms NULL", ms, {0, 0, 0, 0}, true, {40, 5, 0, 0, 0, 0});
    check("ms 2DMS", ms, {5, 0x55, 0, 0}, false, {40, 5, 0, 0, 0, 0});
    EXPECT_EQ(dsv(ms, {3, 0, 0, 0}, false), d3d11::E_INVALIDARG);
    check("ms_array NULL", ms_array, {0, 0, 0, 0}, true, {40, 6, 0, 0, 2, 0});
    check("ms_array 2DMSARRAY {1,-1}", ms_array, {6, 1, all, 0}, false,
          {40, 6, 0, 1, 1, 0});
    check("ms_array as 2DMS", ms_array, {5, 0, 0, 0}, false,
          {40, 5, 0, 0, 0, 0});
    check("1d NULL", line, {0, 0, 0, 0}, true, {40, 1, 0, 0, 0, 0});
    check("1d 1D {1}", line, {1, 1, 0, 0}, false, {40, 1, 0, 1, 0, 1});
    EXPECT_EQ(dsv(line, {3, 0, 0, 0}, false), d3d11::E_INVALIDARG);
    check("1darray NULL", lines, {0, 0, 0, 0}, true, {40, 2, 0, 0, 0, 4});
    check("1darray 1DARRAY {0,1,-1}", lines, {2, 0, 1, all}, false,
          {40, 2, 0, 0, 1, 3});
    check("1darray as 1D", lines, {1, 0, 0, 0}, false, {40, 1, 0, 0, 0, 1});
    EXPECT_EQ(dsv(single, {1, 0, 0, 0}, false), d3d11::E_INVALIDARG);
}

// Multisampled depth and the depth families' format support, against
// tools/oracle_probe/d3d11_msaa_depth_probe.expected and
// d3d11_depth_format_support_probe.expected.
TEST_F(D3D11ViewDescTest, MultisampledDepthIsCreatableAndReportedAsOnWindows) {
    const auto create = [&](u32 format, u32 samples, u32 mips, u32 bind) {
        write_words(DESC, {16, 16, mips, 1, format, samples, 0, 0, bind, 0, 0});
        return device_call(5, {DESC, 0, OUT});
    };
    constexpr u32 DS = 0x40, SRV = 0x8, RT = 0x20, UAV = 0x80;
    EXPECT_EQ(create(40, 4, 1, DS), d3d11::S_OK) << "D32 x4 DS";
    EXPECT_EQ(create(45, 4, 1, DS), d3d11::S_OK) << "D24S8 x4 DS";
    EXPECT_EQ(create(39, 4, 1, DS | SRV), d3d11::S_OK) << "R32_TYPELESS";
    EXPECT_EQ(create(44, 4, 1, DS | SRV), d3d11::S_OK) << "R24G8_TYPELESS";
    EXPECT_EQ(create(40, 4, 2, DS), d3d11::E_INVALIDARG) << "mips=2";
    EXPECT_EQ(create(40, 4, 1, DS | SRV), d3d11::E_INVALIDARG)
        << "typed depth cannot be a shader resource";
    EXPECT_EQ(create(28, 4, 1, RT | UAV), d3d11::E_INVALIDARG) << "MS UAV";
    EXPECT_EQ(create(28, 4, 1, SRV), d3d11::S_OK) << "MS SRV only";
    EXPECT_EQ(create(40, 1, 1, DS | SRV), d3d11::E_INVALIDARG)
        << "the typed-depth rule holds at one sample too";
    EXPECT_EQ(create(45, 1, 1, DS | SRV), d3d11::E_INVALIDARG);
    EXPECT_EQ(create(39, 1, 1, SRV), d3d11::S_OK);

    // One quality level at x1/x2/x4 for depth formats and their families.
    for (const u32 format : {40u, 45u, 20u, 55u, 39u, 44u, 28u}) {
        for (const u32 samples : {1u, 2u, 4u}) {
            ASSERT_TRUE(memory.write_u32(OUT, 0xABABu).is_ok());
            EXPECT_EQ(device_call(30, {format, samples, OUT}), d3d11::S_OK);
            EXPECT_EQ(memory.read_u32(OUT).value(), 1u)
                << "format " << format << " x" << samples;
        }
    }

    // CheckFormatSupport: exactly the measured words.
    const auto support = [&](u32 format) {
        EXPECT_EQ(device_call(29, {format, OUT}), d3d11::S_OK);
        return memory.read_u32(OUT).value();
    };
    constexpr u32 held_back = 0u;
    EXPECT_EQ(support(40), 0x003310b0u & ~held_back) << "D32_FLOAT";
    EXPECT_EQ(support(45), 0x003310b0u & ~held_back) << "D24_UNORM_S8_UINT";
    EXPECT_EQ(support(44), 0x001210b0u & ~held_back) << "R24G8_TYPELESS";
    EXPECT_EQ(support(46), 0x04d217b0u & ~held_back) << "R24_UNORM_X8";
    EXPECT_EQ(support(47), 0x005211b0u & ~held_back) << "X24_TYPELESS_G8";
    EXPECT_NE(support(40) & 0x200000u, 0u) << "MULTISAMPLE_RENDERTARGET";
    EXPECT_EQ(support(40) & 0x300u, 0u) << "no shader bits on a typed D*";
}

// Depth cube maps, against tools/oracle_probe/d3d11_depth_cube_probe.expected.
TEST_F(D3D11ViewDescTest, DepthCubesAreCreatableWithFaceViews) {
    const auto cube = [&](u32 format, u32 faces, u32 bind) {
        // MiscFlags TEXTURECUBE (0x4).
        write_words(DESC, {16, 16, 1, faces, format, 1, 0, 0, bind, 0, 0x4});
        return device_call(5, {DESC, 0, OUT});
    };
    constexpr u32 DS = 0x40, SRV = 0x8;
    EXPECT_EQ(cube(40, 6, DS), d3d11::S_OK) << "D32 cube";
    EXPECT_EQ(cube(44, 6, DS | SRV), d3d11::S_OK) << "R24G8_TYPELESS cube";
    EXPECT_EQ(cube(39, 12, DS | SRV), d3d11::S_OK) << "cube array";
    ASSERT_EQ(cube(39, 6, DS | SRV), d3d11::S_OK);
    const GuestAddr c = memory.read_u64(OUT).value();
    // One face and all six as a TEXTURE2DARRAY DSV; the typeless resource
    // has no NULL-desc DSV.
    write_words(DESC, {40, 4, 0, 0, 2, 1});
    EXPECT_EQ(device_call(10, {c, DESC, OUT}), d3d11::S_OK) << "face 2";
    write_words(DESC, {40, 4, 0, 0, 0, 6});
    EXPECT_EQ(device_call(10, {c, DESC, OUT}), d3d11::S_OK) << "all faces";
    EXPECT_EQ(device_call(10, {c, 0, OUT}), d3d11::E_INVALIDARG);
    // TEXTURECUBE SRV (dimension 9) as R32_FLOAT.
    write_words(DESC, {41, 9, 0, 1, 0, 0});
    EXPECT_EQ(device_call(7, {c, DESC, OUT}), d3d11::S_OK);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
