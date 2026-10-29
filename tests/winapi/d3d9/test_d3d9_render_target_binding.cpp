/// @file test_d3d9_render_target_binding.cpp
/// @brief Viewport, SetRenderTarget, MRT and depth binding for render targets.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;
using vitreo::winapi::d3d9_test::test_f32_bits;

TEST_F(D3D9ManagerTest, SetViewportRejectsRegionOutsideBoundRenderTarget) {
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 64, 1, 0x1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);

    // Back-buffer-sized viewport must fail while a 64×64 RT is bound.
    const std::array<u32, 6> viewport{0, 0, 640, 480, 0, test_f32_bits(1.0f)};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x200, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 47,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x200)}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, SetRenderTargetResetsViewportToTheTargetDimensions) {
    const auto [factory, device] = create_device(false);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 back_buffer = memory_.read_u32(SCRATCH_ADDR).value();

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 32, 1, 0x1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x40), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x80)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x80).value();

    const auto set_viewport = [&](const std::array<u32, 6>& viewport) {
        EXPECT_TRUE(memory_.write_bytes(
            SCRATCH_ADDR + 0x100,
            reinterpret_cast<const Byte*>(viewport.data()),
            sizeof(viewport)).is_ok());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 47,
                         {device, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
                  D3D_OK);
    };
    const auto get_viewport = [&] {
        std::array<u32, 6> viewport{};
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 48,
                         {device, static_cast<u32>(SCRATCH_ADDR + 0x180)}),
                  D3D_OK);
        EXPECT_TRUE(memory_.read_bytes(
            SCRATCH_ADDR + 0x180,
            reinterpret_cast<Byte*>(viewport.data()),
            sizeof(viewport)).is_ok());
        return viewport;
    };
    const auto set_scissor = [&](const std::array<i32, 4>& scissor) {
        EXPECT_TRUE(memory_.write_bytes(
            SCRATCH_ADDR + 0x1C0,
            reinterpret_cast<const Byte*>(scissor.data()),
            sizeof(scissor)).is_ok());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 75,
                         {device, static_cast<u32>(SCRATCH_ADDR + 0x1C0)}),
                  D3D_OK);
    };
    const auto get_scissor = [&] {
        std::array<i32, 4> scissor{};
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 76,
                         {device, static_cast<u32>(SCRATCH_ADDR + 0x200)}),
                  D3D_OK);
        EXPECT_TRUE(memory_.read_bytes(
            SCRATCH_ADDR + 0x200,
            reinterpret_cast<Byte*>(scissor.data()),
            sizeof(scissor)).is_ok());
        return scissor;
    };

    set_viewport({7, 9, 32, 24, test_f32_bits(0.25f),
                  test_f32_bits(0.75f)});
    set_scissor({8, 10, 30, 25});
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);
    EXPECT_EQ(get_viewport(),
              (std::array<u32, 6>{0, 0, 64, 32, test_f32_bits(0.0f),
                                  test_f32_bits(1.0f)}));
    EXPECT_EQ(get_scissor(), (std::array<i32, 4>{0, 0, 64, 32}));

    // The contract also applies when the current target is rebound.
    set_viewport({3, 4, 10, 11, test_f32_bits(0.2f),
                  test_f32_bits(0.8f)});
    set_scissor({3, 4, 13, 15});
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);
    EXPECT_EQ(get_viewport(),
              (std::array<u32, 6>{0, 0, 64, 32, test_f32_bits(0.0f),
                                  test_f32_bits(1.0f)}));
    EXPECT_EQ(get_scissor(), (std::array<i32, 4>{0, 0, 64, 32}));

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37,
                     {device, 0, back_buffer}),
              D3D_OK);
    EXPECT_EQ(get_viewport(),
              (std::array<u32, 6>{0, 0, 640, 480, test_f32_bits(0.0f),
                                  test_f32_bits(1.0f)}));
    EXPECT_EQ(get_scissor(), (std::array<i32, 4>{0, 0, 640, 480}));
}

TEST_F(D3D9ManagerTest, AdditionalRenderTargetSlotsRetainRoundTripAndDisable) {
    const auto [factory, device] = create_device(false);
    constexpr GuestAddr surface_out = SCRATCH_ADDR + 0x300;
    constexpr GuestAddr queried_out = SCRATCH_ADDR + 0x340;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 64, 64, 21, 0, 0, 0,
                      static_cast<u32>(surface_out), 0}),
              D3D_OK);
    const u32 surface = memory_.read_u32(surface_out).value();
    ASSERT_NE(surface, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 1, surface}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 38,
                     {device, 1, static_cast<u32>(queried_out)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(queried_out).value(), surface);
    EXPECT_NE(invoke(D3D9InterfaceId::Surface, 2, {surface}), 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 1, 0}), D3D_OK);
    ASSERT_TRUE(memory_.write_u32(queried_out, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 38,
                     {device, 1, static_cast<u32>(queried_out)}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(memory_.read_u32(queried_out).value(), 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, BackBufferPassIncludesSecondaryRenderTargets) {
    const auto [factory, device] = create_device(false);
    const u32 vertex_buffer = bind_triangle_stream(device);
    constexpr GuestAddr surface_out = SCRATCH_ADDR + 0x348;
    (void)factory;
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 640, 480, D3DFMT_X8R8G8B8, 0, 0, 0,
                      static_cast<u32>(surface_out), 0}),
              D3D_OK);
    const u32 surface = memory_.read_u32(surface_out).value();
    ASSERT_NE(surface, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 1, surface}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.last_swapchain_color_target_count, 2u);
}

TEST_F(D3D9ManagerTest, ClearTargetClearsEveryBoundMrtSurface) {
    const auto [factory, device] = create_device(false);
    constexpr GuestAddr rt0_out = SCRATCH_ADDR + 0x350;
    constexpr GuestAddr rt1_out = SCRATCH_ADDR + 0x354;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 64, 64, D3DFMT_X8R8G8B8, 0, 0, 0,
                      static_cast<u32>(rt0_out), 0}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 64, 64, D3DFMT_X8R8G8B8, 0, 0, 0,
                      static_cast<u32>(rt1_out), 0}),
              D3D_OK);
    const u32 rt0 = memory_.read_u32(rt0_out).value();
    const u32 rt1 = memory_.read_u32(rt1_out).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, rt0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 1, rt1}), D3D_OK);
    const unsigned before = backend_.record.texture_render_target_clears;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_TARGET, 0xFF123456u, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_render_target_clears, before + 2);
}

TEST_F(D3D9ManagerTest, ImplicitBackBufferSurvivesBeingUnbound) {
    const auto [factory, device] = create_device(false);
    constexpr GuestAddr back_out = SCRATCH_ADDR + 0x380;
    constexpr GuestAddr rt_out = SCRATCH_ADDR + 0x384;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                      static_cast<u32>(back_out)}),
              D3D_OK);
    const u32 original_back = memory_.read_u32(back_out).value();
    ASSERT_NE(original_back, 0u);
    // Drop the caller's GetBackBuffer reference. Device ownership must still
    // keep the implicit surface alive after slot 0 is rebound.
    EXPECT_NE(invoke(D3D9InterfaceId::Surface, 2, {original_back}), 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 64, 64, D3DFMT_X8R8G8B8, 0, 0, 0,
                      static_cast<u32>(rt_out), 0}),
              D3D_OK);
    const u32 rt = memory_.read_u32(rt_out).value();
    ASSERT_NE(rt, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, rt}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                      static_cast<u32>(back_out)}),
              D3D_OK);
    const u32 rebound_back = memory_.read_u32(back_out).value();
    EXPECT_EQ(rebound_back, original_back);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 37,
                     {device, 0, rebound_back}),
              D3D_OK);
}

TEST_F(D3D9ManagerTest, TextureRenderTargetUsesTheBoundDepthTexture) {
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 64, 1, 2, 80, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}), D3D_OK);
    const u32 depth_texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {depth_texture, 0, static_cast<u32>(SCRATCH_ADDR + 8)}), D3D_OK);
    const u32 depth = memory_.read_u32(SCRATCH_ADDR + 8).value();
    ASSERT_NE(depth, 0u);
    EXPECT_TRUE(backend_.record.last_texture_desc.depth_stencil);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 39, {device, depth}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 7 /* D3DRS_ZENABLE */, 1}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 64, 1, 0x1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x40), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);

    ASSERT_NE(bind_triangle_stream(device), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.standalone_passes, 1u);
    EXPECT_TRUE(backend_.record.last_standalone_had_depth);
    EXPECT_EQ(backend_.record.last_standalone_depth_format, graphics::PixelFormat::D16Unorm);
    EXPECT_TRUE(backend_.record.draw_calls.front().depth_test_enabled);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, TextureRenderTargetGetsScratchDepthWhenDsIsLarger) {
    // Windows allows an 800x600 depth surface with a 64x64 colour RT. Metal
    // needs equal sizes, so Vitreo allocates a scratch depth for the RT.
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 29,
                     {device, 800, 600, 75 /* D3DFMT_D24S8 */, 0, 0, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 depth = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(depth, 0u);
    EXPECT_TRUE(backend_.record.last_texture_desc.depth_stencil);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 39, {device, depth}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 7 /* D3DRS_ZENABLE */, 1}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 64, 1, 0x1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x40), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);

    ASSERT_NE(bind_triangle_stream(device), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.standalone_passes, 1u);
    EXPECT_TRUE(backend_.record.last_standalone_had_depth);
    EXPECT_TRUE(backend_.record.draw_calls.front().depth_test_enabled);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}
