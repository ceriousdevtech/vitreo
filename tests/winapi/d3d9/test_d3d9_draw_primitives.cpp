/// @file test_d3d9_draw_primitives.cpp
/// @brief Pipeline reuse, indexed draws and triangle fans.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, RepeatedDrawsWithTheSameStateReuseOnePipeline) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
                  D3D_OK);
    }
    EXPECT_EQ(backend_.record.draws, 4u);
    EXPECT_EQ(backend_.record.pipelines_created, 1u);
    // All four draws share one render pass, so the frame is one submission.
    EXPECT_EQ(backend_.record.render_passes_begun, 1u);
    ASSERT_EQ(backend_.record.draw_vertex_constant_pointers.size(), 4u);
    ASSERT_EQ(backend_.record.draw_fragment_constant_pointers.size(), 4u);
    for (usize i = 1; i < 4; ++i) {
        EXPECT_EQ(backend_.record.draw_vertex_constant_pointers[i],
                  backend_.record.draw_vertex_constant_pointers[0]);
        EXPECT_EQ(backend_.record.draw_fragment_constant_pointers[i],
                  backend_.record.draw_fragment_constant_pointers[0]);
        EXPECT_EQ(backend_.record.draw_vertex_constants[i],
                  backend_.record.draw_vertex_constants[0]);
        EXPECT_EQ(backend_.record.draw_fragment_constants[i],
                  backend_.record.draw_fragment_constants[0]);
    }
}

TEST_F(D3D9ManagerTest, SetLightParameterChangeReusesFixedFunctionPipeline) {
    // SetLight updates D3DLIGHT9 constants. The generated FF program is
    // keyed by light type, not by camera-moving position/direction.
    const auto [factory, device] = create_device();
    (void)factory;
    ASSERT_NE(bind_triangle_stream(device, 0x002 | 0x010 | 0x040), 0u);

    std::array<u8, 104> light{};
    u32 type = 3; // D3DLIGHT_DIRECTIONAL
    std::memcpy(light.data(), &type, sizeof(type));
    const float diffuse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float direction_a[3] = {0.0f, 0.0f, 1.0f};
    std::memcpy(light.data() + 4, diffuse, sizeof(diffuse));
    std::memcpy(light.data() + 64, direction_a, sizeof(direction_a));
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR,
                                 reinterpret_cast<const Byte*>(light.data()),
                                 light.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 51,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 53, {device, 0, 1}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(backend_.record.pipelines_created, 1u);

    const float direction_b[3] = {0.6f, 0.0f, 0.8f};
    std::memcpy(light.data() + 64, direction_b, sizeof(direction_b));
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR,
                                 reinterpret_cast<const Byte*>(light.data()),
                                 light.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 51,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.pipelines_created, 1u);
    EXPECT_EQ(backend_.record.draws, 2u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, ChangingStateThatAffectsShadersBuildsANewPipeline) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(backend_.record.pipelines_created, 1u);

    // D3DRS_ALPHABLENDENABLE changes the pipeline, not the shader.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57, {device, 27, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.pipelines_created, 2u);
    EXPECT_TRUE(backend_.record.last_pipeline_desc.blend_enabled);
}

TEST_F(D3D9ManagerTest, DrawPreservesIndependentMrtColorWriteMasks) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)factory;
    (void)vertex_buffer;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57, {device, 168, 0x1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57, {device, 190, 0x2}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57, {device, 191, 0x4}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57, {device, 192, 0x8}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);

    EXPECT_EQ(backend_.record.last_pipeline_desc.color_write_mask, 0x1u);
    EXPECT_EQ(backend_.record.last_pipeline_desc.color_write_masks[0], 0x1u);
    EXPECT_EQ(backend_.record.last_pipeline_desc.color_write_masks[1], 0x2u);
    EXPECT_EQ(backend_.record.last_pipeline_desc.color_write_masks[2], 0x4u);
    EXPECT_EQ(backend_.record.last_pipeline_desc.color_write_masks[3], 0x8u);
}

TEST_F(D3D9ManagerTest, DrawIndexedPrimitiveUsesTheBoundIndexBuffer) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 64, 0, D3DFMT_INDEX16, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x300), 0}),
              D3D_OK);
    const u32 index_buffer = memory_.read_u32(SCRATCH_ADDR + 0x300).value();
    ASSERT_NE(index_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 104, {device, index_buffer}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    // BaseVertexIndex 2, MinIndex 0, NumVertices 3, StartIndex 3, 1 triangle.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 82,
                     {device, 4, 2, 0, 3, 3, 1}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_NE(call.index_buffer, nullptr);
    EXPECT_FALSE(call.index_32bit);
    EXPECT_EQ(call.index_count, 3u);
    EXPECT_EQ(call.base_vertex, 2);
    // StartIndex is a count of indices; the backend takes a byte offset.
    EXPECT_EQ(call.index_buffer_offset, 6u);
}

TEST_F(D3D9ManagerTest, DrawIsRejectedOutsideASceneAndWithoutAStream) {
    const auto [factory, device] = create_device();

    // Outside BeginScene/EndScene.
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3DERR_INVALIDCALL);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    // Zero primitives.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 0}),
              D3DERR_INVALIDCALL);
    // Detach the stream.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100, {device, 0, 0, 0, 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.draws, 0u);
}

TEST_F(D3D9ManagerTest, DrawRejectsAStrideThatContradictsTheFvf) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    // XYZ|DIFFUSE is 16 bytes; claim 20.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, vertex_buffer, 0, 20}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.draws, 0u);
}

TEST_F(D3D9ManagerTest, DrawPrimitiveTriangleFanExpandsToIndexedTriangleList) {
    // Learn: D3DPT_TRIANGLEFAN — Metal has no fan topology, so the draw path
    // rewrites (v0,v1,v2), (v0,v2,v3), … into a temporary index buffer and
    // submits as an indexed triangle list. Device index state is restored.
    const auto [factory, device] = create_device();
    constexpr u32 fvf = 0x002 | 0x040; // XYZ | DIFFUSE
    constexpr u32 stride = 16;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, stride * 4, 0, fvf, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x600), 0}),
              D3D_OK);
    const u32 vertex_buffer = memory_.read_u32(SCRATCH_ADDR + 0x600).value();
    ASSERT_NE(vertex_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, fvf}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, vertex_buffer, 0, stride}),
              D3D_OK);

    // Bind a dummy IB so we can observe that SetIndices is restored after fan
    // expansion (the non-indexed fan path must not leave a temp IB bound).
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 64, 0, D3DFMT_INDEX16, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x300), 0}),
              D3D_OK);
    const u32 bound_ib = memory_.read_u32(SCRATCH_ADDR + 0x300).value();
    ASSERT_NE(bound_ib, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 104, {device, bound_ib}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    // StartVertex 1, two fan triangles → six list indices, base_vertex 1.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 6 /* TRIANGLEFAN */, 1, 2}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_EQ(call.topology, graphics::BackendPrimitiveTopology::TriangleList);
    EXPECT_NE(call.index_buffer, nullptr);
    EXPECT_FALSE(call.index_32bit);
    EXPECT_EQ(call.index_count, 6u);
    EXPECT_EQ(call.index_buffer_offset, 0u);
    EXPECT_EQ(call.base_vertex, 1);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 105,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x10)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x10).value(), bound_ib);
}

TEST_F(D3D9ManagerTest, DrawIndexedPrimitiveTriangleFanExpandsSourceIndices) {
    // Learn: indexed fan expands the bound IB range the same way, keeping
    // BaseVertexIndex and restoring SetIndices afterward.
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 16, 0, D3DFMT_INDEX16, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x300), 0}),
              D3D_OK);
    const u32 index_buffer = memory_.read_u32(SCRATCH_ADDR + 0x300).value();
    ASSERT_NE(index_buffer, 0u);

    // Fan indices at StartIndex 1: apex=2, then 0,1,0 → two triangles
    // (2,0,1) and (2,1,0).
    const u16 indices[5] = {0xFFFF, 2, 0, 1, 0};
    ASSERT_EQ(invoke(D3D9InterfaceId::IndexBuffer, 11,
                     {index_buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x20),
                      0}),
              D3D_OK);
    const GuestAddr locked = memory_.read_u32(SCRATCH_ADDR + 0x20).value();
    ASSERT_NE(locked, 0u);
    ASSERT_TRUE(memory_
                    .write_bytes(locked, reinterpret_cast<const Byte*>(indices),
                                 sizeof(indices))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::IndexBuffer, 12, {index_buffer}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 104, {device, index_buffer}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 82,
                     {device, 6 /* TRIANGLEFAN */, 3 /* BaseVertexIndex */,
                      0, 3, 1 /* StartIndex */, 2}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_EQ(call.topology, graphics::BackendPrimitiveTopology::TriangleList);
    EXPECT_NE(call.index_buffer, nullptr);
    EXPECT_FALSE(call.index_32bit);
    EXPECT_EQ(call.index_count, 6u);
    EXPECT_EQ(call.index_buffer_offset, 0u);
    EXPECT_EQ(call.base_vertex, 3);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 105,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x10)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x10).value(), index_buffer);
}
