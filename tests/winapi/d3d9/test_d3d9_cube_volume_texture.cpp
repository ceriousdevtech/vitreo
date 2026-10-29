/// @file test_d3d9_cube_volume_texture.cpp
/// @brief Cube and volume textures.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, CreateCubeTextureAndGetCubeMapSurface) {
    // Learn: CreateCubeTexture + GetCubeMapSurface return a stable face
    // surface; SetTexture accepts the cube object.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 8, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 cube = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(cube, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::CubeTexture, 13, {cube}), 1u);

    EXPECT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {cube, 0 /* +X */, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 first = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    EXPECT_NE(first, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {cube, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), first);

    EXPECT_EQ(invoke(D3D9InterfaceId::CubeTexture, 18,
                     {cube, 1 /* -X */, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x108)}),
              D3D_OK);
    const u32 other_face = memory_.read_u32(SCRATCH_ADDR + 0x108).value();
    EXPECT_NE(other_face, first);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, cube}), D3D_OK);
}

TEST_F(D3D9ManagerTest, CreateCubeTextureReportsSixFacesToBackend) {
    // Learn: CreateCubeTexture is one cube. The shared backend counts faces
    // (array_size=6), not "one cube" (array_size=1).
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 8, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    EXPECT_TRUE(backend_.record.last_texture_desc.cube);
    EXPECT_EQ(backend_.record.last_texture_desc.array_size, 6u);
    EXPECT_FALSE(backend_.record.last_texture_desc.render_target);
}

TEST_F(D3D9ManagerTest, CreateCubeTextureRenderTargetEdge64) {
    // Learn: Usage=RENDERTARGET, Pool=DEFAULT, edge 64, full mip chain.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 0, 1, 21, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    EXPECT_NE(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
    EXPECT_TRUE(backend_.record.last_texture_desc.cube);
    EXPECT_TRUE(backend_.record.last_texture_desc.render_target);
    EXPECT_EQ(backend_.record.last_texture_desc.array_size, 6u);
    EXPECT_EQ(backend_.record.last_texture_desc.width, 64u);
}

TEST_F(D3D9ManagerTest, CreateCubeTextureL6V5U5IsNotAvailable) {
    // D3DFMT_L6V5U5 is the D3D9 enum 61. That is not an engine-internal
    // format-table slot; those slots store a D3DFORMAT, often A8R8G8B8.
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 25,
                     {device, 64, 1, 0, 61, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3DERR_NOTAVAILABLE);
}

TEST_F(D3D9ManagerTest, CreateVolumeTextureSystemMemLockBoxAndGetVolumeLevel) {
    // Learn: CreateVolumeTexture + GetVolumeLevel + LockBox for CPU pools.
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 24,
                     {device, 4, 4, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 volume_tex = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(volume_tex, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 13, {volume_tex}), 1u);
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 10, {volume_tex}),
              4u /* D3DRTYPE_VOLUMETEXTURE */);

    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 17,
                     {volume_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x40).value(), D3DFMT_A8R8G8B8);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x44).value(),
              2u /* D3DRTYPE_VOLUME */);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x50).value(), 4u); // Width
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x54).value(), 4u); // Height
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x58).value(), 2u); // Depth

    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 18,
                     {volume_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 level0 = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    EXPECT_NE(level0, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 18,
                     {volume_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), level0);

    // LockBox via the volume texture (Level, pLockedVolume, pBox, Flags).
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 19,
                     {volume_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x200), 0,
                      0}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x200).value(), 16u); // RowPitch
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x204).value(), 64u); // SlicePitch
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x208).value();
    EXPECT_NE(bits, 0u);
    ASSERT_TRUE(memory_.write_u32(bits, 0xAABBCCDDu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 20, {volume_tex, 0}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(bits).value(), 0xAABBCCDDu);

    // LockBox via IDirect3DVolume9.
    EXPECT_EQ(invoke(D3D9InterfaceId::Volume, 9,
                     {level0, static_cast<u32>(SCRATCH_ADDR + 0x220), 0, 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Volume, 10, {level0}), D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, volume_tex}),
              D3D_OK);
}

TEST_F(D3D9ManagerTest, CreateVolumeTextureAutoMipChain) {
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_MANAGED = 1;
    // Levels=0 → full chain for 8x4x2: max(8,4,2)=8 → 4 levels
    // (8→4→2→1).
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 24,
                     {device, 8, 4, 2, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 volume_tex = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(volume_tex, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 13, {volume_tex}), 4u);

    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 17,
                     {volume_tex, 3, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x50).value(), 1u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x54).value(), 1u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x58).value(), 1u);
}

TEST_F(D3D9ManagerTest, VolumeTextureDefaultGenerateMipSubLevels) {
    // Learn: IDirect3DBaseTexture9::GenerateMipSubLevels on a DEFAULT volume
    // with BackendTexture3D must call backend generate_mips; AUTOGENMIPMAP
    // UnlockBox of level 0 also regenerates the chain.
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_DEFAULT = 0;
    constexpr u32 D3DUSAGE_AUTOGENMIPMAP = 0x00000400;

    // 8x4x2 → 4 mip levels; AUTOGENMIPMAP marks dirty level-0 for regen.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 24,
                     {device, 8, 4, 2, 0, D3DUSAGE_AUTOGENMIPMAP,
                      D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 volume_tex = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(volume_tex, 0u);
    EXPECT_EQ(backend_.record.texture_3d_creates, 1u);
    EXPECT_EQ(backend_.record.last_texture_3d_desc.mip_levels, 4u);
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 13, {volume_tex}), 4u);

    // Explicit GenerateMipSubLevels → backend generate_mips(BackendTexture3D).
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 16, {volume_tex}), 0u);
    EXPECT_EQ(backend_.record.texture_3d_mip_generations, 1u);

    // UnlockBox level 0 with AUTOGENMIPMAP regenerates again.
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 19,
                     {volume_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x200), 0,
                      0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 20, {volume_tex, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_3d_updates, 1u);
    EXPECT_EQ(backend_.record.texture_3d_mip_generations, 2u);
}

TEST_F(D3D9ManagerTest, AutoGenFilterTypeValidationAndPointMips) {
    // Learn: SetAutoGenFilterType rejects D3DTEXF_NONE / unsupported filters
    // with D3DERR_INVALIDCALL; default is LINEAR; POINT uses CPU nearest
    // downsample (not backend generate_mips / linear blit).
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_DEFAULT = 0;
    constexpr u32 D3DUSAGE_AUTOGENMIPMAP = 0x00000400;
    constexpr u32 D3DTEXF_NONE = 0;
    constexpr u32 D3DTEXF_POINT = 1;
    constexpr u32 D3DTEXF_LINEAR = 2;
    constexpr u32 D3DTEXF_ANISOTROPIC = 3;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8,
                      D3DPOOL_DEFAULT, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 tex = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(tex, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 13, {tex}), 3u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 15, {tex}), D3DTEXF_LINEAR);

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 14, {tex, D3DTEXF_NONE}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 14, {tex, D3DTEXF_ANISOTROPIC}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 15, {tex}), D3DTEXF_LINEAR);

    // Without AUTOGENMIPMAP: no effect, no failure (Learn).
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 plain = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(plain, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 14, {plain, D3DTEXF_POINT}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 15, {plain}), D3DTEXF_LINEAR);

    // Fill level 0 with a checker: top-left 0xAARRGGBB distinct from others.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x200), 0, 0}),
              D3D_OK);
    // D3DLOCKED_RECT (32-bit): Pitch @0, pBits @4.
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x204).value();
    const u32 pitch = memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_NE(bits, 0u);
    ASSERT_GE(pitch, 16u);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 color = (x == 0 && y == 0) ? 0xFF112233u : 0xFFCCDDEEu;
            ASSERT_TRUE(
                memory_.write_u32(bits + y * pitch + x * 4u, color).is_ok());
        }
    }
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 20, {tex, 0}), D3D_OK);
    // Unlock with default LINEAR → backend generate_mips.
    EXPECT_EQ(backend_.record.texture_2d_mip_generations, 1u);

    const unsigned updates_before_point = backend_.record.texture_updates;
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 14, {tex, D3DTEXF_POINT}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 15, {tex}), D3DTEXF_POINT);
    // Set dirties/regenerates via CPU POINT path — no extra linear blit.
    EXPECT_EQ(backend_.record.texture_2d_mip_generations, 1u);
    EXPECT_GT(backend_.record.texture_updates, updates_before_point);

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {tex, 1, static_cast<u32>(SCRATCH_ADDR + 0x300), 0, 0}),
              D3D_OK);
    const u32 mip_bits = memory_.read_u32(SCRATCH_ADDR + 0x304).value();
    const u32 mip_pitch = memory_.read_u32(SCRATCH_ADDR + 0x300).value();
    ASSERT_NE(mip_bits, 0u);
    // Nearest (top-left of each 2×2): level-1 (0,0) matches level-0 (0,0).
    EXPECT_EQ(memory_.read_u32(mip_bits).value(), 0xFF112233u);
    EXPECT_EQ(memory_.read_u32(mip_bits + 4).value(), 0xFFCCDDEEu);
    EXPECT_EQ(memory_.read_u32(mip_bits + mip_pitch).value(), 0xFFCCDDEEu);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 20, {tex, 1}), D3D_OK);

    // LINEAR regenerates via backend blit (Set dirties + explicit Generate).
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 14, {tex, D3DTEXF_LINEAR}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_2d_mip_generations, 2u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 16, {tex}), 0u);
    EXPECT_EQ(backend_.record.texture_2d_mip_generations, 3u);
}

TEST_F(D3D9ManagerTest, CreateVolumeTextureDefaultLockBoxUploadsBackend) {
    // Learn: D3DPOOL_DEFAULT CreateVolumeTexture allocates video-memory
    // storage; LockBox/UnlockBox must upload into BackendTexture3D.
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_DEFAULT = 0;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 24,
                     {device, 4, 4, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 volume_tex = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(volume_tex, 0u);
    EXPECT_EQ(backend_.record.texture_3d_creates, 1u);
    EXPECT_EQ(backend_.record.last_texture_3d_desc.width, 4u);
    EXPECT_EQ(backend_.record.last_texture_3d_desc.height, 4u);
    EXPECT_EQ(backend_.record.last_texture_3d_desc.depth, 2u);
    EXPECT_EQ(backend_.record.last_texture_3d_desc.mip_levels, 1u);
    EXPECT_FALSE(backend_.record.last_texture_3d_desc.cpu_writable);
    EXPECT_TRUE(backend_.record.last_texture_3d_desc.shader_resource);

    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 19,
                     {volume_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x200), 0,
                      0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x208).value();
    ASSERT_NE(bits, 0u);
    ASSERT_TRUE(memory_.write_u32(bits, 0x11223344u).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VolumeTexture, 20, {volume_tex, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.texture_3d_updates, 1u);
    EXPECT_EQ(backend_.record.last_texture_3d_update_level, 0u);
    EXPECT_EQ(backend_.record.last_texture_3d_update_row_pitch, 16u);
    EXPECT_EQ(backend_.record.last_texture_3d_update_slice_pitch, 64u);
    ASSERT_GE(backend_.record.last_texture_3d_update.size(), 4u);
    EXPECT_EQ(backend_.record.last_texture_3d_update[0], 0x44u);
    EXPECT_EQ(backend_.record.last_texture_3d_update[1], 0x33u);
    EXPECT_EQ(backend_.record.last_texture_3d_update[2], 0x22u);
    EXPECT_EQ(backend_.record.last_texture_3d_update[3], 0x11u);
}

TEST_F(D3D9ManagerTest, VolumeTextureDefaultDrawBindsBackend3D) {
    // Learn: SetTexture(IDirect3DVolumeTexture9) + Draw must bind
    // BackendTexture3D — not the opaque-white missing-backing fallback.
    const auto [factory, device] = create_device();
    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_DEFAULT = 0;
    (void)bind_triangle_stream(device, 0x002 | 0x040 | 0x100);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 24,
                     {device, 4, 4, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR + 0x500), 0}),
              D3D_OK);
    const u32 volume_tex = memory_.read_u32(SCRATCH_ADDR + 0x500).value();
    ASSERT_NE(volume_tex, 0u);
    ASSERT_EQ(backend_.record.texture_3d_creates, 1u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, volume_tex}),
              D3D_OK);
    // D3DSAMP_ADDRESSW = D3DTADDRESS_CLAMP — volume sampling uses W.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69, {device, 0, 3, 3}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_NE(call.textures_3d[0], nullptr);
    EXPECT_TRUE(call.textures_3d[0]->valid());
    EXPECT_EQ(call.textures[0], nullptr);
    EXPECT_EQ(call.samplers[0].address_w, graphics::BackendAddressMode::Clamp);
}
