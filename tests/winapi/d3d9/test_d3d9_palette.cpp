/// @file test_d3d9_palette.cpp
/// @brief Palettized textures.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <algorithm>
#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, PaletteEntriesDefaultAndRoundTrip) {
    // Learn: Texture Palettes — 256 PALETTEENTRY default 0xFFFFFFFF; Set/Get
    // round-trip. With ALPHAPALETTE, peFlags may be any alpha value.
    const auto [factory, device] = create_device();
    constexpr GuestAddr kEntries = SCRATCH_ADDR;
    constexpr GuestAddr kReadback = SCRATCH_ADDR + 0x500;
    constexpr usize kPaletteBytes = 256 * 4;

    std::array<u8, kPaletteBytes> defaults{};
    ASSERT_TRUE(memory_
                    .write_bytes(kReadback, defaults.data(), defaults.size())
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 72,
                     {device, 0, static_cast<u32>(kReadback)}),
              D3D_OK);
    ASSERT_TRUE(memory_
                    .read_bytes(kReadback, defaults.data(), defaults.size())
                    .is_ok());
    for (u8 byte : defaults) {
        EXPECT_EQ(byte, 0xFFu);
    }

    std::array<u8, kPaletteBytes> written{};
    for (usize i = 0; i < 256; ++i) {
        written[i * 4 + 0] = static_cast<u8>(i);
        written[i * 4 + 1] = static_cast<u8>(255 - i);
        written[i * 4 + 2] = static_cast<u8>((i * 3) & 0xFF);
        written[i * 4 + 3] = static_cast<u8>((i * 5) & 0xFF); // peFlags alpha
    }
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, written.data(), written.size())
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 3, static_cast<u32>(kEntries)}),
              D3D_OK);

    std::array<u8, kPaletteBytes> readback{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 72,
                     {device, 3, static_cast<u32>(kReadback)}),
              D3D_OK);
    ASSERT_TRUE(memory_
                    .read_bytes(kReadback, readback.data(), readback.size())
                    .is_ok());
    EXPECT_EQ(readback, written);

    // Palette 0 was never set — still the documented default.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 72,
                     {device, 0, static_cast<u32>(kReadback)}),
              D3D_OK);
    ASSERT_TRUE(memory_
                    .read_bytes(kReadback, readback.data(), readback.size())
                    .is_ok());
    for (u8 byte : readback) {
        EXPECT_EQ(byte, 0xFFu);
    }
}

TEST_F(D3D9ManagerTest, PaletteEntriesAcceptNonOpaqueWithAlphaPalette) {
    // Learn: with D3DPTEXTURECAPS_ALPHAPALETTE, SetPaletteEntries allows any
    // peFlags; P8 expand uses peFlags as alpha.
    const auto [factory, device] = create_device();
    constexpr GuestAddr kEntries = SCRATCH_ADDR;
    std::array<u8, 256 * 4> written{};
    written.fill(0xFF);
    written[0] = 0x11; // R
    written[1] = 0x22; // G
    written[2] = 0x33; // B
    written[3] = 0x80; // peFlags/alpha on entry 0
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, written.data(), written.size())
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 0, static_cast<u32>(kEntries)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 0}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 1, 1, 1, 0, D3DFMT_P8, D3DPOOL_MANAGED,
                      static_cast<u32>(SCRATCH_ADDR + 0x800), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x800).value();
    ASSERT_NE(texture, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_NE(bits, 0u);
    ASSERT_TRUE(memory_.write_u8(bits, 0).is_ok()); // index 0
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);
    ASSERT_GE(backend_.record.last_texture_update.size(), 4u);
    // BGRA8: B,G,R,A — alpha from peFlags.
    EXPECT_EQ(backend_.record.last_texture_update[0], 0x33);
    EXPECT_EQ(backend_.record.last_texture_update[1], 0x22);
    EXPECT_EQ(backend_.record.last_texture_update[2], 0x11);
    EXPECT_EQ(backend_.record.last_texture_update[3], 0x80);
}

TEST_F(D3D9ManagerTest, CurrentTexturePaletteRoundTrips) {
    // Learn: SetCurrentTexturePalette / GetCurrentTexturePalette.
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 74,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 7}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 74,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 7u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 0x10000}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 0x10000, static_cast<u32>(SCRATCH_ADDR)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 72,
                     {device, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 74, {device, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, PalettizedP8TextureExpandsThroughCurrentPalette) {
    // Learn: Texture Palettes + D3DFMT_P8 — indices sample through the
    // device's current palette into A8R8G8B8 (BGRA8 backend here).
    const auto [factory, device] = create_device();
    constexpr GuestAddr kEntries = SCRATCH_ADDR;
    constexpr usize kPaletteBytes = 256 * 4;
    std::array<u8, kPaletteBytes> palette{};
    palette.fill(0xFF);
    // PALETTEENTRY: peRed, peGreen, peBlue, peFlags.
    // index 0 → opaque red; index 1 → opaque green; 2 → blue; 3 → white.
    palette[0 * 4 + 0] = 0xFF;
    palette[0 * 4 + 1] = 0x00;
    palette[0 * 4 + 2] = 0x00;
    palette[1 * 4 + 0] = 0x00;
    palette[1 * 4 + 1] = 0xFF;
    palette[1 * 4 + 2] = 0x00;
    palette[2 * 4 + 0] = 0x00;
    palette[2 * 4 + 1] = 0x00;
    palette[2 * 4 + 2] = 0xFF;
    palette[3 * 4 + 0] = 0xFF;
    palette[3 * 4 + 1] = 0xFF;
    palette[3 * 4 + 2] = 0xFF;
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, palette.data(), palette.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 0, static_cast<u32>(kEntries)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 0}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 2, 2, 1, 0, D3DFMT_P8, D3DPOOL_MANAGED,
                      static_cast<u32>(SCRATCH_ADDR + 0x800), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x800).value();
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(backend_.record.last_texture_desc.pixel_format,
              graphics::PixelFormat::BGRA8Unorm);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      0}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 2u); // pitch
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_NE(bits, 0u);
    const std::array<u8, 4> indices{0, 1, 2, 3};
    ASSERT_TRUE(memory_.write_bytes(bits, indices.data(), indices.size()).is_ok());
    const unsigned updates_before = backend_.record.texture_updates;
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);
    ASSERT_EQ(backend_.record.texture_updates, updates_before + 1u);
    // BGRA8: B,G,R,A per texel.
    const std::array<u8, 16> expected{
        0x00, 0x00, 0xFF, 0xFF, // red
        0x00, 0xFF, 0x00, 0xFF, // green
        0xFF, 0x00, 0x00, 0xFF, // blue
        0xFF, 0xFF, 0xFF, 0xFF, // white
    };
    ASSERT_EQ(backend_.record.last_texture_update.size(), expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                           backend_.record.last_texture_update.begin()));

    // Palette animation: bound texture re-expands when current palette
    // entries change (Learn: current palette translates all paletted tex).
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, texture}),
              D3D_OK);
    palette[0 * 4 + 0] = 0x11;
    palette[0 * 4 + 1] = 0x22;
    palette[0 * 4 + 2] = 0x33;
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, palette.data(), palette.size())
                    .is_ok());
    const unsigned updates_bound = backend_.record.texture_updates;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 0, static_cast<u32>(kEntries)}),
              D3D_OK);
    ASSERT_GT(backend_.record.texture_updates, updates_bound);
    ASSERT_GE(backend_.record.last_texture_update.size(), 4u);
    EXPECT_EQ(backend_.record.last_texture_update[0], 0x33); // B
    EXPECT_EQ(backend_.record.last_texture_update[1], 0x22); // G
    EXPECT_EQ(backend_.record.last_texture_update[2], 0x11); // R
    EXPECT_EQ(backend_.record.last_texture_update[3], 0xFF); // A
}

TEST_F(D3D9ManagerTest, PalettizedA8P8TextureUsesPixelAlphaAndPaletteColor) {
    // Learn: D3DFMT_A8P8 — low byte index, high byte alpha; RGB from palette.
    const auto [factory, device] = create_device();
    constexpr GuestAddr kEntries = SCRATCH_ADDR;
    std::array<u8, 256 * 4> palette{};
    palette.fill(0xFF);
    palette[5 * 4 + 0] = 0x10; // R
    palette[5 * 4 + 1] = 0x20; // G
    palette[5 * 4 + 2] = 0x30; // B
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, palette.data(), palette.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 2, static_cast<u32>(kEntries)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 2}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 1, 1, 1, 0, D3DFMT_A8P8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 0x800), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x800).value();
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(backend_.record.last_texture_desc.pixel_format,
              graphics::PixelFormat::BGRA8Unorm);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      0}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 2u);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    const std::array<u8, 2> pixel{5, 0x80}; // index 5, alpha 0x80
    ASSERT_TRUE(memory_.write_bytes(bits, pixel.data(), pixel.size()).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);
    ASSERT_EQ(backend_.record.last_texture_update.size(), 4u);
    EXPECT_EQ(backend_.record.last_texture_update[0], 0x30);
    EXPECT_EQ(backend_.record.last_texture_update[1], 0x20);
    EXPECT_EQ(backend_.record.last_texture_update[2], 0x10);
    EXPECT_EQ(backend_.record.last_texture_update[3], 0x80);
}

TEST_F(D3D9ManagerTest, PalettizedP8UpdateTextureExpandsThroughCurrentPalette) {
    // Learn: UpdateTexture SYSTEMMEM→DEFAULT must expand P8 through the
    // device's current palette into the DEFAULT BGRA8 backend (same as Unlock).
    const auto [factory, device] = create_device();
    constexpr GuestAddr kEntries = SCRATCH_ADDR;
    std::array<u8, 256 * 4> palette{};
    palette.fill(0xFF);
    palette[0 * 4 + 0] = 0xFF; // R
    palette[0 * 4 + 1] = 0x00;
    palette[0 * 4 + 2] = 0x00;
    palette[1 * 4 + 0] = 0x00;
    palette[1 * 4 + 1] = 0xFF; // G
    palette[1 * 4 + 2] = 0x00;
    palette[2 * 4 + 0] = 0x00;
    palette[2 * 4 + 1] = 0x00;
    palette[2 * 4 + 2] = 0xFF; // B
    palette[3 * 4 + 0] = 0xFF;
    palette[3 * 4 + 1] = 0xFF;
    palette[3 * 4 + 2] = 0xFF; // white
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, palette.data(), palette.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 0, static_cast<u32>(kEntries)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 0}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 2, 2, 1, 0, D3DFMT_P8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 0x800), 0}),
              D3D_OK);
    const u32 src = memory_.read_u32(SCRATCH_ADDR + 0x800).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 2, 2, 1, 0, D3DFMT_P8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR + 0x804), 0}),
              D3D_OK);
    const u32 dst = memory_.read_u32(SCRATCH_ADDR + 0x804).value();
    ASSERT_NE(src, 0u);
    ASSERT_NE(dst, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {src, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_NE(bits, 0u);
    const std::array<u8, 4> indices{0, 1, 2, 3};
    ASSERT_TRUE(memory_.write_bytes(bits, indices.data(), indices.size()).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {src, 0}), D3D_OK);

    const unsigned updates_before = backend_.record.texture_updates;
    // UpdateTexture(pSourceTexture, pDestinationTexture) → device slot 31.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 31, {device, src, dst}),
              D3D_OK);
    ASSERT_EQ(backend_.record.texture_updates, updates_before + 1u);
    const std::array<u8, 16> expected{
        0x00, 0x00, 0xFF, 0xFF, // red
        0x00, 0xFF, 0x00, 0xFF, // green
        0xFF, 0x00, 0x00, 0xFF, // blue
        0xFF, 0xFF, 0xFF, 0xFF, // white
    };
    ASSERT_EQ(backend_.record.last_texture_update.size(), expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                           backend_.record.last_texture_update.begin()));

    // Upload stamped the palette serial: SetTexture must not re-expand.
    const unsigned updates_after = backend_.record.texture_updates;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, dst}), D3D_OK);
    EXPECT_EQ(backend_.record.texture_updates, updates_after);
}

TEST_F(D3D9ManagerTest, PalettizedP8UpdateSurfaceExpandsThroughCurrentPalette) {
    // Learn: UpdateSurface SYSTEMMEM→DEFAULT expands P8 via current palette.
    const auto [factory, device] = create_device();
    constexpr GuestAddr kEntries = SCRATCH_ADDR;
    std::array<u8, 256 * 4> palette{};
    palette.fill(0xFF);
    palette[7 * 4 + 0] = 0xAA; // R
    palette[7 * 4 + 1] = 0xBB; // G
    palette[7 * 4 + 2] = 0xCC; // B
    ASSERT_TRUE(memory_
                    .write_bytes(kEntries, palette.data(), palette.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 71,
                     {device, 1, static_cast<u32>(kEntries)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 73, {device, 1}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 1, 1, 1, 0, D3DFMT_P8, D3DPOOL_SYSTEMMEM,
                      static_cast<u32>(SCRATCH_ADDR + 0x800), 0}),
              D3D_OK);
    const u32 src_tex = memory_.read_u32(SCRATCH_ADDR + 0x800).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 1, 1, 1, 0, D3DFMT_P8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR + 0x804), 0}),
              D3D_OK);
    const u32 dst_tex = memory_.read_u32(SCRATCH_ADDR + 0x804).value();
    ASSERT_NE(src_tex, 0u);
    ASSERT_NE(dst_tex, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {src_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x810)}),
              D3D_OK);
    const u32 src_surf = memory_.read_u32(SCRATCH_ADDR + 0x810).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {dst_tex, 0, static_cast<u32>(SCRATCH_ADDR + 0x814)}),
              D3D_OK);
    const u32 dst_surf = memory_.read_u32(SCRATCH_ADDR + 0x814).value();
    ASSERT_NE(src_surf, 0u);
    ASSERT_NE(dst_surf, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {src_surf, static_cast<u32>(SCRATCH_ADDR + 0x100), 0, 0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    const u8 index = 7;
    ASSERT_TRUE(memory_.write_bytes(bits, &index, 1).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 14, {src_surf}), D3D_OK);

    const unsigned updates_before = backend_.record.texture_updates;
    // UpdateSurface(pSourceSurface, pSourceRect, pDestSurface, pDestPoint)
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 30,
                     {device, src_surf, 0, dst_surf, 0}),
              D3D_OK);
    ASSERT_EQ(backend_.record.texture_updates, updates_before + 1u);
    ASSERT_EQ(backend_.record.last_texture_update.size(), 4u);
    EXPECT_EQ(backend_.record.last_texture_update[0], 0xCC); // B
    EXPECT_EQ(backend_.record.last_texture_update[1], 0xBB); // G
    EXPECT_EQ(backend_.record.last_texture_update[2], 0xAA); // R
    EXPECT_EQ(backend_.record.last_texture_update[3], 0xFF); // A
}
