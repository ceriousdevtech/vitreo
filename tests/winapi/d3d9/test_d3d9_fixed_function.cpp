/// @file test_d3d9_fixed_function.cpp
/// @brief Fixed-function vertex pipeline: pre-transformed, point size, clip planes and vertex blending.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, PreTransformedVerticesDrawWithAScreenSpaceShader) {
    const auto [factory, device] = create_device();
    // D3DFVF_XYZRHW | D3DFVF_DIFFUSE: a float4 position plus a packed colour.
    const u32 vertex_buffer = bind_triangle_stream(device, 0x004 | 0x040);
    ASSERT_NE(vertex_buffer, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(backend_.record.draws, 1u);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].format,
              graphics::BackendVertexFormat::Float4);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 16u);
    EXPECT_EQ(desc.vertex_stride, 20u);

    // D3D9's pixel centres are at integer screen coordinates, while Metal's
    // are offset by (0.5, 0.5). The translation must have the positive sign;
    // a negative offset double-applies the correction in D3D9 UI vertices
    // authored at -0.5 texel boundaries.
    EXPECT_NE(desc.vertex_shader_msl.find("viewport_params"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "in.position.xy + float2(0.5, 0.5)"),
              std::string::npos);
    EXPECT_EQ(desc.vertex_shader_msl.find(
                  "in.position.xy - float2(0.5, 0.5)"),
              std::string::npos);
    EXPECT_EQ(desc.vertex_shader_msl.find("cb.world_view_proj"),
              std::string::npos);

    // The render target size reaches the shader as a uniform: it follows the
    // five matrices, so it starts at byte 320.
    const std::vector<u8>& constants =
        backend_.record.draw_vertex_constants.front();
    ASSERT_GE(constants.size(), 320u + 16u);
    std::array<float, 2> viewport_size{};
    std::memcpy(viewport_size.data(), constants.data() + 320,
                sizeof(viewport_size));
    EXPECT_FLOAT_EQ(viewport_size[0], 640.0f);
    EXPECT_FLOAT_EQ(viewport_size[1], 480.0f);
}

TEST_F(D3D9ManagerTest, PreTransformedAndTransformedShadersDifferAndBothCache) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);

    (void)bind_triangle_stream(device, 0x002 | 0x040);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    const std::string transformed =
        backend_.record.last_pipeline_desc.vertex_shader_msl;

    (void)bind_triangle_stream(device, 0x004 | 0x040);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    const std::string pretransformed =
        backend_.record.last_pipeline_desc.vertex_shader_msl;

    EXPECT_NE(transformed, pretransformed);
    // The FVF is part of the shader state, so each got its own pipeline.
    EXPECT_EQ(backend_.record.pipelines_created, 2u);
    EXPECT_EQ(backend_.record.draws, 2u);
}

TEST_F(D3D9ManagerTest, FixedFunctionPointSizeFvfFeedsTheRasterizer) {
    const auto [factory, device] = create_device();
    // XYZ, then PSIZE float, then diffuse D3DCOLOR.
    const u32 vertex_buffer = bind_triangle_stream(device, 0x002 | 0x020 | 0x040);
    ASSERT_NE(vertex_buffer, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 1 /* POINTLIST */, 0, 1}),
              D3D_OK);
    ASSERT_EQ(backend_.record.draws, 1u);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 3u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::Float1);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 16u);
    EXPECT_EQ(desc.vertex_stride, 20u);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float point_size [[attribute(1)]];"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "out.point_size = clamp(in.point_size"),
              std::string::npos);
}

TEST_F(D3D9ManagerTest, FixedFunctionClipPlaneUsesWorldSpaceCoefficients) {
    const auto [factory, device] = create_device();
    (void)bind_triangle_stream(device);
    std::array<float, 16> world{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.25f, 0.0f, 0.0f, 1.0f,
    };
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x800, reinterpret_cast<const Byte*>(world.data()),
        sizeof(world)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 44,
                     {device, 256 /* D3DTS_WORLD */,
                      static_cast<u32>(SCRATCH_ADDR + 0x800)}),
              D3D_OK);
    const std::array<float, 4> plane{1.0f, 0.0f, 0.0f, -0.25f};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x900, reinterpret_cast<const Byte*>(plane.data()),
        sizeof(plane)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 55,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x900)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 152 /* CLIPPLANEENABLE */, 1}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float clip_distances [[clip_distance]] [6];"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "clip_world_position = float4(in.position, 1.0) * cb.world"),
              std::string::npos);
    const std::vector<u8>& constants =
        backend_.record.draw_vertex_constants.front();
    ASSERT_GE(constants.size(), 320u + 6u * 16u);
    std::array<float, 16> packed_world{};
    std::memcpy(packed_world.data(), constants.data(), sizeof(packed_world));
    EXPECT_FLOAT_EQ(packed_world[3], 0.25f);
    std::array<float, 4> packed{};
    std::memcpy(packed.data(), constants.data() + 320, sizeof(packed));
    EXPECT_EQ(packed, plane);
    std::memcpy(packed.data(), constants.data() + 336, sizeof(packed));
    EXPECT_EQ(packed, (std::array<float, 4>{0, 0, 0, 0}));
}

TEST_F(D3D9ManagerTest, FixedFunctionXyzb1BlendsWorldZeroAndWorldOne) {
    const auto [factory, device] = create_device();
    (void)bind_triangle_stream(device, 0x006 /* XYZB1 */ | 0x040 /* DIFFUSE */);

    std::array<float, 16> world0{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        -0.5f, 0.0f, 0.0f, 1.0f,
    };
    std::array<float, 16> world1 = world0;
    world1[12] = 0.5f;
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x800, reinterpret_cast<const Byte*>(world0.data()),
        sizeof(world0)).is_ok());
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x840, reinterpret_cast<const Byte*>(world1.data()),
        sizeof(world1)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 44,
                     {device, 256 /* WORLD0 */,
                      static_cast<u32>(SCRATCH_ADDR + 0x800)}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 44,
                     {device, 257 /* WORLD1 */,
                      static_cast<u32>(SCRATCH_ADDR + 0x840)}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 151 /* VERTEXBLEND */, 1 /* 1WEIGHTS */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 3u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::Float1);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 16u);
    EXPECT_EQ(desc.vertex_stride, 20u);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float blend_weights [[attribute(1)]];"), std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "blend_weight1 = 1.0 - blend_weight0"), std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "blend_weight0 * (object_position * cb.world)"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "blend_weight1 * (object_position * cb.world1)"),
              std::string::npos);

    const auto& constants = backend_.record.draw_vertex_constants.front();
    ASSERT_GE(constants.size(), 448u);
    float packed_world1_translation = 0.0f;
    std::memcpy(&packed_world1_translation, constants.data() + 320u + 12u,
                sizeof(packed_world1_translation));
    EXPECT_FLOAT_EQ(packed_world1_translation, 0.5f);
}

TEST_F(D3D9ManagerTest, FixedFunctionXyzb3UsesThreeExplicitWeights) {
    const auto [factory, device] = create_device();
    (void)bind_triangle_stream(device, 0x00A /* XYZB3 */ | 0x040 /* DIFFUSE */);
    for (u32 i = 0; i < 4; ++i) {
        std::array<float, 16> world{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            static_cast<float>(i), 0.0f, 0.0f, 1.0f,
        };
        ASSERT_TRUE(memory_.write_bytes(
            SCRATCH_ADDR + 0x800, reinterpret_cast<const Byte*>(world.data()),
            sizeof(world)).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 44,
                         {device, 256u + i,
                          static_cast<u32>(SCRATCH_ADDR + 0x800)}), D3D_OK);
    }
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 151 /* VERTEXBLEND */, 3 /* 3WEIGHTS */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 3u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::Float3);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 24u);
    EXPECT_EQ(desc.vertex_stride, 28u);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float3 blend_weights [[attribute(1)]];"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "float blend_weight3 = 1.0 - blend_weight0 - "
                  "blend_weight1 - blend_weight2;"), std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "blend_weight3 * (object_position * cb.world3)"),
              std::string::npos);
    const auto& constants = backend_.record.draw_vertex_constants.front();
    ASSERT_GE(constants.size(), 704u);
}

TEST_F(D3D9ManagerTest, FixedFunctionXyzb2AndXyzb4LayoutsAreAccepted) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);

    struct Case {
        u32 fvf;
        u32 requested_weights;
        graphics::BackendVertexFormat format;
        u32 color_offset;
        u32 stride;
    };
    const Case cases[] = {
        {0x008u | 0x040u, 2u, graphics::BackendVertexFormat::Float2, 20u, 24u},
        // XYZB4 carries four beta floats, but D3DVBF_3WEIGHTS consumes the
        // first three and derives WORLD3's weight as 1-(b0+b1+b2).
        {0x00Cu | 0x040u, 3u, graphics::BackendVertexFormat::Float4, 28u, 32u},
    };
    for (const Case& test : cases) {
        (void)bind_triangle_stream(device, test.fvf);
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                         {device, 151 /* VERTEXBLEND */,
                          test.requested_weights}), D3D_OK);
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                         {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);
        const auto& desc = backend_.record.last_pipeline_desc;
        ASSERT_EQ(desc.vertex_attributes.size(), 3u);
        EXPECT_EQ(desc.vertex_attributes[1].format, test.format);
        EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
        EXPECT_EQ(desc.vertex_attributes[2].offset, test.color_offset);
        EXPECT_EQ(desc.vertex_stride, test.stride);
    }
}

TEST_F(D3D9ManagerTest, FixedFunctionIndexedBlendUsesUbyte4PaletteIndices) {
    const auto [factory, device] = create_device();
    (void)bind_triangle_stream(device,
                               0x008 /* XYZB2 */ | 0x1000 /* LASTBETA_UBYTE4 */ |
                                   0x040 /* DIFFUSE */);
    for (u32 i : {2u, 8u}) {
        std::array<float, 16> world{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            static_cast<float>(i), 0.0f, 0.0f, 1.0f,
        };
        ASSERT_TRUE(memory_.write_bytes(
            SCRATCH_ADDR + 0x800, reinterpret_cast<const Byte*>(world.data()),
            sizeof(world)).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 44,
                         {device, 256u + i,
                          static_cast<u32>(SCRATCH_ADDR + 0x800)}), D3D_OK);
    }
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 151 /* VERTEXBLEND */, 1 /* 1WEIGHTS */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 167 /* INDEXEDVERTEXBLENDENABLE */, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 4u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::Float1);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_attributes[2].format,
              graphics::BackendVertexFormat::UByte4);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 16u);
    EXPECT_EQ(desc.vertex_attributes[3].offset, 20u);
    EXPECT_EQ(desc.vertex_stride, 24u);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "uchar4 blend_indices [[attribute(2)]];"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "cb.blend_worlds[uint(in.blend_indices.y)]"),
              std::string::npos);

    const auto& constants = backend_.record.draw_vertex_constants.front();
    ASSERT_GE(constants.size(), 1472u);
    float world8_translation = 0.0f;
    // Five base matrices precede the nine WORLD palette matrices.
    std::memcpy(&world8_translation,
                constants.data() + 320u + 8u * 64u + 12u,
                sizeof(world8_translation));
    EXPECT_FLOAT_EQ(world8_translation, 8.0f);
}

TEST_F(D3D9ManagerTest, FixedFunctionIndexedZeroWeightsUsesOnePaletteIndex) {
    const auto [factory, device] = create_device();
    (void)bind_triangle_stream(device,
                               0x006 /* XYZB1 */ | 0x1000 /* LASTBETA_UBYTE4 */ |
                                   0x040 /* DIFFUSE */);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 151 /* VERTEXBLEND */, 256 /* 0WEIGHTS */}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 167 /* INDEXEDVERTEXBLENDENABLE */, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 137 /* LIGHTING */, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}), D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 3u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::UByte4);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_attributes[2].offset, 16u);
    EXPECT_EQ(desc.vertex_stride, 20u);
    EXPECT_NE(desc.vertex_shader_msl.find("float blend_weight0 = 1.0;"),
              std::string::npos);
    EXPECT_NE(desc.vertex_shader_msl.find(
                  "cb.blend_worlds[uint(in.blend_indices.x)]"),
              std::string::npos);
}
