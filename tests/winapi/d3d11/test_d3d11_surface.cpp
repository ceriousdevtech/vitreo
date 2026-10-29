// SPDX-License-Identifier: LGPL-2.1-only
//
// The rest of the D3D11 interface surface, map-on-default and standard swizzle.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11SurfaceTest, CountersAreAbsentAsOnWindows) {
    ASSERT_TRUE(memory.write_u64(OUT, 0xABABABABABABABABull).is_ok());
    ASSERT_TRUE(memory.write_u64(OUT + 8, 0xABABABABABABABABull).is_ok());
    device_call(31, {OUT});  // CheckCounterInfo
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 4).value(), 0u);
    EXPECT_EQ(memory.read_u8(OUT + 8).value(), 1u);
    write_words(DESC, {0x40000000u, 0});  // DEVICE_DEPENDENT_0
    ASSERT_TRUE(memory.write_u64(OUT2, POISON).is_ok());
    EXPECT_EQ(device_call(26, {DESC, OUT2}), d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(OUT2).value(), POISON) << "left untouched";
    EXPECT_EQ(device_call(32, {DESC, 0, 0, 0, 0, 0, 0, 0, 0}),
              d3d11::E_INVALIDARG);
}

TEST_F(D3D11SurfaceTest, BlendState1ReportsLogicOpsAndRefusesThem) {
    // DESC1: AlphaToCoverage, Independent, then 8 x 40-byte targets.
    const auto target = [&](u32 index, u32 blend, u32 logic, u32 op) {
        const GuestAddr base = DESC + 8 + index * 40;
        write_words(base, {blend, logic, 2, 1, 1, 2, 1, 1, op, 0xF});
    };
    write_words(DESC, {0, 0});
    for (u32 i = 0; i < 8; ++i) target(i, 0, 0, 4);
    ASSERT_EQ(device_call(45, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr state = memory.read_u64(OUT).value();
    set_arg(0, state);
    set_arg(1, OUT2);
    for (u32 i = 0; i < 88; i += 4) {
        ASSERT_TRUE(memory.write_u32(OUT2 + i, 0xCDCDCDCDu).is_ok());
    }
    d3d11.impl_BlendStateMethod(marshaler, 8);  // GetDesc1
    EXPECT_EQ(memory.read_u32(OUT2 + 8 + 4).value(), 0u) << "LogicOpEnable";
    EXPECT_EQ(memory.read_u32(OUT2 + 8 + 32).value(), 4u) << "LogicOp NOOP";
    EXPECT_EQ(memory.read_u32(OUT2 + 8 + 36).value(), 0xFu) << "write mask";

    // A logic op with blending, or with independent blend: refused (measured).
    target(0, 1, 1, 6);
    EXPECT_EQ(device_call(45, {DESC, OUT}), d3d11::E_INVALIDARG);
    // A lone logic op is fine on Windows hardware, but needs
    // OutputMergerLogicOp, which this device reports FALSE.
    target(0, 0, 1, 6);
    EXPECT_EQ(device_call(45, {DESC, OUT}), d3d11::E_INVALIDARG);
    // An out-of-range LogicOp with logic off is not validated, and GetDesc1
    // reports NOOP for it -- a disabled logic op is canonicalised
    // (d3d11_a04_state_objects_probe "blend1 getdesc1 logicop=AND disabled";
    // this expected 99 back before the oracle measured it).
    target(0, 0, 0, 99);
    ASSERT_EQ(device_call(45, {DESC, OUT}), d3d11::S_OK);
    set_arg(0, memory.read_u64(OUT).value());
    set_arg(1, OUT2);
    d3d11.impl_BlendStateMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(OUT2 + 8 + 32).value(), 4u);
}

TEST_F(D3D11SurfaceTest, Device1CreatorsValidateTheirNewFields) {
    // CreateRasterizerState1: ForcedSampleCount 3 refused, 4 accepted at FL
    // 11_0 (measured, d3d11_a04_state_objects_probe "rs1 forced=3/4"; 4 was
    // E_NOTIMPL here before UAV-only draws honoured the count).
    write_words(DESC, {3, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0});
    EXPECT_EQ(device_call(46, {DESC, OUT}), d3d11::S_OK);
    write_words(DESC + 40, {3});
    EXPECT_EQ(device_call(46, {DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC + 40, {4});
    EXPECT_EQ(device_call(46, {DESC, OUT}), d3d11::S_OK);

    // CreateTexture2D1: without CPU access, layouts other than UNDEFINED are
    // refused on a DEFAULT texture (measured, d3d11_texmap_probe [create]
    // "u ... c0"); D3D11MapOnDefaultTest covers the rest.
    write_words(DESC, {16, 16, 1, 1, 28, 1, 0, 0, 0x8, 0, 0, 0});
    ASSERT_EQ(device_call(54, {DESC, 0, OUT}), d3d11::S_OK);
    const GuestAddr tex1 = memory.read_u64(OUT).value();
    set_arg(0, tex1);
    set_arg(1, OUT2);
    ASSERT_TRUE(memory.write_u32(OUT2 + 44, 0xCDu).is_ok());
    d3d11.impl_Texture2DMethod(marshaler, 11);  // GetDesc1
    EXPECT_EQ(memory.read_u32(OUT2).value(), 16u);
    EXPECT_EQ(memory.read_u32(OUT2 + 44).value(), 0u) << "TextureLayout";
    for (const u32 layout : {1u, 2u, 7u}) {
        write_words(DESC + 44, {layout});
        ASSERT_TRUE(memory.write_u64(OUT, POISON).is_ok());
        EXPECT_EQ(device_call(54, {DESC, 0, OUT}), d3d11::E_INVALIDARG);
        EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    }

    // View *1 creators: plane 1 refused for SRV, UAV and RTV (measured).
    const GuestAddr tex = texture(1, 0xA8);  // SRV|RTV|UAV
    write_words(DESC, {28, 4, 0, 1, 0, 0, 0});            // SRV1 Texture2D
    ASSERT_EQ(device_call(57, {tex, DESC, OUT}), d3d11::S_OK);
    write_words(DESC + 16, {1});
    EXPECT_EQ(device_call(57, {tex, DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC, {28, 4, 0, 1, 0, 0});               // UAV1/RTV1 Texture2D
    EXPECT_EQ(device_call(58, {tex, DESC, OUT}), d3d11::E_INVALIDARG);
    EXPECT_EQ(device_call(59, {tex, DESC, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC + 12, {0});
    EXPECT_EQ(device_call(59, {tex, DESC, OUT}), d3d11::S_OK);

    // CreateQuery1: ContextType kept; 9 refused (measured).
    write_words(DESC, {0, 0, 2});
    ASSERT_EQ(device_call(60, {DESC, OUT}), d3d11::S_OK);
    set_arg(0, memory.read_u64(OUT).value());
    set_arg(1, OUT2);
    d3d11.impl_QueryMethod(marshaler, 9);
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 2u);
    write_words(DESC, {0, 0, 9});
    EXPECT_EQ(device_call(60, {DESC, OUT}), d3d11::E_INVALIDARG);

    // CheckMultisampleQualityLevels1: unknown flags ignored (measured); a
    // tiled resource has no levels on a device without tiled resources.
    EXPECT_EQ(device_call(53, {28, 4, 0, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
    EXPECT_EQ(device_call(53, {28, 4, 2, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
    EXPECT_EQ(device_call(53, {28, 4, 1, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);

    // CreateDeferredContext1: ContextFlags must be 0 (measured).
    EXPECT_EQ(device_call(44, {1, OUT}), d3d11::E_INVALIDARG);
}

TEST_F(D3D11SurfaceTest, TilingAnnotationsProtectionAndRemovalEvents) {
    const GuestAddr tex = texture();
    for (u32 i = 0; i < 64; i += 4) {
        ASSERT_TRUE(memory.write_u32(OUT + i, 0xCDCDCDCDu).is_ok());
    }
    device_call(52, {tex, OUT, OUT + 4, OUT + 16, OUT + 28, 0, 0});
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT + 28).value(), 0u) << "no subresource tilings";

    context_call(134, {tex, 1, OUT, OUT, 0, 0, 0, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "UpdateTileMappings";
    context_call(138, {tex, 131072});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "ResizeTilePool";
    context_call(140, {});
    EXPECT_EQ(cpu.gpr.rax, 0u) << "IsAnnotationEnabled";

    context_call(146, {OUT});
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    context_call(145, {1});
    context_call(146, {OUT});
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u) << "stored, as on Windows";

    EXPECT_EQ(device_call(65, {0, OUT}), 0x887A0001u) << "NULL event";
    ASSERT_TRUE(memory.write_u32(OUT, 0xABABABABu).is_ok());
    EXPECT_EQ(device_call(65, {0x1234, OUT}), d3d11::S_OK);
    EXPECT_NE(memory.read_u32(OUT).value(), 0xABABABABu) << "cookie written";
}

TEST_F(D3D11SurfaceTest, GpuEventsAreSetThroughTheBridge) {
    auto& signaler = GpuEventSignaler::instance();
    std::mutex mutex;
    std::vector<u64> signalled;
    signaler.set_signal_function([&](u64 handle) {
        std::lock_guard<std::mutex> lock(mutex);
        signalled.push_back(handle);
    });
    // Flush1(ContextType, hEvent): the event is set once the flush is done.
    context_call(144, {0, 0x4444});
    for (int i = 0; i < 2000 && signaler.pending() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(std::count(signalled.begin(), signalled.end(), 0x4444u), 1);
    }

    // ID3D11Fence::SetEventOnCompletion: a reached value sets the event at
    // once; an unreached one waits, and releasing the fence drops the wait.
    const std::array<u8, 16> fence_iid{
        0xD1, 0xE9, 0xFD, 0xAF, 0xF7, 0x1D, 0xB7, 0x4B,
        0x8A, 0x34, 0x0F, 0x46, 0x25, 0x1D, 0xAB, 0x80};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(fence_iid.data()), 16).is_ok());
    ASSERT_EQ(device_call(68, {5, 0, DESC, OUT}), d3d11::S_OK);
    const GuestAddr fence = memory.read_u64(OUT).value();
    set_arg(0, fence);
    set_arg(1, 5);
    set_arg(2, 0x5555);
    d3d11.impl_FenceMethod(marshaler, 9);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(std::count(signalled.begin(), signalled.end(), 0x5555u), 1);
    }
    set_arg(0, fence);
    set_arg(1, 9);
    set_arg(2, 0x6666);
    d3d11.impl_FenceMethod(marshaler, 9);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(signaler.pending(), 1u);
    set_arg(0, fence);
    d3d11.impl_FenceMethod(marshaler, 2);  // Release
    EXPECT_EQ(signaler.pending(), 0u) << "cancelled with the fence";
    signaler.set_signal_function(nullptr);
}

TEST_F(D3D11SurfaceTest, DeviceContextStateSwapsBindingsAsOnWindows) {
    // ID3D11Device1 {a04bfb29-08ef-43d6-a49c-a9bdbdcbe686}
    const std::array<u8, 16> device1{
        0x29, 0xFB, 0x4B, 0xA0, 0xEF, 0x08, 0xD6, 0x43,
        0xA4, 0x9C, 0xA9, 0xBD, 0xBD, 0xCB, 0xE6, 0x86};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(device1.data()), 16).is_ok());
    write_words(DESC + 0x20, {0xb000});  // FL 11_0
    ASSERT_EQ(device_call(47, {0, DESC + 0x20, 1, 7, DESC, OUT2, OUT}),
              d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0xb000u) << "chosen level";
    const GuestAddr state = memory.read_u64(OUT).value();
    EXPECT_EQ(device_call(47, {0, DESC + 0x20, 1, 7, DESC, 0, 0}), 1u)
        << "NULL out is the validation-only S_FALSE";

    // Bind a viewport, swap in the fresh state: it has none (measured).
    write_words(DESC + 0x40, {0x3F800000u, 0x40000000u, 0x41F00000u,
                              0x42200000u, 0, 0x3F800000u});
    context_call(44, {1, DESC + 0x40});  // RSSetViewports
    context_call(131, {state, OUT});     // SwapDeviceContextState
    const GuestAddr previous = memory.read_u64(OUT).value();
    EXPECT_NE(previous, 0u);
    ASSERT_TRUE(memory.write_u32(OUT2, 1).is_ok());
    context_call(95, {OUT2, DESC + 0x80});  // RSGetViewports
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0u) << "fresh state";

    // Swapping back restores the viewport and hands back `state`.
    context_call(131, {previous, OUT});
    EXPECT_EQ(memory.read_u64(OUT).value(), state);
    ASSERT_TRUE(memory.write_u32(OUT2, 1).is_ok());
    context_call(95, {OUT2, DESC + 0x80});
    EXPECT_EQ(memory.read_u32(OUT2).value(), 1u);
    EXPECT_EQ(memory.read_u32(DESC + 0x80 + 8).value(), 0x41F00000u) << "w=30";
}

TEST_F(D3D11SurfaceTest, ClearViewClearsWholeViewsAndRects) {
    const GuestAddr tex = texture(1, 0x20);  // RTV
    ASSERT_EQ(device_call(9, {tex, 0, OUT}), d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(OUT).value();
    write_words(DESC, {0x3F800000u, 0, 0, 0x3F800000u});  // red
    const unsigned clears = backend.state.texture_clears;
    context_call(132, {rtv, DESC, 0, 0});
    EXPECT_EQ(backend.state.texture_clears, clears + 1) << "whole view";

    // Two rects: one fill each of the view's converted texel (red in
    // R8G8B8A8_UNORM: ff 00 00 ff) at the rect's own position. A
    // D3D11_RECT is {left, top, right, bottom}. This used to clear a
    // per-call scratch texture and copy each rect out of it.
    write_words(DESC + 0x20, {4, 4, 8, 8, 0, 0, 2, 2});
    backend.state.texel_fills.clear();
    context_call(132, {rtv, DESC, DESC + 0x20, 2});
    ASSERT_EQ(backend.state.texel_fills.size(), 2u);
    const auto rect = [&](usize i) {
        const auto& f = backend.state.texel_fills[i];
        return std::array<unsigned, 4>{f.x, f.y, f.width, f.height};
    };
    EXPECT_EQ(rect(0), (std::array<unsigned, 4>{4, 4, 4, 4}));
    EXPECT_EQ(rect(1), (std::array<unsigned, 4>{0, 0, 2, 2}));
    EXPECT_EQ(backend.state.texel_fills[0].texel,
              (std::vector<u8>{0xFF, 0x00, 0x00, 0xFF}));
}

TEST_F(D3D11SurfaceTest, TexturesAreDxgiSurfaces) {
    // IDXGISurface {cafcb56c-6ac3-4889-bf47-9e23bbd260ec}
    const std::array<u8, 16> surface_iid{
        0x6C, 0xB5, 0xFC, 0xCA, 0xC3, 0x6A, 0x89, 0x48,
        0xBF, 0x47, 0x9E, 0x23, 0xBB, 0xD2, 0x60, 0xEC};
    ASSERT_TRUE(memory.write_bytes(
        DESC + 0x80, reinterpret_cast<const Byte*>(surface_iid.data()),
        16).is_ok());
    const GuestAddr tex = texture();
    set_arg(0, tex);
    set_arg(1, DESC + 0x80);
    set_arg(2, OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr surface = memory.read_u64(OUT).value();
    EXPECT_NE(surface, tex);
    set_arg(0, surface);
    set_arg(1, OUT2);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 8);  // GetDesc
    EXPECT_EQ(memory.read_u32(OUT2).value(), 16u);
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 28u) << "format";
    EXPECT_EQ(memory.read_u32(OUT2 + 12).value(), 1u) << "sample count";

    // A texture with mips is not one surface.
    const GuestAddr mipped = texture(2);
    set_arg(0, mipped);
    set_arg(1, DESC + 0x80);
    set_arg(2, OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_NOINTERFACE);
}

TEST_F(D3D11SurfaceTest, ViewportsAndScissorsRoundTripAsOnWindows) {
    // tools/oracle_probe/d3d11_rs_state_probe.expected, line for line.
    const auto get = [&](usize slot, u32 ask) {
        for (u32 i = 0; i < 0x100; i += 4) {
            ASSERT_TRUE(memory.write_u32(OUT2 + i, 0xCDCDCDCDu).is_ok());
        }
        ASSERT_TRUE(memory.write_u32(OUT, ask).is_ok());
        context_call(slot, {OUT, OUT2});
    };
    get(95, 2);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u) << "fresh: none bound";
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 0u) << "zeroed, not left";

    // Three viewports; asking for five returns three and zeroes two.
    write_words(DESC, {0, 0, 0x41F00000u, 0x42200000u, 0, 0x3F800000u,
                       0x3F800000u, 0x3F800000u, 0x42480000u, 0x42700000u, 0,
                       0x3F800000u,
                       0x40000000u, 0x40000000u, 0x428C0000u, 0x42A00000u, 0,
                       0x3F800000u});
    context_call(44, {3, DESC});
    get(95, 5);
    EXPECT_EQ(memory.read_u32(OUT).value(), 3u);
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 0x41F00000u);
    EXPECT_EQ(memory.read_u32(OUT2 + 24 + 8).value(), 0x42480000u);
    EXPECT_EQ(memory.read_u32(OUT2 + 48 + 8).value(), 0x428C0000u);
    EXPECT_EQ(memory.read_u32(OUT2 + 72 + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT2 + 96 + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT2 + 120).value(), 0xCDCDCDCDu)
        << "nothing past the requested count";
    get(95, 1);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);

    // A shorter set unbinds the rest; zero unbinds all.
    context_call(44, {1, DESC + 24});
    get(95, 3);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 0x42480000u);
    EXPECT_EQ(memory.read_u32(OUT2 + 24 + 8).value(), 0u);
    context_call(44, {0, 0});
    get(95, 2);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    context_call(44, {2, DESC});
    context_call(110, {});  // ClearState
    get(95, 2);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u) << "ClearState";

    // Scissor rects follow the same rules.
    write_words(DESC, {0, 0, 10, 10, 1, 1, 20, 20, 2, 2, 30, 30});
    context_call(45, {3, DESC});
    get(96, 5);
    EXPECT_EQ(memory.read_u32(OUT).value(), 3u);
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 10u);
    EXPECT_EQ(memory.read_u32(OUT2 + 40).value(), 30u);
    EXPECT_EQ(memory.read_u32(OUT2 + 56).value(), 0u);
    context_call(45, {1, DESC + 32});
    get(96, 3);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 30u);
    EXPECT_EQ(memory.read_u32(OUT2 + 24).value(), 0u);
}

// ---------------------------------------------------------------------------
// Map on DEFAULT textures and the 64K standard swizzle (D3D11.3,
// D3D11_OPTIONS2.MapOnDefaultTextures / StandardSwizzle). Every expected value
// is Windows's (WARP): tools/oracle_probe/d3d11_texmap_probe.expected, the
// quoted label naming its line; box semantics are Learn's
// (ID3D11Device3::WriteToSubresource / ReadFromSubresource).
// ---------------------------------------------------------------------------

class D3D11MapOnDefaultTest : public D3D11SurfaceTest {
protected:
    static constexpr GuestAddr SCRATCH = 0x40000;  // 64 KB
    static constexpr u32 MAP_READ = 1, MAP_WRITE = 2, MAP_WRITE_DISCARD = 4;

    void SetUp() override {
        D3D11SurfaceTest::SetUp();
        ASSERT_TRUE(memory.map_region(SCRATCH, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
    }
    // D3D11_TEXTURE2D_DESC1: W H Mips Array Format Count Quality Usage Bind
    // CPU Misc Layout.
    u32 create2d1(u32 w, u32 h, u32 mips, u32 format, u32 usage, u32 bind,
                  u32 cpu, u32 misc, u32 layout, GuestAddr* out = nullptr) {
        write_words(DESC, {w, h, mips, 1, format, 1, 0, usage, bind, cpu,
                           misc, layout});
        (void)memory.write_u64(OUT, POISON);
        const u32 hr = device_call(54, {DESC, 0, OUT});
        if (out != nullptr) *out = memory.read_u64(OUT).value();
        return hr;
    }
    u32 layout_of(GuestAddr texture) {
        set_arg(0, texture);
        set_arg(1, OUT2);
        EXPECT_TRUE(memory.write_u32(OUT2 + 44, 0xCDCDCDCDu).is_ok());
        d3d11.impl_Texture2DMethod(marshaler, 11);  // GetDesc1
        return memory.read_u32(OUT2 + 44).value();
    }
    u32 map(GuestAddr resource, u32 sub, u32 type, u32 flags,
            GuestAddr mapped) {
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, context);
        set_arg(1, resource);
        set_arg(2, sub);
        set_arg(3, type);
        set_arg(4, flags);
        set_arg(5, mapped);
        d3d11.impl_ContextMethod(marshaler, 14);
        return static_cast<u32>(cpu.gpr.rax);
    }
    void unmap(GuestAddr resource, u32 sub) {
        set_arg(0, context);
        set_arg(1, resource);
        set_arg(2, sub);
        d3d11.impl_ContextMethod(marshaler, 15);
    }
    // ID3D11Device3::WriteToSubresource (63) / ReadFromSubresource (64).
    void write_to(GuestAddr resource, u32 sub, GuestAddr box, GuestAddr src,
                  u32 row_pitch, u32 depth_pitch) {
        (void)device_call(63, {resource, sub, box, src, row_pitch, depth_pitch});
    }
    void read_from(GuestAddr dst, u32 row_pitch, u32 depth_pitch,
                   GuestAddr resource, u32 sub, GuestAddr box) {
        (void)device_call(64, {dst, row_pitch, depth_pitch, resource, sub, box});
    }
};

TEST_F(D3D11MapOnDefaultTest, OptionsReportMapOnDefaultAndStandardSwizzle) {
    // "caps OPTIONS2 ... MapOnDefaultTextures=1 StandardSwizzle=1"
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(memory.write_u32(SCRATCH + i * 4, 0xFFFFFFFFu).is_ok());
    }
    ASSERT_EQ(device_call(33, {14, SCRATCH, 32}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH + 20).value(), 1u) << "MapOnDefaultTextures";
    EXPECT_EQ(memory.read_u32(SCRATCH + 24).value(), 1u) << "StandardSwizzle";
}

TEST_F(D3D11MapOnDefaultTest, CreationRulesAsWarp) {
    constexpr u32 RW = 0x30000, R = 0x20000;
    GuestAddr tex = 0;
    // "C a 2D1L0 ... b28 c30000": any bind but depth-stencil takes CPU access.
    EXPECT_EQ(create2d1(64, 64, 1, 28, 0, 0x28, RW, 0, 0), d3d11::S_OK);
    EXPECT_EQ(create2d1(64, 64, 1, 28, 0, 0x88, RW, 0, 0), d3d11::S_OK);
    // "C f 2D1L0 ... f45 ... b40 c30000": E_INVALIDARG.
    EXPECT_EQ(create2d1(64, 64, 1, 45, 0, 0x40, RW, 0, 0), d3d11::E_INVALIDARG);
    // "C x 2D1L0 ... b8 c30000 x80": RESOURCE_CLAMP refused.
    EXPECT_EQ(create2d1(64, 64, 1, 28, 0, 0x8, RW, 0x80, 0),
              d3d11::E_INVALIDARG);
    // "C f 2D ... f6 ... c30000": R32G32B32 refused on a 2D texture.
    EXPECT_EQ(create2d1(64, 64, 1, 6, 0, 0, RW, 0, 0), d3d11::E_INVALIDARG);
    // ROW_MAJOR: no bind, one mip ("C a 2D1L1 b8 c30000" IA, "C s 2D1L1
    // m3" IA, "C a 2D1L1 b0 c30000" layout=1).
    EXPECT_EQ(create2d1(64, 64, 1, 28, 0, 0x8, RW, 0, 1), d3d11::E_INVALIDARG);
    EXPECT_EQ(create2d1(64, 64, 3, 28, 0, 0, RW, 0, 1), d3d11::E_INVALIDARG);
    ASSERT_EQ(create2d1(64, 64, 1, 28, 0, 0, RW, 0, 1, &tex), d3d11::S_OK);
    EXPECT_EQ(layout_of(tex), 1u);
    // 64K_STANDARD_SWIZZLE with any bind and mips ("C s 2D1L2 m0 b8").
    ASSERT_EQ(create2d1(64, 64, 0, 28, 0, 0x28, RW, 0, 2, &tex), d3d11::S_OK);
    EXPECT_EQ(layout_of(tex), 2u);
    // A layout needs CPU access on a DEFAULT texture ("C a 2D1L2 b0 c0").
    EXPECT_EQ(create2d1(64, 64, 1, 28, 0, 0, 0, 0, 2), d3d11::E_INVALIDARG);
    // ... but not on a STAGING one, which keeps it ("C u 2D1L2 u3 c20000").
    ASSERT_EQ(create2d1(64, 64, 1, 28, 3, 0, R, 0, 2, &tex), d3d11::S_OK);
    EXPECT_EQ(layout_of(tex), 2u);
    // Values past the enum are stored ("O 2D1 layout=0x80000000 cpu").
    ASSERT_EQ(create2d1(64, 64, 1, 28, 0, 0, RW, 0, 0x80000000u, &tex),
              d3d11::S_OK);
    EXPECT_EQ(layout_of(tex), 0x80000000u);
    // Multisampled textures take no CPU access ("C s 2D ... ms4 c30000").
    write_words(DESC, {64, 64, 1, 1, 28, 4, 0, 0, 0x20, RW, 0, 0});
    EXPECT_EQ(device_call(54, {DESC, 0, OUT}), d3d11::E_INVALIDARG);
    // D3D11_TEXTURE3D_DESC1: W H D Mips Format Usage Bind CPU Misc Layout.
    // A volume with CPU access is one mip unless standard-swizzled ("C s 3D1L0
    // 16x16x4 m3" IA, "C s 3D1L2 16x16x4 m3" S_OK).
    write_words(DESC, {16, 16, 4, 3, 28, 0, 0, RW, 0, 0});
    EXPECT_EQ(device_call(55, {DESC, 0, OUT}), d3d11::E_INVALIDARG);
    write_words(DESC, {16, 16, 4, 3, 28, 0, 0, RW, 0, 2});
    EXPECT_EQ(device_call(55, {DESC, 0, OUT}), d3d11::S_OK);
}

TEST_F(D3D11MapOnDefaultTest, MapRulesOfAnUndefinedLayout) {
    GuestAddr tex = 0;
    ASSERT_EQ(create2d1(4, 4, 1, 28, 0, 0, 0x30000, 0, 0, &tex), d3d11::S_OK);
    backend.state.native_texture_readback.assign(64, 0);
    // "M 2D1L0 b0 sub0 t1 f0 NULL" S_OK; "... out" E_INVALIDARG: an
    // undefined layout has no CPU pointer.
    EXPECT_EQ(map(tex, 0, MAP_READ, 0, SCRATCH), d3d11::E_INVALIDARG);
    ASSERT_EQ(map(tex, 0, MAP_READ, 0, 0), d3d11::S_OK);
    // "twice NULL READ,READ,WRITE": a mapped subresource refuses NULL maps.
    EXPECT_EQ(map(tex, 0, MAP_READ, 0, 0), d3d11::E_INVALIDARG);
    unmap(tex, 0);
    // WRITE_DISCARD, type 0/6, flag bit 0: E_INVALIDARG; WRITE_NO_OVERWRITE
    // (5) and DO_NOT_WAIT accepted.
    EXPECT_EQ(map(tex, 0, MAP_WRITE_DISCARD, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(map(tex, 0, 6, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(map(tex, 0, MAP_READ, 1, 0), d3d11::E_INVALIDARG);
    ASSERT_EQ(map(tex, 0, 5, 0, 0), d3d11::S_OK);
    unmap(tex, 0);
    ASSERT_EQ(map(tex, 0, MAP_READ, 0x100000, 0), d3d11::S_OK);
    unmap(tex, 0);
    // Subresource past the end.
    EXPECT_EQ(map(tex, 1, MAP_READ, 0, 0), d3d11::E_INVALIDARG);
    // "M 2D1L0 c20000 sub0 t2": a READ-only texture refuses WRITE.
    ASSERT_EQ(create2d1(4, 4, 1, 28, 0, 0, 0x20000, 0, 0, &tex), d3d11::S_OK);
    EXPECT_EQ(map(tex, 0, MAP_WRITE, 0, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(map(tex, 0, 3, 0, 0), d3d11::E_INVALIDARG);
    // A STAGING texture never maps with a NULL pointer ("undef" 14).
    ASSERT_EQ(create2d1(4, 4, 1, 28, 3, 0, 0x30000, 0, 0, &tex), d3d11::S_OK);
    EXPECT_EQ(map(tex, 0, MAP_READ, 0, 0), d3d11::E_INVALIDARG);
}

TEST_F(D3D11MapOnDefaultTest, WriteAndReadSubresourceBoxes) {
    GuestAddr tex = 0;
    ASSERT_EQ(create2d1(4, 4, 1, 28, 0, 0, 0x30000, 0, 0, &tex), d3d11::S_OK);
    // The GPU's texels: byte b of texel (x, y) = 16y + 4x + b.
    auto& gpu = backend.state.native_texture_readback;
    gpu.resize(64);
    for (u32 i = 0; i < 64; ++i) gpu[i] = static_cast<u8>(i);
    const GuestAddr BOX = SCRATCH;
    const GuestAddr DATA = SCRATCH + 0x100;
    write_words(BOX, {1, 1, 0, 3, 3, 1});
    // Read: the box's two rows of two texels at DstRowPitch 12, nothing else
    // written (Learn ReadFromSubresource).
    ASSERT_EQ(map(tex, 0, MAP_READ, 0, 0), d3d11::S_OK);
    for (u32 i = 0; i < 32; ++i) {
        ASSERT_TRUE(memory.write_u8(DATA + i, 0xEE).is_ok());
    }
    read_from(DATA, 12, 24, tex, 0, BOX);
    for (u32 b = 0; b < 8; ++b) {
        EXPECT_EQ(memory.read_u8(DATA + b).value(), 16 + 4 + b);
        EXPECT_EQ(memory.read_u8(DATA + 12 + b).value(), 32 + 4 + b);
    }
    EXPECT_EQ(memory.read_u8(DATA + 8).value(), 0xEEu) << "row padding";
    // A write into a READ mapping is gone at its Unmap ("undef" 2).
    const u32 updates = backend.state.texture_updates;
    write_to(tex, 0, BOX, DATA, 12, 24);
    unmap(tex, 0);
    EXPECT_EQ(backend.state.texture_updates, updates);
    // WRITE: only the box changes; the Unmap stores the whole subresource in
    // order with the recorded work ([data] "after write").
    ASSERT_EQ(map(tex, 0, MAP_WRITE, 0, 0), d3d11::S_OK);
    for (u32 i = 0; i < 24; ++i) {
        ASSERT_TRUE(memory.write_u8(DATA + i, static_cast<u8>(0xA0 + i)).is_ok());
    }
    write_to(tex, 0, BOX, DATA, 12, 24);
    unmap(tex, 0);
    ASSERT_EQ(backend.state.texture_updates, updates + 1);
    EXPECT_EQ((backend.state.last_texture_update_region),
              (std::array<unsigned, 6>{0, 0, 0, 0, 4, 4}));
    std::vector<u8> expected = gpu;
    for (u32 b = 0; b < 8; ++b) {
        expected[16 + 4 + b] = static_cast<u8>(0xA0 + b);
        expected[32 + 4 + b] = static_cast<u8>(0xA0 + 12 + b);
    }
    EXPECT_EQ(backend.state.last_texture_update, expected);
    // An empty box is a no-op; so is a WRITE mapping nothing wrote into
    // ([data] "after empty write", "after empty boxes").
    write_words(BOX, {2, 0, 0, 2, 1, 1});
    ASSERT_EQ(map(tex, 0, MAP_WRITE, 0, 0), d3d11::S_OK);
    write_to(tex, 0, BOX, DATA, 12, 24);
    unmap(tex, 0);
    EXPECT_EQ(backend.state.texture_updates, updates + 1);
    // Not mapped: undefined on Windows (WARP faults); ignored here.
    write_words(BOX, {0, 0, 0, 1, 1, 1});
    write_to(tex, 0, BOX, DATA, 4, 4);
    EXPECT_EQ(backend.state.texture_updates, updates + 1);
}

TEST_F(D3D11MapOnDefaultTest, RowMajorAndTexture1DPitches) {
    // "P 2D1L1 f61 3x5 [4 20]": rows rounded up to 4 bytes.
    GuestAddr tex = 0;
    ASSERT_EQ(create2d1(3, 5, 1, 61, 0, 0, 0x30000, 0, 1, &tex), d3d11::S_OK);
    auto& gpu = backend.state.native_texture_readback;
    gpu.resize(15);
    for (u32 i = 0; i < 15; ++i) gpu[i] = static_cast<u8>(0x10 + i);
    const GuestAddr MAPPED = SCRATCH;
    ASSERT_EQ(map(tex, 0, MAP_READ, 0, MAPPED), d3d11::S_OK);
    const GuestAddr data = memory.read_u64(MAPPED).value();
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 4u);
    EXPECT_EQ(memory.read_u32(MAPPED + 12).value(), 20u);
    EXPECT_EQ(memory.read_u8(data + 4).value(), 0x13u) << "row 1 at RowPitch";
    EXPECT_EQ(memory.read_u8(data + 3).value(), 0u) << "row padding";
    unmap(tex, 0);
    // A second pointer map of a mapped subresource hands back the same one
    // ("twice out ... same=1").
    ASSERT_EQ(map(tex, 0, MAP_READ, 0, MAPPED), d3d11::S_OK);
    ASSERT_EQ(map(tex, 0, MAP_READ, 0, MAPPED + 16), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(MAPPED + 16).value(), data);
    unmap(tex, 0);
    // "P 1D f49 3 [8 8]": a DEFAULT Texture1D maps through a pointer with
    // the same rounding. D3D11_TEXTURE1D_DESC: W Mips Array Format Usage
    // Bind CPU Misc.
    write_words(DESC, {3, 1, 1, 49, 0, 0, 0x30000, 0});
    ASSERT_EQ(device_call(4, {DESC, 0, OUT}), d3d11::S_OK);
    const GuestAddr t1d = memory.read_u64(OUT).value();
    gpu.assign(6, 0x42);
    ASSERT_EQ(map(t1d, 0, MAP_READ, 0, MAPPED), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 8u);
    EXPECT_EQ(memory.read_u32(MAPPED + 12).value(), 8u);
    unmap(t1d, 0);
}

TEST_F(D3D11MapOnDefaultTest, StandardSwizzleMappingThroughMapAndUnmap) {
    // 256x2 RGBA8, 64K_STANDARD_SWIZZLE: two 128x128 pages side by side
    // (RowPitch = DepthPitch = 2 pages, the [pitch] rule). Texel (x, y)
    // holds (y << 16) | x.
    GuestAddr tex = 0;
    ASSERT_EQ(create2d1(256, 2, 1, 28, 0, 0x8, 0x30000, 0, 2, &tex),
              d3d11::S_OK);
    auto& gpu = backend.state.native_texture_readback;
    gpu.resize(256 * 2 * 4);
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 256; ++x) {
            const u32 v = (y << 16) | x;
            std::memcpy(gpu.data() + (y * 256 + x) * 4, &v, 4);
        }
    }
    const GuestAddr MAPPED = SCRATCH;
    ASSERT_EQ(map(tex, 0, MAP_WRITE, 0, MAPPED), d3d11::S_OK);
    const GuestAddr data = memory.read_u64(MAPPED).value();
    EXPECT_EQ(memory.read_u32(MAPPED + 8).value(), 131072u);
    EXPECT_EQ(memory.read_u32(MAPPED + 12).value(), 131072u);
    // "S 2D1L2 f28 off=... -> x y": where each texel is.
    const std::array<std::array<u32, 3>, 9> at = {{
        {0x4, 1, 0}, {0x10, 0, 1}, {0x80, 4, 0}, {0x200, 8, 0},
        {0x800, 16, 0}, {0x2000, 32, 0}, {0x8000, 64, 0},
        {0x10000, 128, 0}, {0x10010, 128, 1}}};
    for (const auto& [offset, x, y] : at) {
        EXPECT_EQ(memory.read_u32(data + offset).value(), (y << 16) | x)
            << "offset 0x" << std::hex << offset;
    }
    // A write through the pointer reaches the texel the swizzle names.
    ASSERT_TRUE(memory.write_u32(data + 0x10, 0xDEADBEEFu).is_ok());
    unmap(tex, 0);
    std::vector<u8> expected = gpu;
    const u32 v = 0xDEADBEEFu;
    std::memcpy(expected.data() + 256 * 4, &v, 4);  // texel (0, 1)
    EXPECT_EQ(backend.state.last_texture_update, expected);
}

// The interleave masks against every offset WARP located (d3d11_texmap_probe
// [swz] "S 2D1L2/3D1L2 f<fmt> off=... -> x y z", extracted from the
// .expected): 2D 512x512 and 3D 128x64x64 texel textures, whose element
// grids are the BC blocks / packed pixel pairs of the listed format.
TEST(D3D11StandardSwizzle, InterleaveMatchesWarpOffsets) {
    struct Case {
        u32 dim;
        u32 format;
        u32 element_bytes;
        std::vector<std::array<u32, 4>> offsets;  // offset, x, y, z
    };
    const std::vector<Case> cases = {
    {2, 3, 16, {{0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 1, 0, 0}, {0x80, 2, 0, 0}, {0x100, 0, 4, 0}, {0x200, 4, 0, 0}, {0x400, 0, 8, 0}, {0x800, 8, 0, 0}, {0x1000, 0, 16, 0}, {0x2000, 16, 0, 0}, {0x4000, 0, 32, 0}, {0x8000, 32, 0, 0}, {0x10000, 64, 0, 0}, {0x20000, 128, 0, 0}, {0x40000, 256, 0, 0}, {0x80000, 0, 64, 0}, {0x100000, 0, 128, 0}, {0x200000, 0, 256, 0}}},
    {2, 11, 8, {{0x8, 1, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 2, 0, 0}, {0x80, 4, 0, 0}, {0x100, 0, 4, 0}, {0x200, 8, 0, 0}, {0x400, 0, 8, 0}, {0x800, 16, 0, 0}, {0x1000, 0, 16, 0}, {0x2000, 32, 0, 0}, {0x4000, 0, 32, 0}, {0x8000, 64, 0, 0}, {0x10000, 128, 0, 0}, {0x20000, 256, 0, 0}, {0x40000, 0, 64, 0}, {0x80000, 0, 128, 0}, {0x100000, 0, 256, 0}}},
    {2, 28, 4, {{0x4, 1, 0, 0}, {0x8, 2, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 4, 0}, {0x80, 4, 0, 0}, {0x100, 0, 8, 0}, {0x200, 8, 0, 0}, {0x400, 0, 16, 0}, {0x800, 16, 0, 0}, {0x1000, 0, 32, 0}, {0x2000, 32, 0, 0}, {0x4000, 0, 64, 0}, {0x8000, 64, 0, 0}, {0x10000, 128, 0, 0}, {0x20000, 256, 0, 0}, {0x40000, 0, 128, 0}, {0x80000, 0, 256, 0}}},
    {2, 49, 2, {{0x2, 1, 0, 0}, {0x4, 2, 0, 0}, {0x8, 4, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 4, 0}, {0x80, 8, 0, 0}, {0x100, 0, 8, 0}, {0x200, 16, 0, 0}, {0x400, 0, 16, 0}, {0x800, 32, 0, 0}, {0x1000, 0, 32, 0}, {0x2000, 64, 0, 0}, {0x4000, 0, 64, 0}, {0x8000, 128, 0, 0}, {0x10000, 256, 0, 0}, {0x20000, 0, 128, 0}, {0x40000, 0, 256, 0}}},
    {2, 61, 1, {{0x1, 1, 0, 0}, {0x2, 2, 0, 0}, {0x4, 4, 0, 0}, {0x8, 8, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 4, 0}, {0x80, 0, 8, 0}, {0x100, 0, 16, 0}, {0x200, 16, 0, 0}, {0x400, 0, 32, 0}, {0x800, 32, 0, 0}, {0x1000, 0, 64, 0}, {0x2000, 64, 0, 0}, {0x4000, 0, 128, 0}, {0x8000, 128, 0, 0}, {0x10000, 256, 0, 0}, {0x20000, 0, 256, 0}}},
    {2, 68, 4, {{0x4, 1, 0, 0}, {0x8, 2, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 4, 0}, {0x80, 4, 0, 0}, {0x100, 0, 8, 0}, {0x200, 8, 0, 0}, {0x400, 0, 16, 0}, {0x800, 16, 0, 0}, {0x1000, 0, 32, 0}, {0x2000, 32, 0, 0}, {0x4000, 0, 64, 0}, {0x8000, 64, 0, 0}, {0x10000, 128, 0, 0}, {0x20000, 0, 128, 0}, {0x40000, 0, 256, 0}}},
    {2, 71, 8, {{0x8, 1, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 2, 0, 0}, {0x80, 4, 0, 0}, {0x100, 0, 4, 0}, {0x200, 8, 0, 0}, {0x400, 0, 8, 0}, {0x800, 16, 0, 0}, {0x1000, 0, 16, 0}, {0x2000, 32, 0, 0}, {0x4000, 0, 32, 0}, {0x8000, 64, 0, 0}, {0x10000, 0, 64, 0}}},
    {2, 77, 16, {{0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 1, 0, 0}, {0x80, 2, 0, 0}, {0x100, 0, 4, 0}, {0x200, 4, 0, 0}, {0x400, 0, 8, 0}, {0x800, 8, 0, 0}, {0x1000, 0, 16, 0}, {0x2000, 16, 0, 0}, {0x4000, 0, 32, 0}, {0x8000, 32, 0, 0}, {0x10000, 64, 0, 0}, {0x20000, 0, 64, 0}}},
    {2, 107, 4, {{0x4, 1, 0, 0}, {0x8, 2, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 4, 0}, {0x80, 4, 0, 0}, {0x100, 0, 8, 0}, {0x200, 8, 0, 0}, {0x400, 0, 16, 0}, {0x800, 16, 0, 0}, {0x1000, 0, 32, 0}, {0x2000, 32, 0, 0}, {0x4000, 0, 64, 0}, {0x8000, 64, 0, 0}, {0x10000, 128, 0, 0}, {0x20000, 0, 128, 0}, {0x40000, 0, 256, 0}}},
    {3, 3, 16, {{0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 1, 0, 0}, {0x200, 2, 0, 0}, {0x400, 0, 0, 4}, {0x800, 0, 4, 0}, {0x1000, 4, 0, 0}, {0x2000, 0, 0, 8}, {0x4000, 0, 8, 0}, {0x8000, 8, 0, 0}, {0x10000, 16, 0, 0}, {0x20000, 32, 0, 0}, {0x40000, 64, 0, 0}, {0x80000, 0, 16, 0}, {0x100000, 0, 32, 0}, {0x200000, 0, 0, 16}, {0x400000, 0, 0, 32}}},
    {3, 11, 8, {{0x8, 1, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 2, 0, 0}, {0x200, 4, 0, 0}, {0x400, 0, 0, 4}, {0x800, 0, 4, 0}, {0x1000, 8, 0, 0}, {0x2000, 0, 0, 8}, {0x4000, 0, 8, 0}, {0x8000, 16, 0, 0}, {0x10000, 32, 0, 0}, {0x20000, 64, 0, 0}, {0x40000, 0, 16, 0}, {0x80000, 0, 32, 0}, {0x100000, 0, 0, 16}, {0x200000, 0, 0, 32}}},
    {3, 28, 4, {{0x4, 1, 0, 0}, {0x8, 2, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 0, 4, 0}, {0x200, 4, 0, 0}, {0x400, 0, 0, 4}, {0x800, 0, 8, 0}, {0x1000, 8, 0, 0}, {0x2000, 0, 0, 8}, {0x4000, 0, 16, 0}, {0x8000, 16, 0, 0}, {0x10000, 32, 0, 0}, {0x20000, 64, 0, 0}, {0x40000, 0, 32, 0}, {0x80000, 0, 0, 16}, {0x100000, 0, 0, 32}}},
    {3, 49, 2, {{0x2, 1, 0, 0}, {0x4, 2, 0, 0}, {0x8, 4, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 0, 0, 4}, {0x200, 0, 4, 0}, {0x400, 0, 0, 8}, {0x800, 0, 8, 0}, {0x1000, 8, 0, 0}, {0x2000, 0, 0, 16}, {0x4000, 0, 16, 0}, {0x8000, 16, 0, 0}, {0x10000, 32, 0, 0}, {0x20000, 64, 0, 0}, {0x40000, 0, 32, 0}, {0x80000, 0, 0, 32}}},
    {3, 61, 1, {{0x1, 1, 0, 0}, {0x2, 2, 0, 0}, {0x4, 4, 0, 0}, {0x8, 8, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 0, 0, 4}, {0x200, 0, 4, 0}, {0x400, 0, 0, 8}, {0x800, 0, 8, 0}, {0x1000, 16, 0, 0}, {0x2000, 0, 0, 16}, {0x4000, 0, 16, 0}, {0x8000, 32, 0, 0}, {0x10000, 64, 0, 0}, {0x20000, 0, 32, 0}, {0x40000, 0, 0, 32}}},
    {3, 71, 8, {{0x8, 1, 0, 0}, {0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 2, 0, 0}, {0x200, 4, 0, 0}, {0x400, 0, 0, 4}, {0x800, 0, 4, 0}, {0x1000, 8, 0, 0}, {0x2000, 0, 0, 8}, {0x4000, 0, 8, 0}, {0x8000, 16, 0, 0}, {0x10000, 0, 0, 16}, {0x20000, 0, 0, 32}}},
    {3, 77, 16, {{0x10, 0, 1, 0}, {0x20, 0, 2, 0}, {0x40, 0, 0, 1}, {0x80, 0, 0, 2}, {0x100, 1, 0, 0}, {0x200, 2, 0, 0}, {0x400, 0, 0, 4}, {0x800, 0, 4, 0}, {0x1000, 4, 0, 0}, {0x2000, 0, 0, 8}, {0x4000, 0, 8, 0}, {0x8000, 8, 0, 0}, {0x10000, 16, 0, 0}, {0x20000, 0, 0, 16}, {0x40000, 0, 0, 32}}},
    };
    for (const auto& c : cases) {
        const bool volume = c.dim == 3;
        const bool block = c.format == 71 || c.format == 77;
        const bool pair = c.format == 107 || c.format == 68;
        const u32 w = volume ? 128 : 512, h = volume ? 64 : 512;
        const u32 ew = block ? w / 4 : pair ? w / 2 : w;
        const u32 eh = block ? h / 4 : h;
        d3d11_swizzle::Layout layout{};
        ASSERT_TRUE(d3d11_swizzle::layout_for(c.element_bytes, volume, ew, eh,
                                              volume ? 64u : 1u, layout))
            << "format " << c.format;
        for (const auto& [offset, x, y, z] : c.offsets) {
            EXPECT_EQ(layout.offset(x, y, z), offset)
                << "format " << c.format << " dim " << c.dim << " (" << x
                << "," << y << "," << z << ")";
        }
    }
    // Pitches of a partial-page subresource ("P 2D1L2 f28 300x200
    // [196608 393216]", "P 3D1L2 f11 100x60x7 [262144 1048576]").
    d3d11_swizzle::Layout l{};
    ASSERT_TRUE(d3d11_swizzle::layout_for(4, false, 300, 200, 1, l));
    EXPECT_EQ(l.row_pitch(), 196608u);
    EXPECT_EQ(l.depth_pitch(), 393216u);
    ASSERT_TRUE(d3d11_swizzle::layout_for(8, true, 100, 60, 7, l));
    EXPECT_EQ(l.row_pitch(), 262144u);
    EXPECT_EQ(l.depth_pitch(), 1048576u);
    EXPECT_FALSE(d3d11_swizzle::layout_for(12, false, 4, 4, 1, l));
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
