/// @file test_d3d9_state.cpp
/// @brief Device state store: render, light, texture-stage, sampler and transform state.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

// =============================================================================
// IDirect3DDevice9 — state store
// =============================================================================

TEST_F(D3D9ManagerTest, RenderStateReadsBackWindowsDefaultsBeforeAnySet) {
    const auto [factory, device] = create_device();
    const auto get_state = [&](u32 state) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 58,
                         {device, state, static_cast<u32>(SCRATCH_ADDR)}),
                  D3D_OK);
        return memory_.read_u32(SCRATCH_ADDR).value();
    };

    EXPECT_EQ(get_state(8), 3u);            // D3DRS_FILLMODE = D3DFILL_SOLID
    EXPECT_EQ(get_state(22), 3u);           // D3DRS_CULLMODE = D3DCULL_CCW
    EXPECT_EQ(get_state(23), 4u);           // D3DRS_ZFUNC = D3DCMP_LESSEQUAL
    EXPECT_EQ(get_state(137), 1u);          // D3DRS_LIGHTING = TRUE
    EXPECT_EQ(get_state(168), 0x0000000Fu); // D3DRS_COLORWRITEENABLE
    EXPECT_EQ(get_state(58), 0xFFFFFFFFu);  // D3DRS_STENCILMASK
    // D3DRS_ZENABLE follows whether the device has a depth buffer.
    EXPECT_EQ(get_state(7), 0u);
}

TEST_F(D3D9ManagerTest, ClipStatusDefaultsMatchLearn) {
    const auto [factory, device] = create_device();
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 4, 0xCAFEBABEu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 63,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    // Learn: ClipUnion=0, ClipIntersection=0xFFFFFFFF before any draw/Set.
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 4).value(), 0xFFFFFFFFu);
}

TEST_F(D3D9ManagerTest, ClipStatusRoundTrips) {
    const auto [factory, device] = create_device();
    const std::array<u32, 2> status{0x00000003u, 0x00000001u};
    ASSERT_TRUE(memory_.write_bytes(
                    SCRATCH_ADDR, reinterpret_cast<const Byte*>(status.data()),
                    sizeof(status))
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 62,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);

    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 0u).is_ok());
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR + 4, 0u).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 63,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0x00000003u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 4).value(), 0x00000001u);
}

TEST_F(D3D9ManagerTest, ClipStatusRejectsNullPointer) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 62, {device, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 63, {device, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, GetLightNeverSetReturnsLearnDefaults) {
    // Learn LightEnable table: directional, Diffuse=(1,1,1,0), Direction=(0,0,1).
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 52,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    std::array<u8, 104> light{};
    ASSERT_TRUE(memory_
                    .read_bytes(SCRATCH_ADDR,
                                reinterpret_cast<Byte*>(light.data()),
                                light.size())
                    .is_ok());
    u32 type = 0;
    std::memcpy(&type, light.data(), sizeof(type));
    EXPECT_EQ(type, 3u); // D3DLIGHT_DIRECTIONAL
    float diffuse[4]{};
    float direction[3]{};
    std::memcpy(diffuse, light.data() + 4, sizeof(diffuse));
    std::memcpy(direction, light.data() + 64, sizeof(direction));
    EXPECT_FLOAT_EQ(diffuse[0], 1.0f);
    EXPECT_FLOAT_EQ(diffuse[1], 1.0f);
    EXPECT_FLOAT_EQ(diffuse[2], 1.0f);
    EXPECT_FLOAT_EQ(diffuse[3], 0.0f);
    EXPECT_FLOAT_EQ(direction[0], 0.0f);
    EXPECT_FLOAT_EQ(direction[1], 0.0f);
    EXPECT_FLOAT_EQ(direction[2], 1.0f);
    // Remaining scalars stay zero in the documented default.
    float range = 1.0f;
    std::memcpy(&range, light.data() + 76, sizeof(range));
    EXPECT_FLOAT_EQ(range, 0.0f);
}

TEST_F(D3D9ManagerTest, SetLightGetLightRoundTrips) {
    const auto [factory, device] = create_device();
    std::array<u8, 104> written{};
    u32 type = 1; // D3DLIGHT_POINT
    std::memcpy(written.data(), &type, sizeof(type));
    const float diffuse[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    const float position[3] = {1.0f, 2.0f, 3.0f};
    std::memcpy(written.data() + 4, diffuse, sizeof(diffuse));
    std::memcpy(written.data() + 52, position, sizeof(position));
    float range = 100.0f;
    std::memcpy(written.data() + 76, &range, sizeof(range));
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR,
                                 reinterpret_cast<const Byte*>(written.data()),
                                 written.size())
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 51,
                     {device, 7, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);

    std::array<u8, 104> read_back{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 52,
                     {device, 7, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    ASSERT_TRUE(memory_
                    .read_bytes(SCRATCH_ADDR + 0x100,
                                reinterpret_cast<Byte*>(read_back.data()),
                                read_back.size())
                    .is_ok());
    EXPECT_EQ(written, read_back);
}

TEST_F(D3D9ManagerTest, GetLightRejectsNullPointer) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 52, {device, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 51, {device, 0, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, LightEnableNeverSetCreatesLearnDefault) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 53, {device, 3, 1}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 54,
                     {device, 3, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 1u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 52,
                     {device, 3, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    u32 type = 0;
    ASSERT_TRUE(memory_
                    .read_bytes(SCRATCH_ADDR + 0x100,
                                reinterpret_cast<Byte*>(&type), sizeof(type))
                    .is_ok());
    EXPECT_EQ(type, 3u); // D3DLIGHT_DIRECTIONAL
}

TEST_F(D3D9ManagerTest, RenderStateZEnableDefaultsTrueWithAutoDepthStencil) {
    const auto [factory, device] = create_device(true);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 58,
                     {device, 7, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 1u);
}

TEST_F(D3D9ManagerTest, RenderStateRoundTrips) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 57, {device, 22, 1}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 58,
                     {device, 22, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 1u);
}

TEST_F(D3D9ManagerTest, TextureStageAndSamplerStateDefaultsMatchWindows) {
    const auto [factory, device] = create_device();
    const auto stage_state = [&](u32 stage, u32 type) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 66,
                         {device, stage, type,
                          static_cast<u32>(SCRATCH_ADDR)}),
                  D3D_OK);
        return memory_.read_u32(SCRATCH_ADDR).value();
    };
    const auto sampler_state = [&](u32 sampler, u32 type) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 68,
                         {device, sampler, type,
                          static_cast<u32>(SCRATCH_ADDR)}),
                  D3D_OK);
        return memory_.read_u32(SCRATCH_ADDR).value();
    };

    EXPECT_EQ(stage_state(0, 1), 4u);  // stage 0 COLOROP = MODULATE
    EXPECT_EQ(stage_state(1, 1), 1u);  // stage 1 COLOROP = DISABLE
    EXPECT_EQ(stage_state(0, 4), 2u);  // stage 0 ALPHAOP = SELECTARG1
    EXPECT_EQ(stage_state(3, 11), 3u); // TEXCOORDINDEX defaults to the stage
    EXPECT_EQ(stage_state(0, 32), 0xFFFFFFFFu); // CONSTANT defaults opaque white
    EXPECT_EQ(stage_state(7, 32), 0xFFFFFFFFu); // independently for every stage
    EXPECT_EQ(sampler_state(0, 1), 1u);  // ADDRESSU = WRAP
    EXPECT_EQ(sampler_state(0, 3), 1u);  // ADDRESSW = WRAP
    EXPECT_EQ(sampler_state(0, 4), 0u);  // BORDERCOLOR = 0x00000000
    EXPECT_EQ(sampler_state(0, 5), 1u);  // MAGFILTER = POINT
    EXPECT_EQ(sampler_state(0, 10), 1u); // MAXANISOTROPY = 1
}

TEST_F(D3D9ManagerTest, TextureStageConstantsRoundTripIndependently) {
    const auto [factory, device] = create_device();
    constexpr u32 D3DTSS_CONSTANT = 32;
    constexpr u32 kStage0 = 0xFF102030u;
    constexpr u32 kStage1 = 0xFFABCDEFu;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 0, D3DTSS_CONSTANT, kStage0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 67,
                     {device, 1, D3DTSS_CONSTANT, kStage1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 66,
                     {device, 0, D3DTSS_CONSTANT,
                      static_cast<u32>(SCRATCH_ADDR)}), D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), kStage0);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 66,
                     {device, 1, D3DTSS_CONSTANT,
                      static_cast<u32>(SCRATCH_ADDR)}), D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), kStage1);
}

TEST_F(D3D9ManagerTest, SamplerAddressWRoundTripsViaGetSamplerState) {
    // Learn: D3DSAMP_ADDRESSW (3) Set/GetSamplerState round-trips.
    const auto [factory, device] = create_device();
    constexpr u32 D3DSAMP_ADDRESSW = 3;
    constexpr u32 D3DTADDRESS_CLAMP = 3;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, 0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 68,
                     {device, 0, D3DSAMP_ADDRESSW,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), D3DTADDRESS_CLAMP);
}

TEST_F(D3D9ManagerTest, SamplerBorderColorRoundTripsViaGetSamplerState) {
    // Learn: D3DSAMP_BORDERCOLOR (4) Set/GetSamplerState round-trips the
    // full D3DCOLOR even when Metal can only realize three border colors.
    const auto [factory, device] = create_device();
    constexpr u32 D3DSAMP_BORDERCOLOR = 4;
    constexpr u32 kBorder = 0x80FF4000u; // semi-transparent orange
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, 0, D3DSAMP_BORDERCOLOR, kBorder}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 68,
                     {device, 0, D3DSAMP_BORDERCOLOR,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), kBorder);
}

TEST_F(D3D9ManagerTest, TransformDefaultsToIdentityAndRoundTrips) {
    const auto [factory, device] = create_device();
    std::array<float, 16> matrix{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 45,
                     {device, 2 /* D3DTS_VIEW */,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR, reinterpret_cast<Byte*>(matrix.data()),
        sizeof(matrix)).is_ok());
    for (usize i = 0; i < 16; ++i) {
        EXPECT_FLOAT_EQ(matrix[i], (i % 5 == 0) ? 1.0f : 0.0f) << "element " << i;
    }

    std::array<float, 16> written{};
    for (usize i = 0; i < 16; ++i) written[i] = static_cast<float>(i);
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(written.data()),
        sizeof(written)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 44,
                     {device, 2, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    std::array<float, 16> read_back{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 45,
                     {device, 2, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR + 0x100, reinterpret_cast<Byte*>(read_back.data()),
        sizeof(read_back)).is_ok());
    EXPECT_EQ(written, read_back);
}

TEST_F(D3D9ManagerTest, MultiplyTransformLeftMultipliesAndRejectsNull) {
    // Learn: MultiplyTransform stores pMatrix * State (left-multiply).
    const auto [factory, device] = create_device();
    constexpr u32 kView = 2; // D3DTS_VIEW

    // Identity * A == A when the transform is unset.
    const std::array<float, 16> a{
        2, 0, 0, 0,
        0, 3, 0, 0,
        0, 0, 4, 0,
        5, 6, 7, 1,
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(a.data()),
        sizeof(a)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 46,
                     {device, kView, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    std::array<float, 16> got{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 45,
                     {device, kView, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR + 0x100, reinterpret_cast<Byte*>(got.data()),
        sizeof(got)).is_ok());
    EXPECT_EQ(got, a);

    // Second multiply accumulates: B * A.
    const std::array<float, 16> b{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        10, 20, 30, 1,
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(b.data()),
        sizeof(b)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 46,
                     {device, kView, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 45,
                     {device, kView, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR + 0x100, reinterpret_cast<Byte*>(got.data()),
        sizeof(got)).is_ok());
    // Row-major: (B * A)_ij = sum_k B_ik A_kj
    std::array<float, 16> expected{};
    for (usize row = 0; row < 4; ++row) {
        for (usize col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (usize k = 0; k < 4; ++k) {
                sum += b[row * 4 + k] * a[k * 4 + col];
            }
            expected[row * 4 + col] = sum;
        }
    }
    EXPECT_EQ(got, expected);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 46, {device, kView, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, VertexShaderConstantsRoundTripAndBoundsCheck) {
    const auto [factory, device] = create_device();
    const std::array<float, 8> values{1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR, reinterpret_cast<const Byte*>(values.data()),
        sizeof(values)).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 4, static_cast<u32>(SCRATCH_ADDR), 2}),
              D3D_OK);

    std::array<float, 8> read_back{};
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 95,
                     {device, 4, static_cast<u32>(SCRATCH_ADDR + 0x100), 2}),
              D3D_OK);
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR + 0x100, reinterpret_cast<Byte*>(read_back.data()),
        sizeof(read_back)).is_ok());
    EXPECT_EQ(values, read_back);

    // Past the 256-register file.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 94,
                     {device, 255, static_cast<u32>(SCRATCH_ADDR), 2}),
              D3DERR_INVALIDCALL);
}
