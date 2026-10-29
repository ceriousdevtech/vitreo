/// @file test_d3d9_render_target.cpp
/// @brief Render-target draws, cube-face targets and fp16 targets.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest,
       DrawWithExplicitDepthStencilWorksWithoutAutoDepthStencil) {
    // RCT3 creates the device without EnableAutoDepthStencil, then binds a
    // depth surface and enables Z. The swap-chain pass still needs a depth
    // attachment or Metal rejects the draw.
    const auto [factory, device] = create_device(false);
    EXPECT_TRUE(backend_.record.last_swap_chain_desc.depth_stencil);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 29,
                     {device, 640, 480, 75 /* D3DFMT_D24S8 */, 0, 0, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 depth = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(depth, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 39, {device, depth}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 7 /* D3DRS_ZENABLE */, 1}),
              D3D_OK);

    const u32 vertex_buffer = bind_triangle_stream(device);
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3D_OK);
    ASSERT_EQ(backend_.record.draws, 1u);
    EXPECT_TRUE(backend_.record.draw_calls.front().depth_test_enabled);
    EXPECT_TRUE(backend_.record.last_pipeline_desc.depth_attachment);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, DrawToTextureRenderTargetUsesStandalonePass) {
    // RCT3 CreateTexture(D3DUSAGE_RENDERTARGET) + GetSurfaceLevel +
    // SetRenderTarget, then draws into the texture before sampling it.
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 64, 1, 0x1 /* RENDERTARGET */, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);
    EXPECT_TRUE(backend_.record.last_texture_desc.render_target);
    EXPECT_FALSE(backend_.record.last_texture_desc.cpu_writable);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(surface, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);

    const u32 vertex_buffer = bind_triangle_stream(device);
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.standalone_passes, 1u);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_FALSE(backend_.record.draw_calls.front().depth_test_enabled);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, DrawToCubeFaceRenderTargetUsesFaceSlice) {
    // GetCubeMapSurface(Face, 0) + SetRenderTarget + Draw must open the
    // standalone pass on that face, not slice 0.
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 1, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 cube = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(cube, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {cube, 5, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(surface, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, surface}),
              D3D_OK);

    const u32 vertex_buffer = bind_triangle_stream(device);
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.standalone_passes, 1u);
    EXPECT_EQ(backend_.record.last_standalone_color_slice, 5u);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, ColorFillCubeFaceUsesFaceSlice) {
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 1, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 cube = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(cube, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {cube, 5, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(surface, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35,
                     {device, surface, 0, 0x80AABBCC}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_render_target_clears, 1u);
    EXPECT_EQ(backend_.record.last_clear_slice, 5u);
}

TEST_F(D3D9ManagerTest, StretchRectCubeFacesUsesFaceSlices) {
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 1, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src_cube = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(src_cube, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 1, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst_cube = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dst_cube, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {src_cube, 2, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {dst_cube, 5, 0, static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_NE(src, 0u);
    ASSERT_NE(dst, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 34,
                     {device, src, 0, dst, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_region_copies, 1u);
    EXPECT_EQ(backend_.record.last_region_src_slice, 2u);
    EXPECT_EQ(backend_.record.last_region_dst_slice, 5u);
}

TEST_F(D3D9ManagerTest, GetRenderTargetDataCubeFaceUsesFaceSlice) {
    const auto [factory, device] = create_device(false);
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 4, 1, 1, D3DFMT_A8R8G8B8, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 cube = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(cube, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {cube, 5, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(src, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35,
                     {device, src, 0, 0x80AABBCC}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 8), 0}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 8).value();
    ASSERT_NE(dst, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, src, dst}),
              D3D_OK);
    EXPECT_EQ(backend_.record.last_readback_slice, 5u);
}

TEST_F(D3D9ManagerTest, UpdateTextureCopiesEveryCubeFace) {
    // Learn: UpdateTexture accepts IDirect3DCubeTexture9 and copies all six
    // faces. A Texture-only lookup returns D3DERR_INVALIDCALL and leaves the
    // DEFAULT cube empty (white / missing environment faces).
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_DEFAULT = 0;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(src, 0u);
    ASSERT_NE(dst, 0u);

    for (u32 face = 0; face < 6; ++face) {
        ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 19,
                         {src, face, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                          0, 0}),
                  D3D_OK);
        const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
        ASSERT_NE(bits, 0u);
        std::array<u8, 64> pixels{};
        pixels.fill(static_cast<u8>(0x10u + face));
        ASSERT_TRUE(
            memory_.write_bytes(bits, pixels.data(), pixels.size()).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 20, {src, face, 0}),
                  D3D_OK);
    }

    const unsigned updates_before = backend_.record.texture_updates;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 31, {device, src, dst}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_updates, updates_before + 6u);
    EXPECT_EQ(backend_.record.last_texture_update_slice, 5u);
    ASSERT_FALSE(backend_.record.last_texture_update.empty());
    EXPECT_EQ(backend_.record.last_texture_update[0], 0x15);
}

TEST_F(D3D9ManagerTest, StretchRectScaledCubeFacesUsesFaceSlices) {
    // Same-size StretchRect is a blit. The scaled path must still name the
    // source and dest cube faces; omitting them samples/writes slice 0.
    const auto [factory, device] = create_device(false);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 32, 1, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src_cube = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 1, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst_cube = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(src_cube, 0u);
    ASSERT_NE(dst_cube, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {src_cube, 2, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {dst_cube, 5, 0, static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_NE(src, 0u);
    ASSERT_NE(dst, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 34,
                     {device, src, 0, dst, 0, 2 /* D3DTEXF_LINEAR */}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_stretches, 1u);
    EXPECT_EQ(backend_.record.last_stretch_src_slice, 2u);
    EXPECT_EQ(backend_.record.last_stretch_dst_slice, 5u);
    EXPECT_EQ(backend_.record.last_stretch_src_width, 32u);
    EXPECT_EQ(backend_.record.last_stretch_dst_width, 64u);
    EXPECT_EQ(backend_.record.last_stretch_filter,
              graphics::BackendFilter::Linear);
}

TEST_F(D3D9ManagerTest, Fp16RenderTargetClearsAndSamples) {
    // D3DFMT_A16B16G16R16F render targets (water / reflection) must create as
    // RGBA16Float, accept a GPU clear, and bind for a later sample.
    const auto [factory, device] = create_device(false);
    constexpr u32 D3DFMT_A16B16G16R16F = 113;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 64, 64, 1, 0x1 /* RENDERTARGET */,
                      D3DFMT_A16B16G16R16F, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(backend_.record.last_texture_desc.pixel_format,
              graphics::PixelFormat::RGBA16Float);
    EXPECT_TRUE(backend_.record.last_texture_desc.render_target);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(surface, 0u);
    const unsigned clears_before = backend_.record.texture_render_target_clears;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35,
                     {device, surface, 0, 0xFF00FF00}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_render_target_clears,
              clears_before + 1u);
    EXPECT_NEAR(backend_.record.last_clear[1], 1.0f, 1e-5f);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, texture}),
              D3D_OK);
    const u32 vertex_buffer = bind_triangle_stream(device);
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}
