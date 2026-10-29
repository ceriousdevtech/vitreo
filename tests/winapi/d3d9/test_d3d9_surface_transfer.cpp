/// @file test_d3d9_surface_transfer.cpp
/// @brief Shader integer/bool constants and surface transfers.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

// =============================================================================
// Shader integer/bool constants + surface transfers
// =============================================================================

TEST_F(D3D9ManagerTest, VertexShaderConstantIRoundTrips) {
    // Learn: Set/GetVertexShaderConstantI store four ints per register.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    const std::array<i32, 4> values{7, -3, 0x12345678, 42};
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR,
                                 reinterpret_cast<const Byte*>(values.data()),
                                 sizeof(values))
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 96,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR), 1}),
              D3D_OK);

    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 0x20, 0).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 97,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x20), 1}),
              D3D_OK);
    for (usize i = 0; i < values.size(); ++i) {
        i32 got = 0;
        ASSERT_TRUE(memory_
                        .read_bytes(SCRATCH_ADDR + 0x20 + i * 4,
                                    reinterpret_cast<Byte*>(&got), sizeof(got))
                        .is_ok());
        EXPECT_EQ(got, values[i]) << "lane " << i;
    }

    // Out-of-range register bank is INVALIDCALL.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 96,
                     {device, 16, static_cast<u32>(SCRATCH_ADDR), 1}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, VertexShaderConstantBRoundTrips) {
    // Learn: Set/GetVertexShaderConstantB — BOOL is a 32-bit Windows value.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 1).is_ok());
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 4, 0).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 98,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR), 2}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 99,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x40), 2}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x40).value(), 1u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x44).value(), 0u);
}

TEST_F(D3D9ManagerTest, UpdateSurfaceCopiesSystemMemToDefault) {
    // Learn: UpdateSurface — SYSTEMMEM → DEFAULT, same format, no stretch.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(src, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 4, 4, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dst, 0u);

    // LockRect(pLockedRect, pRect, Flags) → slot 13 on Surface.
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {src, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 pitch = memory_.read_u32(SCRATCH_ADDR + 0x20).value();
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    ASSERT_NE(bits, 0u);
    ASSERT_GE(pitch, 16u);
    const u32 pixel = 0xFF224466u; // AARRGGBB stored as BGRA bytes
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            ASSERT_TRUE(memory_
                            .write_u32(bits + y * pitch + x * 4, pixel)
                            .is_ok());
        }
    }
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {src}), D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 30, {device, src, 0, dst, 0}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dst, static_cast<u32>(SCRATCH_ADDR + 0x30), 0, 0}),
              D3D_OK);
    const u32 dst_pitch = memory_.read_u32(SCRATCH_ADDR + 0x30).value();
    const u32 dst_bits = memory_.read_u32(SCRATCH_ADDR + 0x34).value();
    EXPECT_EQ(memory_.read_u32(dst_bits).value(), pixel);
    EXPECT_EQ(memory_.read_u32(dst_bits + dst_pitch + 4).value(), pixel);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dst}), D3D_OK);
}

TEST_F(D3D9ManagerTest, UpdateSurfaceCopiesCompressedBlockRectangles) {
    const auto [factory, device] = create_device();
    for (u32 format : {0x31545844u, 0x35545844u}) {
        const u32 block_bytes = format == 0x31545844u ? 8u : 16u;
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 36,
                         {device, 16, 16, format, D3DPOOL_SYSTEMMEM,
                          static_cast<u32>(SCRATCH_ADDR), 0}), D3D_OK);
        const u32 src = memory_.read_u32(SCRATCH_ADDR).value();
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                         {device, 16, 16, 1, 0, format, D3DPOOL_DEFAULT,
                          static_cast<u32>(SCRATCH_ADDR + 4), 0}), D3D_OK);
        const u32 texture = memory_.read_u32(SCRATCH_ADDR + 4).value();
        ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                         {texture, 0, static_cast<u32>(SCRATCH_ADDR + 8)}), D3D_OK);
        const u32 dst = memory_.read_u32(SCRATCH_ADDR + 8).value();
        ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                         {src, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}), D3D_OK);
        const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
        std::vector<u8> pixels(16u * block_bytes);
        for (usize k = 0; k < pixels.size(); ++k) pixels[k] = static_cast<u8>(k + 1);
        ASSERT_TRUE(memory_.write_bytes(bits, pixels.data(), pixels.size()).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 14, {src}), D3D_OK);
        const std::array<i32, 4> rect{4, 4, 12, 12};
        const std::array<i32, 2> point{4, 0};
        ASSERT_TRUE(memory_.write_bytes(SCRATCH_ADDR + 0x40,
                    reinterpret_cast<const Byte*>(rect.data()), sizeof(rect)).is_ok());
        ASSERT_TRUE(memory_.write_bytes(SCRATCH_ADDR + 0x50,
                    reinterpret_cast<const Byte*>(point.data()), sizeof(point)).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 30,
                         {device, src, static_cast<u32>(SCRATCH_ADDR + 0x40), dst,
                          static_cast<u32>(SCRATCH_ADDR + 0x50)}), D3D_OK);
        const auto& uploaded = backend_.record.last_texture_update;
        ASSERT_EQ(uploaded.size(), pixels.size());
        for (u32 y = 0; y < 2; ++y)
            for (u32 x = 0; x < 2 * block_bytes; ++x)
                EXPECT_EQ(uploaded[y * 4 * block_bytes + block_bytes + x],
                          pixels[(y + 1) * 4 * block_bytes + block_bytes + x]);
        EXPECT_EQ(uploaded[0], 0u);
        const auto before = uploaded;
        ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 0x40, 1).is_ok());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 30,
                         {device, src, static_cast<u32>(SCRATCH_ADDR + 0x40), dst,
                          static_cast<u32>(SCRATCH_ADDR + 0x50)}), D3DERR_INVALIDCALL);
        EXPECT_EQ(backend_.record.last_texture_update, before);
    }
}

TEST_F(D3D9ManagerTest, ColorFillWritesDefaultOffscreenSurface) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 2, 2, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(surface, 0u);

    constexpr u32 fill = 0x80AABBCC;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, surface, 0, fill}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {surface, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    // A8R8G8B8 lock memory is BGRA bytes; the D3DCOLOR dword layout matches.
    EXPECT_EQ(memory_.read_u32(bits).value(), fill);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {surface}), D3D_OK);
}

TEST_F(D3D9ManagerTest, ColorFillWritesR5G6B5OffscreenSurface) {
    // Learn: ColorFill supports D3DFMT_R5G6B5 (REF/SW list). Previously the
    // CPU path only packed A8R8G8B8/X8R8G8B8 and returned E_NOTIMPL here.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_R5G6B5 = 23;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 2, 2, D3DFMT_R5G6B5, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(surface, 0u);

    constexpr u32 fill = 0x80AABBCC; // A8R8G8B8 D3DCOLOR
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, surface, 0, fill}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {surface, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    // R=0xAA→5, G=0xBB→6, B=0xCC→5 → 0xADD9 little-endian.
    constexpr u16 expected = static_cast<u16>(((0xAAu >> 3) << 11) |
                                              ((0xBBu >> 2) << 5) |
                                              (0xCCu >> 3));
    EXPECT_EQ(memory_.read_u16(bits).value(), expected);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {surface}), D3D_OK);
}

TEST_F(D3D9ManagerTest, GetRenderTargetDataReadsCreateRenderTarget) {
    // Learn: GetRenderTargetData — DEFAULT RT → SYSTEMMEM, same size/format.
    // CreateRenderTarget must resolve a colour backend so ColorFill +
    // readback succeed (not E_NOTIMPL / empty guest pixels).
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    // CreateRenderTarget(Width, Height, Format, MultiSample, Quality,
    //                    Lockable, ppSurface, pSharedHandle)
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 4, 4, D3DFMT_A8R8G8B8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 rt = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(rt, 0u);
    EXPECT_TRUE(backend_.record.last_texture_desc.render_target);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dest, 0u);

    constexpr u32 fill = 0xFF4060A0; // A8R8G8B8
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, rt, 0, fill}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, rt, dest}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    // BGRA memory for A8R8G8B8: B=A0 G=60 R=40 A=FF
    EXPECT_EQ(memory_.read_u32(bits).value(), 0xFF4060A0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}

TEST_F(D3D9ManagerTest, GetRenderTargetDataPacksR5G6B5FromBackend) {
    // Backend readback is always BGRA8; GetRenderTargetData packs into
    // SYSTEMMEM D3DFMT_R5G6B5 (same format as the RT).
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_R5G6B5 = 23;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 2, 2, D3DFMT_R5G6B5, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 rt = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(rt, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 2, 2, D3DFMT_R5G6B5, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dest, 0u);

    constexpr u32 fill = 0x80AABBCC;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, rt, 0, fill}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, rt, dest}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    constexpr u16 expected = static_cast<u16>(((0xAAu >> 3) << 11) |
                                              ((0xBBu >> 2) << 5) |
                                              (0xCCu >> 3));
    EXPECT_EQ(memory_.read_u16(bits).value(), expected);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}

TEST_F(D3D9ManagerTest, GetRenderTargetDataPacksA1R5G5B5FromBackend) {
    // Backend readback is always BGRA8; GetRenderTargetData packs into
    // SYSTEMMEM D3DFMT_A1R5G5B5 (1-bit alpha from A8 threshold).
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A1R5G5B5 = 25;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 2, 2, D3DFMT_A1R5G5B5, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 rt = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(rt, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 2, 2, D3DFMT_A1R5G5B5, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dest, 0u);

    // A>=0x80 → alpha bit set; RGB each >> 3 into 5-bit channels.
    constexpr u32 fill_opaque = 0x80AABBCC;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, rt, 0, fill_opaque}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, rt, dest}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    constexpr u16 expected_opaque = static_cast<u16>(
        0x8000u | ((0xAAu >> 3) << 10) | ((0xBBu >> 3) << 5) | (0xCCu >> 3));
    EXPECT_EQ(memory_.read_u16(bits).value(), expected_opaque);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);

    // A<0x80 → alpha bit clear.
    constexpr u32 fill_clear = 0x40AABBCC;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, rt, 0, fill_clear}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, rt, dest}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    constexpr u16 expected_clear = static_cast<u16>(
        ((0xAAu >> 3) << 10) | ((0xBBu >> 3) << 5) | (0xCCu >> 3));
    EXPECT_EQ(memory_.read_u16(bits).value(), expected_clear);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}

TEST_F(D3D9ManagerTest, GetRenderTargetDataPacksX1R5G5B5FromBackend) {
    // X1R5G5B5: high bit is don't-care / opaque (written as 1) even when
    // source A8 is zero.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_X1R5G5B5 = 24;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 2, 2, D3DFMT_X1R5G5B5, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 rt = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(rt, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 2, 2, D3DFMT_X1R5G5B5, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dest, 0u);

    constexpr u32 fill = 0x00AABBCC;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, rt, 0, fill}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, rt, dest}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    constexpr u16 expected = static_cast<u16>(
        0x8000u | ((0xAAu >> 3) << 10) | ((0xBBu >> 3) << 5) | (0xCCu >> 3));
    EXPECT_EQ(memory_.read_u16(bits).value(), expected);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}

TEST_F(D3D9ManagerTest, GetRenderTargetDataReadsRtTextureLevel) {
    // Learn: source may be a level of a DEFAULT render-target texture.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_A8R8G8B8 = 21;
    constexpr u32 D3DUSAGE_RENDERTARGET = 0x1;
    constexpr u32 D3DPOOL_SYSTEMMEM = 2;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                      D3DPOOL_DEFAULT, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 tex = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(tex, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x10)}),
              D3D_OK);
    const u32 rt = memory_.read_u32(SCRATCH_ADDR + 0x10).value();
    ASSERT_NE(rt, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 4, 4, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 dest = memory_.read_u32(SCRATCH_ADDR + 4).value();
    ASSERT_NE(dest, 0u);

    constexpr u32 fill = 0xFF112233;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 35, {device, rt, 0, fill}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 32, {device, rt, dest}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {dest, static_cast<u32>(SCRATCH_ADDR + 0x20), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x24).value();
    EXPECT_EQ(memory_.read_u32(bits).value(), 0xFF112233u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {dest}), D3D_OK);
}

TEST_F(D3D9ManagerTest, SurfaceGetDCReturnsNotImplWithoutFakeHdc) {
    // Learn: IDirect3DSurface9::GetDC is valid for offscreen plain surfaces in
    // formats without alpha (e.g. X8R8G8B8). Vitreo has no GDI↔D3D9 surface
    // DC path yet, so GetDC/ReleaseDC stay E_NOTIMPL and must not invent an HDC.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr u32 D3DFMT_X8R8G8B8 = 22;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 16, 16, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(surface, 0u);

    constexpr u32 hdc_out = static_cast<u32>(SCRATCH_ADDR + 0x20);
    ASSERT_TRUE(memory_.write_u32(hdc_out, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 15, {surface, hdc_out}),
              E_NOTIMPL_D3D);
    EXPECT_EQ(memory_.read_u32(hdc_out).value(), 0xDEADBEEFu);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 16, {surface, 0}),
              E_NOTIMPL_D3D);
}

TEST_F(D3D9ManagerTest, StaleReleaseKeepsIUnknownVtable) {
    // IUnknown::Release is `mov ecx,[this]; call [ecx+8]`. Zeroing the object
    // header on destroy made a stale Release GP at address 8.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);
    const u32 vtable = memory_.read_u32(texture).value();
    ASSERT_NE(vtable, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture}), 0u);
    EXPECT_EQ(memory_.read_u32(texture).value(), vtable);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture}), 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture}), 0u);
}
