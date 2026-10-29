/// @file test_d3d9_fixed_function_state.cpp
/// @brief Render state reaching the backend and declaration-driven fixed-function pipelines.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;
using vitreo::winapi::d3d9_test::test_f32_bits;

TEST_F(D3D9ManagerTest, DepthBiasRenderStatesReachBackendDraw) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device, 0x002 /* XYZ */ |
                                               0x040 /* DIFFUSE */),
              0u);
    const float constant_bias = -0.125f;
    const float slope_bias = 1.75f;
    u32 constant_bits = 0;
    u32 slope_bits = 0;
    std::memcpy(&constant_bits, &constant_bias, sizeof(constant_bits));
    std::memcpy(&slope_bits, &slope_bias, sizeof(slope_bits));
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 195 /* DEPTHBIAS */, constant_bits}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 175 /* SLOPESCALEDEPTHBIAS */, slope_bits}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draw_calls.size(), 1u);
    EXPECT_FLOAT_EQ(backend_.record.draw_calls[0].depth_bias, constant_bias);
    EXPECT_FLOAT_EQ(backend_.record.draw_calls[0].depth_bias_slope_scale,
                    slope_bias);
}

TEST_F(D3D9ManagerTest, TableFogUsesPerPixelDeviceDepth) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device, 0x002 /* XYZ */ |
                                               0x040 /* DIFFUSE */),
              0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 28 /* FOGENABLE */, 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 35 /* FOGTABLEMODE */, 3 /* LINEAR */}),
              D3D_OK);
    const float start = 0.25f;
    const float end = 0.75f;
    u32 start_bits = 0;
    u32 end_bits = 0;
    std::memcpy(&start_bits, &start, sizeof(start_bits));
    std::memcpy(&end_bits, &end, sizeof(end_bits));
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 36 /* FOGSTART */, start_bits}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 37 /* FOGEND */, end_bits}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    const std::string& fragment =
        backend_.record.last_pipeline_desc.fragment_shader_msl;
    EXPECT_NE(fragment.find("float fog_distance = in.position.z;"),
              std::string::npos) << fragment;
    EXPECT_NE(fragment.find("float4 fog_params;"), std::string::npos)
        << fragment;
    const auto& uniforms = backend_.record.draw_fragment_constants.front();
    ASSERT_GE(uniforms.size(), sizeof(float) * 4u);
    std::array<float, 4> params{};
    std::memcpy(params.data(), uniforms.data() + uniforms.size() - 16,
                sizeof(params));
    EXPECT_FLOAT_EQ(params[0], start);
    EXPECT_FLOAT_EQ(params[1], end);
}

TEST_F(D3D9ManagerTest, BlendOperationsAndSeparateAlphaReachBackendPipeline) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device, 0x002 /* XYZ */ |
                                               0x040 /* DIFFUSE */),
              0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 27 /* ALPHABLENDENABLE */, 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 19 /* SRCBLEND */, 2 /* ONE */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 20 /* DESTBLEND */, 1 /* ZERO */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 171 /* BLENDOP */, 2 /* SUBTRACT */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 206 /* SEPARATEALPHABLENDENABLE */, 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 207 /* SRCBLENDALPHA */, 1 /* ZERO */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 208 /* DESTBLENDALPHA */, 2 /* ONE */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 209 /* BLENDOPALPHA */, 3 /* REVSUBTRACT */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    const auto& pipeline = backend_.record.last_pipeline_desc;
    EXPECT_TRUE(pipeline.blend_enabled);
    EXPECT_EQ(pipeline.source_blend, graphics::BackendBlendFactor::One);
    EXPECT_EQ(pipeline.destination_blend, graphics::BackendBlendFactor::Zero);
    EXPECT_EQ(pipeline.blend_operation,
              graphics::BackendBlendOperation::Subtract);
    EXPECT_EQ(pipeline.source_blend_alpha,
              graphics::BackendBlendFactor::Zero);
    EXPECT_EQ(pipeline.destination_blend_alpha,
              graphics::BackendBlendFactor::One);
    EXPECT_EQ(pipeline.blend_operation_alpha,
              graphics::BackendBlendOperation::ReverseSubtract);
}

TEST_F(D3D9ManagerTest, AdvertisedBlendFactorsReachBackendDraw) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device, 0x002 /* XYZ */ |
                                               0x040 /* DIFFUSE */),
              0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 27 /* ALPHABLENDENABLE */, 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 19 /* SRCBLEND */, 11 /* SRCALPHASAT */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 20 /* DESTBLEND */, 1 /* ZERO */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.last_pipeline_desc.source_blend,
              graphics::BackendBlendFactor::SourceAlphaSaturated);

    constexpr u32 kBlendFactor = 0x80402010u;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 19 /* SRCBLEND */, 14 /* BLENDFACTOR */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 20 /* DESTBLEND */, 15 /* INVBLENDFACTOR */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 193 /* BLENDFACTOR */, kBlendFactor}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    EXPECT_EQ(backend_.record.last_pipeline_desc.source_blend,
              graphics::BackendBlendFactor::BlendColor);
    EXPECT_EQ(backend_.record.last_pipeline_desc.destination_blend,
              graphics::BackendBlendFactor::OneMinusBlendColor);
    ASSERT_GE(backend_.record.draw_calls.size(), 2u);
    const auto& blend_color = backend_.record.draw_calls.back().blend_color;
    EXPECT_FLOAT_EQ(blend_color[0], 64.0f / 255.0f);
    EXPECT_FLOAT_EQ(blend_color[1], 32.0f / 255.0f);
    EXPECT_FLOAT_EQ(blend_color[2], 16.0f / 255.0f);
    EXPECT_FLOAT_EQ(blend_color[3], 128.0f / 255.0f);
}

TEST_F(D3D9ManagerTest, BothSourceAlphaBlendOverridesDestinationState) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device, 0x002 /* XYZ */ |
                                               0x040 /* DIFFUSE */),
              0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 27 /* ALPHABLENDENABLE */, 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 19 /* SRCBLEND */, 12 /* BOTHSRCALPHA */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 20 /* DESTBLEND */, 1 /* deliberately ZERO */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    const auto& pipeline = backend_.record.last_pipeline_desc;
    EXPECT_EQ(pipeline.source_blend,
              graphics::BackendBlendFactor::SourceAlpha);
    EXPECT_EQ(pipeline.destination_blend,
              graphics::BackendBlendFactor::OneMinusSourceAlpha);
    EXPECT_EQ(pipeline.source_blend_alpha,
              graphics::BackendBlendFactor::SourceAlpha);
    EXPECT_EQ(pipeline.destination_blend_alpha,
              graphics::BackendBlendFactor::OneMinusSourceAlpha);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 19 /* SRCBLEND */,
                      13 /* BOTHINVSRCALPHA */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);
    EXPECT_EQ(backend_.record.last_pipeline_desc.source_blend,
              graphics::BackendBlendFactor::OneMinusSourceAlpha);
    EXPECT_EQ(backend_.record.last_pipeline_desc.destination_blend,
              graphics::BackendBlendFactor::SourceAlpha);
}

TEST_F(D3D9ManagerTest, LastBetaD3dColorIsRejectedHonestly) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(
                  device, 0x008 /* XYZB2 */ | 0x8000 /* LASTBETA_D3DCOLOR */ |
                              0x040 /* DIFFUSE */),
              0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              E_NOTIMPL_D3D);
}

TEST_F(D3D9ManagerTest, FixedFunctionTweeningPreservesDeclarationSemantics) {
    const auto [factory, device] = create_device();
    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},    // FLOAT3 POSITION0
        {0, 12, 2, 0, 0, 1},   // FLOAT3 POSITION1
        {0, 24, 2, 0, 3, 0},   // FLOAT3 NORMAL0
        {0, 36, 2, 0, 3, 1},   // FLOAT3 NORMAL1
        {0, 48, 4, 0, 10, 0},  // D3DCOLOR COLOR0
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x700, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x700),
                      static_cast<u32>(SCRATCH_ADDR + 0x780)}), D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x780).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 52u * 3u, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x784), 0}), D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR + 0x784).value();
    ASSERT_NE(buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87,
                     {device, declaration}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, buffer, 0, 52}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 151 /* VERTEXBLEND */, 255 /* TWEENING */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 170 /* TWEENFACTOR */, test_f32_bits(0.25f)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 5u);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 24u);
    EXPECT_EQ(desc.vertex_attributes[3].offset, 36u);
    EXPECT_EQ(desc.vertex_attributes[4].offset, 48u);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float3 position1 [[attribute(1)]];"), std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float3 normal1 [[attribute(3)]];"), std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "mix(in.position, in.position1, cb.tween_params.x)"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "mix(in.normal, in.normal1, cb.tween_params.x)"),
              std::string::npos);
    const auto& constants = backend_.record.draw_vertex_constants.front();
    ASSERT_GE(constants.size(), 336u);
    float packed_tween = 0.0f;
    std::memcpy(&packed_tween, constants.data() + 320u,
                sizeof(packed_tween));
    EXPECT_FLOAT_EQ(packed_tween, 0.25f);
}

TEST_F(D3D9ManagerTest, FixedFunctionPositionTDeclarationBuildsPretransformedPipeline) {
    // Learn D3DDECLUSAGE_POSITIONT: screen-space vertices skip VS processing.
    // A declaration with only POSITIONT (no POSITION0) must still produce a
    // pipeline; dropping it leaves 2D UI off the colour target.
    const auto [factory, device] = create_device();
    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    const Element elements[] = {
        {0, 0, 3, 0, 9, 0},    // FLOAT4 POSITIONT
        {0, 16, 4, 0, 10, 0},  // D3DCOLOR COLOR0
        {0, 20, 1, 0, 5, 0},   // FLOAT2 TEXCOORD0
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x700, reinterpret_cast<const Byte*>(elements),
        sizeof(elements)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x700),
                      static_cast<u32>(SCRATCH_ADDR + 0x780)}), D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x780).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 28u * 3u, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x784), 0}), D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR + 0x784).value();
    ASSERT_NE(buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87,
                     {device, declaration}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, buffer, 0, 28}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 3u);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 16u);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 20u);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "screen.x / cb.viewport_params.x"), std::string::npos);
    EXPECT_EQ(desc.vertex_shader_msl.find(
                  "cb.world_view_proj * float4"), std::string::npos);
}
