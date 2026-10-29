/// @file test_d3d9_texture_lock.cpp
/// @brief Texture and surface locking.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <algorithm>
#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, TextureLevelsLockAndUploadThroughTheSharedBackend) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);
    EXPECT_FALSE(backend_.record.last_texture_desc.cpu_writable);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 13, {texture}), 1u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      0}),
              D3D_OK);
    const u32 pitch = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    EXPECT_EQ(pitch, 16u); // 4 pixels * 4 bytes
    ASSERT_NE(bits, 0u);

    std::array<u8, 64> pixels{};
    for (usize i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<u8>(i);
    }
    ASSERT_TRUE(memory_.write_bytes(bits, pixels.data(),
                                    pixels.size()).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);

    ASSERT_EQ(backend_.record.texture_updates, 1u);
    EXPECT_EQ(backend_.record.last_texture_update_level, 0u);
    ASSERT_EQ(backend_.record.last_texture_update.size(), pixels.size());
    EXPECT_TRUE(std::equal(pixels.begin(), pixels.end(),
                           backend_.record.last_texture_update.begin()));
}

TEST_F(D3D9ManagerTest, DynamicTextureRequestsCpuWritableStorage) {
    // Learn: D3DUSAGE_DYNAMIC (0x200) DEFAULT textures are CPU-lockable;
    // Metal keeps Shared storage so LockRect can replaceRegion.
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0x200u /* DYNAMIC */, 21 /* A8R8G8B8 */,
                      0 /* DEFAULT */, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);
    EXPECT_TRUE(backend_.record.last_texture_desc.cpu_writable);

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      0}),
              D3D_OK);
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_NE(bits, 0u);
    std::array<u8, 64> pixels{};
    pixels.fill(0x7F);
    ASSERT_TRUE(memory_.write_bytes(bits, pixels.data(),
                                    pixels.size()).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);
    ASSERT_EQ(backend_.record.texture_updates, 1u);
}

TEST_F(D3D9ManagerTest, TextureLockRectUsesX64PointerOffset) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 2, 2, 1, 0, 21, D3DPOOL_MANAGED,
                      static_cast<u32>(SCRATCH_ADDR), 0}), D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    constexpr GuestAddr output = 0x140000000;
    ASSERT_TRUE(memory_.map_region(output, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory_.write_u64(output + 8, UINT64_MAX).is_ok());
    cpu_.init_long_mode();
    cpu_.gpr.rsp = STACK_BASE + 0x800;
    cpu_.gpr.rcx = texture;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = output;
    cpu_.gpr.r9 = 0;
    ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp + 40, 0).is_ok());
    mgr_.impl_method(marshaler_, D3D9InterfaceId::Texture, 19);
    ASSERT_EQ(cpu_.gpr.rax, D3D_OK);
    EXPECT_GE(memory_.read_u32(output).value(), 8u);
    const GuestAddr bits = memory_.read_u64(output + 8).value();
    EXPECT_NE(bits, 0u);
    ASSERT_TRUE(memory_.write_u32(bits, 0xFFAABBCC).is_ok());
    EXPECT_EQ(memory_.read_u32(bits).value(), 0xFFAABBCCu);
}

TEST_F(D3D9ManagerTest, TextureAndSurfaceSubrectLocksOffsetBitsAndKeepPitch) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 8, 8, 1, 0, 21 /* A8R8G8B8 */,
                      3 /* SCRATCH */, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);

    const std::array<i32, 4> rect{2, 3, 6, 7};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x200, reinterpret_cast<const Byte*>(rect.data()),
        sizeof(rect)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      static_cast<u32>(SCRATCH_ADDR + 0x200),
                      D3DLOCK_READONLY}),
              D3D_OK);
    const u32 subrect_bits =
        memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 32u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      D3DLOCK_READONLY}),
              D3D_OK);
    const u32 level_bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    EXPECT_EQ(subrect_bits, level_bits + 3u * 32u + 2u * 4u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x300)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR + 0x300).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {surface, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      static_cast<u32>(SCRATCH_ADDR + 0x200),
                      D3DLOCK_READONLY}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 32u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), subrect_bits);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 14, {surface}), D3D_OK);
}

TEST_F(D3D9ManagerTest, DxtSubrectLockUsesBlockRowsAndValidatesAlignment) {
    constexpr u32 D3DFMT_DXT3 = 0x33545844u;
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 16, 16, 1, 0, D3DFMT_DXT3, 3 /* SCRATCH */,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();

    const std::array<i32, 4> rect{4, 8, 12, 16};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x200, reinterpret_cast<const Byte*>(rect.data()),
        sizeof(rect)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      static_cast<u32>(SCRATCH_ADDR + 0x200),
                      D3DLOCK_READONLY}),
              D3D_OK);
    const u32 subrect_bits =
        memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 64u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0,
                      D3DLOCK_READONLY}),
              D3D_OK);
    const u32 level_bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    EXPECT_EQ(subrect_bits, level_bits + 2u * 64u + 16u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 20, {texture, 0}), D3D_OK);

    // A subrectangle cannot be combined with DISCARD.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      static_cast<u32>(SCRATCH_ADDR + 0x200),
                      D3DLOCK_DISCARD}),
              D3DERR_INVALIDCALL);

    const std::array<i32, 4> unaligned{2, 0, 8, 8};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x200,
        reinterpret_cast<const Byte*>(unaligned.data()),
        sizeof(unaligned)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 19,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      static_cast<u32>(SCRATCH_ADDR + 0x200),
                      D3DLOCK_READONLY}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, ReleasedComObjectAddressesAreRecycled) {
    // RCT3 creates thousands of Texture+Surface objects while loading OVLs.
    // Without recycling, the fixed COM region exhausts even after Release.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 first = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(first, 0u);

    // Release the texture (and its level surfaces) down to zero.
    u32 refs = 0;
    do {
        refs = invoke(D3D9InterfaceId::Texture, 2, {first});
    } while (refs > 0);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 second = memory_.read_u32(SCRATCH_ADDR).value();
    // LIFO free-list: the texture slot itself is pushed last on destroy.
    EXPECT_EQ(second, first);
}

TEST_F(D3D9ManagerTest, GetSurfaceLevelReturnsAStableInterfacePointer) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 8, 8, 2, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();

    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 first = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 0, static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), first);

    // Level 1 is a different, half-sized surface.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 1, static_cast<u32>(SCRATCH_ADDR + 0x108)}),
              D3D_OK);
    const u32 second = memory_.read_u32(SCRATCH_ADDR + 0x108).value();
    EXPECT_NE(second, first);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 12,
                     {second, static_cast<u32>(SCRATCH_ADDR + 0x200)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x200 + 24).value(), 4u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x200 + 28).value(), 4u);

    // A level past the mip chain does not exist.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                     {texture, 2, static_cast<u32>(SCRATCH_ADDR + 0x10C)}),
              D3DERR_INVALIDCALL);
}
