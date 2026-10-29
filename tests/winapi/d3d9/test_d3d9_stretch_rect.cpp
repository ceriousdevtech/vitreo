/// @file test_d3d9_stretch_rect.cpp
/// @brief StretchRect and front-buffer readback.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, StretchRectUsesHostRegionBlit) {
    // Learn: StretchRect equal-size sub-rects prefer a host GPU blit.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DUSAGE_RENDERTARGET = 0x1;
    // CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, …)
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                      D3DPOOL_DEFAULT, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src_tex = memory_.read_u32(SCRATCH_ADDR).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                      D3DPOOL_DEFAULT, static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst_tex = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(src_tex, 0u);
    ASSERT_NE(dst_tex, 0u);

    // Texture GetSurfaceLevel → slot 18.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {src_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x10)}),
              D3D_OK);
    const u32 src_surf = memory_.read_u32(SCRATCH_ADDR + 0x10).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {dst_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x14)}),
              D3D_OK);
    const u32 dst_surf = memory_.read_u32(SCRATCH_ADDR + 0x14).value();
    ASSERT_NE(src_surf, 0u);
    ASSERT_NE(dst_surf, 0u);

    const i32 src_rect[4] = {1, 2, 5, 6}; // 4×4
    const i32 dst_rect[4] = {3, 1, 7, 5};
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x40,
                                 reinterpret_cast<const Byte*>(src_rect),
                                 sizeof(src_rect))
                    .is_ok());
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x50,
                                 reinterpret_cast<const Byte*>(dst_rect),
                                 sizeof(dst_rect))
                    .is_ok());

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 34,
                     {device, src_surf, static_cast<u32>(SCRATCH_ADDR + 0x40),
                      dst_surf, static_cast<u32>(SCRATCH_ADDR + 0x50),
                      1 /* D3DTEXF_POINT */}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_region_copies, 1u);
    EXPECT_EQ(backend_.record.last_region_src_x, 1u);
    EXPECT_EQ(backend_.record.last_region_src_y, 2u);
    EXPECT_EQ(backend_.record.last_region_dst_x, 3u);
    EXPECT_EQ(backend_.record.last_region_dst_y, 1u);
    EXPECT_EQ(backend_.record.last_region_width, 4u);
    EXPECT_EQ(backend_.record.last_region_height, 4u);
}

TEST_F(D3D9ManagerTest, StretchRectNoneRequiresEqualSize) {
    // Learn: StretchRect allows D3DTEXF_NONE, POINT, or LINEAR. NONE is the
    // copy filter — unequal source/dest rectangles are D3DERR_INVALIDCALL.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 16, 16, D3DFMT_A8R8G8B8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 8, 8, D3DFMT_A8R8G8B8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(src, 0u);
    ASSERT_NE(dst, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 34,
                     {device, src, 0, dst, 0, 0 /* D3DTEXF_NONE */}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.texture_stretches, 0u);
    EXPECT_EQ(backend_.record.texture_region_copies, 0u);
}

TEST_F(D3D9ManagerTest, StretchRectScalesRenderTargetsViaHostBlit) {
    // Learn: POINT/LINEAR may stretch DEFAULT→DEFAULT GPU render targets.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 16, 16, D3DFMT_A8R8G8B8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 8, 8, D3DFMT_A8R8G8B8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(src, 0u);
    ASSERT_NE(dst, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 34,
                     {device, src, 0, dst, 0, 1 /* D3DTEXF_POINT */}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_stretches, 1u);
    EXPECT_EQ(backend_.record.texture_region_copies, 0u);
    EXPECT_EQ(backend_.record.last_stretch_src_width, 16u);
    EXPECT_EQ(backend_.record.last_stretch_src_height, 16u);
    EXPECT_EQ(backend_.record.last_stretch_dst_width, 8u);
    EXPECT_EQ(backend_.record.last_stretch_dst_height, 8u);
    EXPECT_EQ(backend_.record.last_stretch_filter,
              graphics::BackendFilter::Nearest);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 34,
                     {device, src, 0, dst, 0, 2 /* D3DTEXF_LINEAR */}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_stretches, 2u);
    EXPECT_EQ(backend_.record.last_stretch_filter,
              graphics::BackendFilter::Linear);
}

TEST_F(D3D9ManagerTest, GetFrontBufferDataCopiesPresentedImage) {
    // Learn: GetFrontBufferData — SYSTEMMEM A8R8G8B8 at desktop/screen size.
    // Windowed Vitreo has no distinct front buffer; readback uses the
    // presentable back buffer after Clear/Present.
    mgr_.set_display_size_resolver([](u32& width, u32& height) {
        width = 640;
        height = 480;
        return true;
    });
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 640, 480, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(dest, 0u);

    // Clear(target) with opaque red → presentable back-buffer readback source.
    constexpr u32 D3DCLEAR_TARGET = 0x1;
    constexpr u32 clear_color = 0xFFFF0000; // opaque red
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_TARGET, clear_color, 0, 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 17,
                     {device, 0, 0, 0, 0}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 33, {device, 0, dest}), D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    // BGRA in lock memory: red → 0xFFFF0000 as DWORD on little-endian.
    EXPECT_EQ(memory_.read_u32(bits).value(), clear_color);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}

TEST_F(D3D9ManagerTest, SwapChainGetFrontBufferDataCopiesPresentedImage) {
    // Learn: IDirect3DSwapChain9::GetFrontBufferData shares the device helper
    // and reads this chain's presentable back buffer.
    mgr_.set_display_size_resolver([](u32& width, u32& height) {
        width = 640;
        height = 480;
        return true;
    });
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 14,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 swap_chain = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(swap_chain, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 640, 480, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 0x10), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR + 0x10).value();
    ASSERT_NE(dest, 0u);

    constexpr u32 D3DCLEAR_TARGET = 0x1;
    constexpr u32 clear_color = 0xFF00FF00; // opaque green
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_TARGET, clear_color, 0, 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 3,
                     {swap_chain, 0, 0, 0, 0, 0}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 4, {swap_chain, dest}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    EXPECT_EQ(memory_.read_u32(bits).value(), clear_color);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}
