/// @file test_d3d9_private_data.cpp
/// @brief Resource private data.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

// =============================================================================
// Resources
// =============================================================================

TEST_F(D3D9ManagerTest, ResourcePrivateDataRoundTripFreeAndErrors) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);

    constexpr GuestAddr kGuid = SCRATCH_ADDR + 0x100;
    constexpr GuestAddr kValue = SCRATCH_ADDR + 0x120;
    constexpr GuestAddr kSize = SCRATCH_ADDR + 0x140;
    constexpr GuestAddr kOut = SCRATCH_ADDR + 0x160;
    const std::array<u8, 16> guid{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01};
    const std::array<u8, 4> value{1, 3, 5, 7};
    ASSERT_TRUE(memory_.write_bytes(kGuid, guid.data(), guid.size()).is_ok());
    ASSERT_TRUE(memory_.write_bytes(kValue, value.data(), value.size()).is_ok());

    // Missing GUID → NOTFOUND.
    ASSERT_TRUE(memory_.write_u32(kSize, 4).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 5,
                     {buffer, static_cast<u32>(kGuid),
                      static_cast<u32>(kOut), static_cast<u32>(kSize)}),
              D3DERR_NOTFOUND_D3D);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 6,
                     {buffer, static_cast<u32>(kGuid)}),
              D3DERR_NOTFOUND_D3D);

    // Set → undersized Get → MOREDATA with required size → full Get.
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {buffer, static_cast<u32>(kGuid),
                      static_cast<u32>(kValue),
                      static_cast<u32>(value.size()), 0}),
              D3D_OK);
    ASSERT_TRUE(memory_.write_u32(kSize, 0).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 5,
                     {buffer, static_cast<u32>(kGuid), 0,
                      static_cast<u32>(kSize)}),
              D3DERR_MOREDATA);
    EXPECT_EQ(memory_.read_u32(kSize).value(), value.size());
    ASSERT_TRUE(memory_.write_u32(kSize, value.size()).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 5,
                     {buffer, static_cast<u32>(kGuid),
                      static_cast<u32>(kOut), static_cast<u32>(kSize)}),
              D3D_OK);
    std::array<u8, 4> copied{};
    ASSERT_TRUE(
        memory_.read_bytes(kOut, copied.data(), copied.size()).is_ok());
    EXPECT_EQ(copied, value);

    // Free then Get → NOTFOUND.
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 6,
                     {buffer, static_cast<u32>(kGuid)}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 5,
                     {buffer, static_cast<u32>(kGuid),
                      static_cast<u32>(kOut), static_cast<u32>(kSize)}),
              D3DERR_NOTFOUND_D3D);

    // Unknown / non-Vitreo IUnknown pointer → INVALIDCALL.
    ASSERT_TRUE(memory_.write_u32(kValue, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {buffer, static_cast<u32>(kGuid),
                      static_cast<u32>(kValue), 4, D3DSPD_IUNKNOWN}),
              D3DERR_INVALIDCALL);

    // Null GUID pointer → INVALIDCALL.
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {buffer, 0, static_cast<u32>(kValue), 4, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, ResourcePrivateDataIUnknownAddRefRelease) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 owner = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(owner, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */,
                      static_cast<u32>(SCRATCH_ADDR + 0x40), 0}),
              D3D_OK);
    const u32 texture_a = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_NE(texture_a, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21 /* A8R8G8B8 */, 1 /* MANAGED */,
                      static_cast<u32>(SCRATCH_ADDR + 0x44), 0}),
              D3D_OK);
    const u32 texture_b = memory_.read_u32(SCRATCH_ADDR + 0x44).value();
    ASSERT_NE(texture_b, 0u);

    constexpr GuestAddr kGuid = SCRATCH_ADDR + 0x100;
    constexpr GuestAddr kPtr = SCRATCH_ADDR + 0x120;
    constexpr GuestAddr kSize = SCRATCH_ADDR + 0x140;
    constexpr GuestAddr kOut = SCRATCH_ADDR + 0x160;
    const std::array<u8, 16> guid{
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
        0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90};
    ASSERT_TRUE(memory_.write_bytes(kGuid, guid.data(), guid.size()).is_ok());

    // Probe living refcount: AddRef returns new count; Release restores.
    const u32 a_after_probe = invoke(D3D9InterfaceId::Texture, 1, {texture_a});
    ASSERT_GT(a_after_probe, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}),
              a_after_probe - 1);
    const u32 a_base = a_after_probe - 1;

    const u32 b_after_probe = invoke(D3D9InterfaceId::Texture, 1, {texture_b});
    ASSERT_GT(b_after_probe, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_b}),
              b_after_probe - 1);
    const u32 b_base = b_after_probe - 1;

    // Set IUNKNOWN → AddRefs the stored interface.
    ASSERT_TRUE(memory_.write_u32(kPtr, texture_a).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kPtr),
                      4, D3DSPD_IUNKNOWN}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_a}), a_base + 2);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), a_base + 1);

    // Get returns the same pointer and AddRefs for the caller.
    ASSERT_TRUE(memory_.write_u32(kSize, 4).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 5,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kOut),
                      static_cast<u32>(kSize)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(kOut).value(), texture_a);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_a}), a_base + 3);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), a_base + 2);
    // Caller Releases the GetPrivateData AddRef.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), a_base + 1);

    // Replace with texture_b → Releases A, AddRefs B.
    ASSERT_TRUE(memory_.write_u32(kPtr, texture_b).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kPtr),
                      4, D3DSPD_IUNKNOWN}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_a}), a_base + 1);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), a_base);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_b}), b_base + 2);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_b}), b_base + 1);

    // Free → Releases B.
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 6,
                     {owner, static_cast<u32>(kGuid)}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_b}), b_base + 1);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_b}), b_base);

    // Destroy owner while holding IUNKNOWN → Releases stored interface.
    ASSERT_TRUE(memory_.write_u32(kPtr, texture_a).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kPtr),
                      4, D3DSPD_IUNKNOWN}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_a}), a_base + 2);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), a_base + 1);

    // Drain the owner's refs (create left it at 1).
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 2, {owner}), 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_a}), a_base + 1);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), a_base);

    // Wrong SizeOfData for IUNKNOWN → INVALIDCALL.
    ASSERT_TRUE(memory_.write_u32(kPtr, texture_a).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 4,
                     {texture_b, static_cast<u32>(kGuid),
                      static_cast<u32>(kPtr), 8, D3DSPD_IUNKNOWN}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, ResourcePrivateDataGuestIUnknownViaCallback) {
    // Minimal guest IUnknown: [vtable*][refcount]. Callback reads AddRef/
    // Release slots (ComCore's role at the composition root).
    constexpr GuestAddr kVtable = SCRATCH_ADDR + 0x200;
    constexpr GuestAddr kObject = SCRATCH_ADDR + 0x220;
    constexpr GuestAddr kAddRef = 0x71001000;
    constexpr GuestAddr kRelease = 0x71001010;
    ASSERT_TRUE(memory_.write_u32(kVtable + 0, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(kVtable + 4, kAddRef).is_ok());
    ASSERT_TRUE(memory_.write_u32(kVtable + 8, kRelease).is_ok());
    ASSERT_TRUE(memory_.write_u32(kObject, static_cast<u32>(kVtable)).is_ok());
    ASSERT_TRUE(memory_.write_u32(kObject + 4, 1).is_ok());

    u32 callback_addrefs = 0;
    u32 callback_releases = 0;
    mgr_.set_guest_iunknown_refcount(
        [&](u32 punk) -> u32 {
            EXPECT_EQ(punk, static_cast<u32>(kObject));
            auto vtable_r = memory_.read_u32(punk);
            if (!vtable_r.is_ok() || vtable_r.value() == 0) return 0;
            auto method_r = memory_.read_u32(vtable_r.value() + 4);
            if (!method_r.is_ok() || method_r.value() != kAddRef) return 0;
            auto refs_r = memory_.read_u32(punk + 4);
            if (!refs_r.is_ok()) return 0;
            const u32 next = refs_r.value() + 1;
            EXPECT_TRUE(memory_.write_u32(punk + 4, next).is_ok());
            ++callback_addrefs;
            return next;
        },
        [&](u32 punk) {
            EXPECT_EQ(punk, static_cast<u32>(kObject));
            auto vtable_r = memory_.read_u32(punk);
            ASSERT_TRUE(vtable_r.is_ok());
            auto method_r = memory_.read_u32(vtable_r.value() + 8);
            ASSERT_TRUE(method_r.is_ok());
            EXPECT_EQ(method_r.value(), kRelease);
            auto refs_r = memory_.read_u32(punk + 4);
            ASSERT_TRUE(refs_r.is_ok());
            ASSERT_GT(refs_r.value(), 0u);
            EXPECT_TRUE(
                memory_.write_u32(punk + 4, refs_r.value() - 1).is_ok());
            ++callback_releases;
        });

    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 owner = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(owner, 0u);

    constexpr GuestAddr kGuid = SCRATCH_ADDR + 0x100;
    constexpr GuestAddr kPtr = SCRATCH_ADDR + 0x120;
    constexpr GuestAddr kSize = SCRATCH_ADDR + 0x140;
    constexpr GuestAddr kOut = SCRATCH_ADDR + 0x160;
    const std::array<u8, 16> guid{
        0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
        0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x91};
    ASSERT_TRUE(memory_.write_bytes(kGuid, guid.data(), guid.size()).is_ok());
    ASSERT_TRUE(memory_.write_u32(kPtr, static_cast<u32>(kObject)).is_ok());

    // Set → AddRef via callback.
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kPtr),
                      4, D3DSPD_IUNKNOWN}),
              D3D_OK);
    EXPECT_EQ(callback_addrefs, 1u);
    EXPECT_EQ(memory_.read_u32(kObject + 4).value(), 2u);

    // Get → AddRef for the caller.
    ASSERT_TRUE(memory_.write_u32(kSize, 4).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 5,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kOut),
                      static_cast<u32>(kSize)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(kOut).value(), static_cast<u32>(kObject));
    EXPECT_EQ(callback_addrefs, 2u);
    EXPECT_EQ(memory_.read_u32(kObject + 4).value(), 3u);

    // Caller Releases the GetPrivateData ref (guest-side; not via D3D9).
    ASSERT_TRUE(memory_.write_u32(kObject + 4, 2).is_ok());

    // Free → Release via callback.
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 6,
                     {owner, static_cast<u32>(kGuid)}),
              D3D_OK);
    EXPECT_EQ(callback_releases, 1u);
    EXPECT_EQ(memory_.read_u32(kObject + 4).value(), 1u);

    // Destroy owner while holding guest IUnknown → Release via callback.
    ASSERT_TRUE(memory_.write_u32(kPtr, static_cast<u32>(kObject)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 4,
                     {owner, static_cast<u32>(kGuid), static_cast<u32>(kPtr),
                      4, D3DSPD_IUNKNOWN}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(kObject + 4).value(), 2u);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 2, {owner}), 0u);
    EXPECT_EQ(callback_releases, 2u);
    EXPECT_EQ(memory_.read_u32(kObject + 4).value(), 1u);
}
