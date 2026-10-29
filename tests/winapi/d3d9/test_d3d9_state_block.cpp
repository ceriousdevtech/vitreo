/// @file test_d3d9_state_block.cpp
/// @brief State block record, capture and apply.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest,
       RecordedStateBlockRestoresTextureCombinerSamplerAndVsConstants) {
    const auto [factory, device] = create_device();
    constexpr u32 D3DVERTEXTEXTURESAMPLER0 = 257;

    const auto create_texture = [&](GuestAddr out) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                         {device, 4, 4, 1, 0, 21, 1,
                          static_cast<u32>(out), 0}),
                  D3D_OK);
        return memory_.read_u32(out).value();
    };
    const u32 recorded_texture = create_texture(SCRATCH_ADDR + 0x200);
    const u32 replacement_texture = create_texture(SCRATCH_ADDR + 0x204);
    ASSERT_NE(recorded_texture, 0u);
    ASSERT_NE(replacement_texture, 0u);

    const std::array<float, 4> recorded_constant{0.25f, 0.5f, 0.75f, 1.0f};
    const std::array<float, 4> replacement_constant{9.0f, 8.0f, 7.0f, 6.0f};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x300,
        reinterpret_cast<const Byte*>(recorded_constant.data()),
        sizeof(recorded_constant)).is_ok());
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x340,
        reinterpret_cast<const Byte*>(replacement_constant.data()),
        sizeof(replacement_constant)).is_ok());

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 60, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, 0, recorded_texture}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER0, recorded_texture}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 0, 1 /* COLOROP */, 5 /* MODULATE2X */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, 0, 6 /* MINFILTER */, 2 /* LINEAR */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, D3DVERTEXTEXTURESAMPLER0,
                      6 /* MINFILTER */, 2 /* LINEAR */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 2, static_cast<u32>(SCRATCH_ADDR + 0x300), 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 61,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x400)}),
              D3D_OK);
    const u32 block = memory_.read_u32(SCRATCH_ADDR + 0x400).value();
    ASSERT_NE(block, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, 0, replacement_texture}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER0,
                      replacement_texture}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 0, 1, 2 /* SELECTARG1 */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, 0, 6, 1 /* POINT */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, D3DVERTEXTEXTURESAMPLER0, 6, 1 /* POINT */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 2, static_cast<u32>(SCRATCH_ADDR + 0x340), 1}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::StateBlock, 5, {block}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 64,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x500)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x500).value(),
              recorded_texture);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 64,
                     {device, D3DVERTEXTEXTURESAMPLER0,
                      static_cast<u32>(SCRATCH_ADDR + 0x50C)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x50C).value(),
              recorded_texture);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 66,
                     {device, 0, 1, static_cast<u32>(SCRATCH_ADDR + 0x504)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x504).value(), 5u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 68,
                     {device, 0, 6, static_cast<u32>(SCRATCH_ADDR + 0x508)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x508).value(), 2u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 68,
                     {device, D3DVERTEXTEXTURESAMPLER0, 6,
                      static_cast<u32>(SCRATCH_ADDR + 0x510)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x510).value(), 2u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 95,
                     {device, 2, static_cast<u32>(SCRATCH_ADDR + 0x540), 1}),
              D3D_OK);
    std::array<float, 4> restored{};
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR + 0x540, reinterpret_cast<Byte*>(restored.data()),
        sizeof(restored)).is_ok());
    EXPECT_EQ(restored, recorded_constant);
}

TEST_F(D3D9ManagerTest,
       StateBlockCaptureUpdatesOnlyStatesOriginallyIncluded) {
    const auto [factory, device] = create_device();

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 60, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 0, 1 /* COLOROP */, 5 /* MODULATE2X */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 61,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x400)}),
              D3D_OK);
    const u32 block = memory_.read_u32(SCRATCH_ADDR + 0x400).value();
    ASSERT_NE(block, 0u);

    // Capture changes the value of stage 0 already represented by the block.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 0, 1, 2 /* SELECTARG1 */}),
              D3D_OK);
    // Stage 1 was never represented and therefore must not be added by
    // Capture or later modified by Apply.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 1, 1, 4 /* ADD */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::StateBlock, 4, {block}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 0, 1, 1 /* DISABLE */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 1, 1, 6 /* MODULATE4X */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::StateBlock, 5, {block}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 66,
                     {device, 0, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x500)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x500).value(), 2u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 66,
                     {device, 1, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x504)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x504).value(), 6u);
}

TEST_F(D3D9ManagerTest, StateBlockCaptureFailsDuringRecordMode) {
    const auto [factory, device] = create_device();

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 60, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 7 /* ZENABLE */, 0}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 61,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x400)}),
              D3D_OK);
    const u32 block = memory_.read_u32(SCRATCH_ADDR + 0x400).value();
    ASSERT_NE(block, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 60, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::StateBlock, 4, {block}),
              D3DERR_INVALIDCALL);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 61,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x404)}),
              D3D_OK);
}
