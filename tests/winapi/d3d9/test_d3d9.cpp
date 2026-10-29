/// @file test_d3d9.cpp
/// @brief Contract tests for the D3D9.dll guest COM frontend: the pure
///        translation helpers and the public constants. The device-level tests
///        live in the test_d3d9_*.cpp files beside it.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <graphics/contracts/backend_device.hpp>
#include <graphics/shaders/ff_emulation.hpp>
#include <winapi/graphics/d3d9/d3d9.hpp>
#include <winapi/graphics/d3d9/d3d9_state.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

TEST(D3D9GammaRamp, IdentityAndRgba8Pack) {
    std::array<u16, D3DGAMMARAMP_CHANNEL_ENTRIES * 3> ramp{};
    d3d9_fill_identity_gamma_ramp(ramp);
    EXPECT_TRUE(d3d9_gamma_ramp_is_identity(ramp));
    ramp[10] = 0x4000;
    EXPECT_FALSE(d3d9_gamma_ramp_is_identity(ramp));

    std::array<u8, D3DGAMMARAMP_CHANNEL_ENTRIES * 4> rgba{};
    d3d9_gamma_ramp_to_rgba8(ramp, rgba);
    EXPECT_EQ(rgba[0], 0u);
    EXPECT_EQ(rgba[1 * 4], 1u);
    EXPECT_EQ(rgba[10 * 4], 0x40u);
    EXPECT_EQ(rgba[255 * 4], 255u);
    EXPECT_EQ(rgba[10 * 4 + 3], 255u);
}

// D3DRS_CULLMODE names the winding that is discarded; the backend needs a
// (front face, culled face) pair. Both halves must be produced together, so
// every case pins the winding as well as the cull mode.
TEST(D3D9CullTranslation, NoneCullsNothing) {
    const auto result = translate_d3d9_cull_mode(D3DCULL_NONE);
    EXPECT_EQ(result.cull_mode, graphics::BackendCullMode::None);
    EXPECT_FALSE(result.front_counter_clockwise);
}

TEST(D3D9CullTranslation, ClockwiseDiscardsTheFrontFace) {
    const auto result = translate_d3d9_cull_mode(D3DCULL_CW);
    EXPECT_FALSE(result.front_counter_clockwise);
    EXPECT_EQ(result.cull_mode, graphics::BackendCullMode::Front);
}

TEST(D3D9CullTranslation, CounterClockwiseDiscardsTheBackFace) {
    const auto result = translate_d3d9_cull_mode(D3DCULL_CCW);
    EXPECT_FALSE(result.front_counter_clockwise);
    EXPECT_EQ(result.cull_mode, graphics::BackendCullMode::Back);
}

TEST(D3D9CullTranslation, OutOfRangeValuesCullNothing) {
    for (const u32 mode : {0u, 4u, 0xFFFFFFFFu}) {
        const auto result = translate_d3d9_cull_mode(mode);
        EXPECT_EQ(result.cull_mode, graphics::BackendCullMode::None) << mode;
    }
}

// The default-constructed draw call must agree with the canonical D3D9 front
// face, so a frontend that forgets to set the winding cannot silently invert
// culling.
TEST(D3D9CullTranslation, MatchesBackendDrawCallDefaultWinding) {
    const graphics::BackendDrawCall call{};
    EXPECT_EQ(translate_d3d9_cull_mode(D3DCULL_CCW).front_counter_clockwise,
              call.front_counter_clockwise);
}

TEST(D3D9FrozenTables, FreezeIsSharedUntilInvalidated) {
    D3D9Manager::DeviceState state;
    state.render_states[7] = 1;
    const auto first = state.freeze_draw_tables();
    const auto second = state.freeze_draw_tables();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->render_states.at(7), 1u);

    state.invalidate_draw_pipeline_reuse();
    const auto third = state.freeze_draw_tables();
    ASSERT_NE(third, nullptr);
    EXPECT_NE(third, first);
    EXPECT_EQ(third->render_states.at(7), 1u);
}

TEST(D3D9BumpLuminanceFormatTest, ExpandsMixedSignedAndUnsignedChannels) {
    std::vector<u8> expanded;
    // L6V5U5: L=32, V=-8, U=+8. Format names bits MSB-to-LSB.
    const u16 l6v5u5 = static_cast<u16>((32u << 10u) | (24u << 5u) | 8u);
    const u8 packed16[2] = {static_cast<u8>(l6v5u5),
                            static_cast<u8>(l6v5u5 >> 8u)};
    ASSERT_TRUE(d3d9_expand_special_bump_to_rgba32f(
        61, 1, 1, packed16, sizeof(packed16), expanded));
    ASSERT_EQ(expanded.size(), 4u * sizeof(float));
    float channels[4]{};
    std::memcpy(channels, expanded.data(), sizeof(channels));
    EXPECT_FLOAT_EQ(channels[0], 8.0f / 15.0f);
    EXPECT_FLOAT_EQ(channels[1], -8.0f / 15.0f);
    EXPECT_FLOAT_EQ(channels[2], 32.0f / 63.0f);
    EXPECT_FLOAT_EQ(channels[3], 1.0f);

    // X8L8V8U8 is U,V,L,X in little-endian memory.
    const u8 packed32[4] = {64u, 192u, 128u, 0xA5u};
    ASSERT_TRUE(d3d9_expand_special_bump_to_rgba32f(
        62, 1, 1, packed32, sizeof(packed32), expanded));
    std::memcpy(channels, expanded.data(), sizeof(channels));
    EXPECT_FLOAT_EQ(channels[0], 64.0f / 127.0f);
    EXPECT_FLOAT_EQ(channels[1], -64.0f / 127.0f);
    EXPECT_FLOAT_EQ(channels[2], 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(channels[3], 1.0f);

    // CxV8U8 stores U,V and derives the positive normal component C.
    const u8 cxv8u8[2] = {64u, 0u};
    ASSERT_TRUE(d3d9_expand_special_bump_to_rgba32f(
        117, 1, 1, cxv8u8, sizeof(cxv8u8), expanded));
    std::memcpy(channels, expanded.data(), sizeof(channels));
    EXPECT_FLOAT_EQ(channels[0], 64.0f / 127.0f);
    EXPECT_FLOAT_EQ(channels[1], 0.0f);
    EXPECT_NEAR(channels[2], std::sqrt(1.0f - channels[0] * channels[0]),
                1.0e-6f);
    EXPECT_FLOAT_EQ(channels[3], 1.0f);

    const u8 v16u16[4] = {0u, 64u, 0u, 192u};
    ASSERT_TRUE(d3d9_expand_special_bump_to_rgba32f(
        64, 1, 1, v16u16, sizeof(v16u16), expanded));
    std::memcpy(channels, expanded.data(), sizeof(channels));
    EXPECT_FLOAT_EQ(channels[0], 16384.0f / 32767.0f);
    EXPECT_FLOAT_EQ(channels[1], -16384.0f / 32767.0f);
    EXPECT_FLOAT_EQ(channels[2], 0.0f);
    EXPECT_FLOAT_EQ(channels[3], 1.0f);
}

TEST(D3D9ShaderContractTest,
     HybridPreTransformedVertexUsesMetalPixelCenterOffset) {
    graphics::FFPipelineState state{};
    state.fvf_flags = graphics::fvf::XYZRHW | graphics::fvf::DIFFUSE;
    state.lighting_enabled = false;

    const std::string vertex =
        graphics::generate_ff_vertex_for_sm3_varyings(state, {0}, {});
    EXPECT_NE(vertex.find("in.position.xy + float2(0.5, 0.5)"),
              std::string::npos) << vertex;
    EXPECT_EQ(vertex.find("in.position.xy - float2(0.5, 0.5)"),
              std::string::npos) << vertex;
}

} // namespace

// =============================================================================
// D3D9 Constants
// =============================================================================

TEST(D3D9ConstantsTest, D3DOkIsZero) {
    EXPECT_EQ(D3D_OK, 0u);
}

TEST(D3D9ConstantsTest, D3DErrInvalidCall) {
    EXPECT_EQ(D3DERR_INVALIDCALL, 0x8876086Cu);
}

TEST(D3D9ConstantsTest, ENoInterfaceD3D) {
    EXPECT_EQ(E_NOINTERFACE_D3D, 0x80004002u);
}

TEST(D3D9ConstantsTest, ENotImplD3D) {
    EXPECT_EQ(E_NOTIMPL_D3D, 0x80004001u);
}

TEST(D3D9ConstantsTest, D3DSdkVersion) {
    EXPECT_EQ(D3D_SDK_VERSION, 32u);
}
