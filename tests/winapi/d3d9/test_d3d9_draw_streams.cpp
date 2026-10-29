/// @file test_d3d9_draw_streams.cpp
/// @brief Draws from user memory, stream-source frequency and stream setup.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;
using vitreo::winapi::d3d9_test::test_f32_bits;

TEST_F(D3D9ManagerTest, DrawPrimitiveUPSubmitsFromUserMemory) {
    // Learn: DrawPrimitiveUP — stream-0 user pointer, then stream 0 is NULL.
    const auto [factory, device] = create_device();
    constexpr u32 fvf = 0x002 | 0x040; // XYZ | DIFFUSE
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, fvf}), D3D_OK);

    // Three vertices of a triangle (stride 16).
    alignas(4) u8 verts[48]{};
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x100, verts, sizeof(verts))
                    .is_ok());

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 83,
                     {device, 4 /* TRIANGLELIST */, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x100), 16}),
              D3D_OK);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_EQ(backend_.record.draw_calls.front().vertex_count, 3u);

    // Stream 0 must be cleared after UP.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 101,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 4),
                      static_cast<u32>(SCRATCH_ADDR + 8)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
}

TEST_F(D3D9ManagerTest, DrawIndexedPrimitiveUPSubmitsAndClearsIndices) {
    // Learn: DrawIndexedPrimitiveUP — also resets the index buffer binding.
    const auto [factory, device] = create_device();
    constexpr u32 fvf = 0x002 | 0x040;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, fvf}), D3D_OK);

    alignas(4) u8 verts[48]{};
    const u16 indices[3] = {0, 1, 2};
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x100, verts, sizeof(verts))
                    .is_ok());
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x200,
                                 reinterpret_cast<const Byte*>(indices),
                                 sizeof(indices))
                    .is_ok());

    // Bind a dummy index buffer first so we can observe the post-UP clear.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 6, 0, D3DFMT_INDEX16, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 ib = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(ib, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 104, {device, ib}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 84,
                     {device, 4 /* TRIANGLELIST */, 0, 3, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), D3DFMT_INDEX16,
                      static_cast<u32>(SCRATCH_ADDR + 0x100), 16}),
              D3D_OK);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_EQ(backend_.record.draw_calls.front().index_count, 3u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 105,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x10)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x10).value(), 0u);
}

TEST_F(D3D9ManagerTest, DrawPrimitiveUPIgnoresExtraStreamBindings) {
    // Learn: DrawPrimitiveUP is a single stream-0 user-memory draw. Multi-stream
    // device state left from SetStreamSource(1+) must not reject the UP call;
    // only stream 0 is replaced for the draw and cleared afterward.
    const auto [factory, device] = create_device();
    constexpr u32 fvf = 0x002 | 0x040; // XYZ | DIFFUSE
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, fvf}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 32, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x40), 0}),
              D3D_OK);
    const u32 stream1_vb = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_NE(stream1_vb, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 1, stream1_vb, 0, 8}),
              D3D_OK);

    alignas(4) u8 verts[48]{};
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x100, verts, sizeof(verts))
                    .is_ok());

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 83,
                     {device, 4 /* TRIANGLELIST */, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x100), 16}),
              D3D_OK);
    EXPECT_EQ(backend_.record.draws, 1u);
    EXPECT_EQ(backend_.record.draw_calls.front().vertex_count, 3u);

    // Stream 0 cleared; stream 1 binding remains.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 101,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 4),
                      static_cast<u32>(SCRATCH_ADDR + 8)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 101,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR + 0x10),
                      static_cast<u32>(SCRATCH_ADDR + 0x14),
                      static_cast<u32>(SCRATCH_ADDR + 0x18)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x10).value(), stream1_vb);
}

TEST_F(D3D9ManagerTest, DrawPrimitiveUPRejectsMultiStreamDeclaration) {
    // Learn: UP forbids a vertex declaration that refers to streams other
    // than stream 0.
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
        {0, 0, 2, 0, 0, 0},
        {1, 0, 1, 0, 5, 0},
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
                    SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
                    sizeof(elements))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87, {device, declaration}),
              D3D_OK);

    alignas(4) u8 verts[48]{};
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR + 0x200, verts, sizeof(verts))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 83,
                     {device, 4 /* TRIANGLELIST */, 1,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), 12}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(backend_.record.draws, 0u);
}

TEST_F(D3D9ManagerTest, SetStreamSourceFreqRoundTrip) {
    // Learn: Set/GetStreamSourceFreq store the divider / instance flags.
    const auto [factory, device] = create_device();
    constexpr u32 indexed_data = D3DSTREAMSOURCE_INDEXEDDATA;
    constexpr u32 instance_data = D3DSTREAMSOURCE_INSTANCEDATA;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 102,
                     {device, 0, indexed_data | 10}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 102,
                     {device, 1, instance_data | 1}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 103,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), indexed_data | 10);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 103,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR + 4)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 4).value(), instance_data | 1);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 102, {device, 16, 1}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, DrawIndexedPrimitiveHonorsStreamSourceFreqInstancing) {
    // Learn: SetStreamSourceFreq — INDEXEDDATA on stream 0 is the instance
    // count for DrawIndexedPrimitive; INSTANCEDATA marks a per-instance
    // stream with the given step rate.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    // Stream 0: FLOAT3 POSITION. Stream 1: FLOAT2 instance TEXCOORD.
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},
        {1, 0, 1, 0, 5, 0},
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_
                    .write_bytes(SCRATCH_ADDR,
                                 reinterpret_cast<const Byte*>(elements),
                                 sizeof(elements))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87, {device, declaration}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 12 * 3, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), 0}),
              D3D_OK);
    const u32 positions = memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_NE(positions, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 8 * 4, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x204), 0}),
              D3D_OK);
    const u32 instances = memory_.read_u32(SCRATCH_ADDR + 0x204).value();
    ASSERT_NE(instances, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 64, 0, D3DFMT_INDEX16, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x208), 0}),
              D3D_OK);
    const u32 index_buffer = memory_.read_u32(SCRATCH_ADDR + 0x208).value();
    ASSERT_NE(index_buffer, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, positions, 0, 12}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 1, instances, 0, 8}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 104, {device, index_buffer}),
              D3D_OK);

    constexpr u32 instance_count = 4;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 102,
                     {device, 0, D3DSTREAMSOURCE_INDEXEDDATA | instance_count}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 102,
                     {device, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 82,
                     {device, 4 /* TRIANGLELIST */, 0, 0, 3, 0, 1}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_EQ(call.instance_count, instance_count);
    EXPECT_EQ(call.index_count, 3u);
    EXPECT_NE(call.vertex_buffers[0], nullptr);
    EXPECT_NE(call.vertex_buffers[1], nullptr);

    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_FALSE(desc.vertex_buffer_per_instance[0]);
    EXPECT_TRUE(desc.vertex_buffer_per_instance[1]);
    EXPECT_EQ(desc.vertex_buffer_step_rates[1], 1u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

// =============================================================================
// Fixed-function drawing
// =============================================================================

TEST_F(D3D9ManagerTest, DrawPrimitiveMultiStreamVertexDeclaration) {
    // Learn: programming one or more streams — SetStreamSource per stream,
    // declaration elements name Stream; DrawPrimitive assembles from all.
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    // Stream 0: FLOAT3 POSITION @0. Stream 1: FLOAT2 TEXCOORD @0.
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},
        {1, 0, 1, 0, 5, 0},
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
                    SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
                    sizeof(elements))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87, {device, declaration}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 12 * 3, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), 0}),
              D3D_OK);
    const u32 positions = memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_NE(positions, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 8 * 3, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x204), 0}),
              D3D_OK);
    const u32 texcoords = memory_.read_u32(SCRATCH_ADDR + 0x204).value();
    ASSERT_NE(texcoords, 0u);

    // Stream 1 missing → INVALIDCALL.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, positions, 0, 12}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3DERR_INVALIDCALL);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 1, texcoords, 0, 8}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_NE(call.vertex_buffer, nullptr);
    EXPECT_EQ(call.vertex_buffers[0], call.vertex_buffer);
    EXPECT_NE(call.vertex_buffers[1], nullptr);
    EXPECT_NE(call.vertex_buffers[0], call.vertex_buffers[1]);

    const auto& desc = backend_.record.last_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].format,
              graphics::BackendVertexFormat::Float3);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[0].buffer_index, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::Float2);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].buffer_index, 1u);
    EXPECT_EQ(desc.vertex_stride, 12u);
    EXPECT_EQ(desc.vertex_buffer_strides[0], 12u);
    EXPECT_EQ(desc.vertex_buffer_strides[1], 8u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, DrawAllowsStreamStrideGreaterThanDeclarationSize) {
    // Learn (SetStreamSource): with a vertex declaration, stride may be >=
    // the stream size computed from the declaration (padding between verts).
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    struct Element {
        u16 stream;
        u16 offset;
        u8 type;
        u8 method;
        u8 usage;
        u8 usage_index;
    };
    // Stream 0: FLOAT3 POSITION @0 → declaration size 12.
    const Element elements[] = {
        {0, 0, 2, 0, 0, 0},
        {0xFF, 0, 0, 0, 0, 0},
    };
    ASSERT_TRUE(memory_.write_bytes(
                    SCRATCH_ADDR, reinterpret_cast<const Byte*>(elements),
                    sizeof(elements))
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 86,
                     {device, static_cast<u32>(SCRATCH_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    const u32 declaration = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(declaration, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 87, {device, declaration}),
              D3D_OK);

    constexpr u32 padded_stride = 16;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, padded_stride * 3, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x200), 0}),
              D3D_OK);
    const u32 positions = memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_NE(positions, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 100,
                     {device, 0, positions, 0, padded_stride}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    ASSERT_EQ(backend_.record.draws, 1u);
    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_EQ(desc.vertex_stride, padded_stride);
    EXPECT_EQ(desc.vertex_buffer_strides[0], padded_stride);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
}

TEST_F(D3D9ManagerTest, DrawPrimitiveBuildsAPipelineAndSubmitsAThroughDraw) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    ASSERT_NE(vertex_buffer, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);

    EXPECT_EQ(backend_.record.pipelines_created, 1u);
    ASSERT_EQ(backend_.record.draws, 1u);
    const graphics::BackendDrawCall& call = backend_.record.draw_calls.front();
    EXPECT_EQ(call.topology, graphics::BackendPrimitiveTopology::TriangleList);
    EXPECT_EQ(call.vertex_count, 3u);
    EXPECT_EQ(call.index_buffer, nullptr);
    EXPECT_NE(call.vertex_buffer, nullptr);
    EXPECT_NE(call.vertex_constants, nullptr);
    EXPECT_EQ(call.viewport[2], 640.0f);
    EXPECT_EQ(call.viewport[3], 480.0f);

    // The generated shaders are what the backend was asked to compile.
    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_EQ(desc.vertex_entry_point, "ff_vertex");
    EXPECT_EQ(desc.fragment_entry_point, "ff_fragment");
    EXPECT_NE(desc.vertex_shader_msl.find("ff_vertex"), std::string::npos);
    EXPECT_NE(desc.fragment_shader_msl.find("ff_fragment"),
              std::string::npos);
    // XYZ|DIFFUSE is a float3 position plus a packed colour.
    ASSERT_EQ(desc.vertex_attributes.size(), 2u);
    EXPECT_EQ(desc.vertex_attributes[0].format,
              graphics::BackendVertexFormat::Float3);
    EXPECT_EQ(desc.vertex_attributes[0].offset, 0u);
    EXPECT_EQ(desc.vertex_attributes[1].format,
              graphics::BackendVertexFormat::UByte4NormalizedBGRA);
    EXPECT_EQ(desc.vertex_attributes[1].offset, 12u);
    EXPECT_EQ(desc.vertex_stride, 16u);

    // EndScene submits the pass.
    EXPECT_EQ(backend_.record.render_passes_begun, 1u);
    EXPECT_EQ(backend_.record.render_passes_ended, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);
    EXPECT_EQ(backend_.record.render_passes_ended, 1u);
}

TEST_F(D3D9ManagerTest, DisablingScissorRestoresTheFullTargetForTheDraw) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device), 0u);

    const std::array<i32, 4> small_scissor{100, 80, 220, 180};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x300,
        reinterpret_cast<const Byte*>(small_scissor.data()),
        sizeof(small_scissor)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 75,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x300)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 174 /* SCISSORTESTENABLE */, 1}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);
    ASSERT_EQ(backend_.record.draw_calls.size(), 1u);
    EXPECT_TRUE(backend_.record.draw_calls[0].scissor_enabled);
    EXPECT_EQ(backend_.record.draw_calls[0].scissor, small_scissor);

    // A disabled D3D scissor test means no scissor, even when the viewport is
    // smaller than the render target. The native backend must neutralize its
    // persistent scissor with the full target rather than the viewport.
    const std::array<u32, 6> small_viewport{
        40, 30, 320, 240, test_f32_bits(0.0f), test_f32_bits(1.0f)};
    ASSERT_TRUE(memory_.write_bytes(
        SCRATCH_ADDR + 0x340,
        reinterpret_cast<const Byte*>(small_viewport.data()),
        sizeof(small_viewport)).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 47,
                     {device, static_cast<u32>(SCRATCH_ADDR + 0x340)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 57,
                     {device, 174 /* SCISSORTESTENABLE */, 0}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);
    ASSERT_EQ(backend_.record.draw_calls.size(), 2u);
    EXPECT_FALSE(backend_.record.draw_calls[1].scissor_enabled);
    EXPECT_EQ(backend_.record.draw_calls[1].scissor,
              (std::array<i32, 4>{0, 0, 640, 480}));
}

TEST_F(D3D9ManagerTest,
       NullTextureInColorArg1TerminatesFixedFunctionStageCascade) {
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device), 0u);

    // Stage zero defaults to MODULATE with COLORARG1=D3DTA_TEXTURE. With no
    // texture bound, Direct3D processes neither this stage nor any later one.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, 0, 0}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4, 0, 1}),
              D3D_OK);

    const auto& desc = backend_.record.last_pipeline_desc;
    EXPECT_EQ(desc.fragment_shader_msl.find("texture2d<float> tex0"),
              std::string::npos);
    EXPECT_NE(desc.fragment_shader_msl.find("float4 current = v_diffuse"),
              std::string::npos);
    ASSERT_EQ(backend_.record.draw_calls.size(), 1u);
    EXPECT_EQ(backend_.record.draw_calls[0].textures[0], nullptr);
}
