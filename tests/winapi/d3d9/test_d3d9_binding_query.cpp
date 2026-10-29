/// @file test_d3d9_binding_query.cpp
/// @brief Texture binding, queries and device release.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, SetTextureValidatesTheInterfaceAndRoundTrips) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();

    // A device pointer is not a texture.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, device}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, 16u, texture}),
              D3DERR_INVALIDCALL);

    for (u32 sampler : {0u, 8u, 15u}) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, sampler, texture}),
                  D3D_OK);
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 64,
                         {device, sampler, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
                  D3D_OK);
        EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), texture);
    }
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 67, {device, 8, 1, 1}), D3DERR_INVALIDCALL);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 59,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR + 0x200)}), D3D_OK);
    const auto block = memory_.read_u32(SCRATCH_ADDR + 0x200).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 15, 0}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::StateBlock, 5, {block}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 64,
                     {device, 15, static_cast<u32>(SCRATCH_ADDR + 0x100)}), D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), texture);
}

TEST_F(D3D9ManagerTest,
       VertexTextureSamplerStateRoundTripsAndOwnsIndependentReferences) {
    const auto [factory, device] = create_device();
    constexpr u32 D3DVERTEXTEXTURESAMPLER0 = 257;
    constexpr u32 D3DVERTEXTEXTURESAMPLER1 = 258;
    constexpr u32 D3DDMAPSAMPLER = 256;

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture_a = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, 21, 1,
                      static_cast<u32>(SCRATCH_ADDR + 4), 0}),
              D3D_OK);
    const u32 texture_b = memory_.read_u32(SCRATCH_ADDR + 4).value();

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, texture_a}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER0, texture_a}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER1, texture_b}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, D3DVERTEXTEXTURESAMPLER0, 6, 2}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 68,
                     {device, D3DVERTEXTEXTURESAMPLER0, 6,
                      static_cast<u32>(SCRATCH_ADDR + 0x108)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x108).value(), 2u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DDMAPSAMPLER, texture_a}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 261, texture_a}),
              D3DERR_INVALIDCALL);

    // Drop the caller references. Pixel slot 0 and vertex slot 257 each own
    // one independent reference to A; vertex slot 258 owns one to B.
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), 2u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_b}), 1u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 64,
                     {device, D3DVERTEXTEXTURESAMPLER0,
                      static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), texture_a);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), 2u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER0, 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 64,
                     {device, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x104)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), texture_a);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 2, {texture_a}), 1u);

    // Clearing one namespace must not affect the other, and clearing the last
    // binding releases the final object reference.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65, {device, 0, 0}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_a}), 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER1, 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 1, {texture_b}), 0u);
}

TEST_F(D3D9ManagerTest, VertexShaderTexldlBindsTextureToVertexBackendStage) {
    const auto [factory, device] = create_device();
    constexpr u32 D3DVERTEXTEXTURESAMPLER0 = 257;
    auto reg = [](u32 type) {
        return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
    };
    constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
    const std::vector<u32> tokens{
        0xFFFE0300u,
        31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u,
        31u | (2u << 24), (2u << 27), DST_MASK | reg(10) | 0u,
        95u | (3u << 24), DST_MASK | reg(0) | 0u,
        0x80000000u | reg(1) | 0u | (0xE4u << 16),
        0x80000000u | reg(10) | 0u | (0xE4u << 16),
        1u | (2u << 24), DST_MASK | reg(4) | 0u,
        0x80000000u | reg(0) | 0u | (0xE4u << 16),
        0x0000FFFFu,
    };
    write_shader_bytecode(SHADER_ADDR, tokens);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 91,
                     {device, static_cast<u32>(SHADER_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0x820)}),
              D3D_OK);
    const u32 shader = memory_.read_u32(SCRATCH_ADDR + 0x820).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 92, {device, shader}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                      static_cast<u32>(SCRATCH_ADDR + 0x824), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR + 0x824).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 65,
                     {device, D3DVERTEXTEXTURESAMPLER0, texture}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 69,
                     {device, D3DVERTEXTEXTURESAMPLER0, 6, 2}), D3D_OK);
    ASSERT_NE(bind_triangle_stream(device, 0x002), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);

    ASSERT_FALSE(backend_.record.draw_calls.empty());
    const auto& call = backend_.record.draw_calls.back();
    EXPECT_NE(call.vertex_textures[0], nullptr);
    EXPECT_EQ(call.textures[0], nullptr);
    EXPECT_EQ(call.vertex_samplers[0].min_filter,
              graphics::BackendFilter::Linear);
}

TEST_F(D3D9ManagerTest, StreamSourceRequiresAStrideExactlyWhenABufferIsSet) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100, {device, 0, buffer, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100, {device, 0, 0, 0, 16}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100, {device, 0, buffer, 8, 16}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 101,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      static_cast<u32>(SCRATCH_ADDR + 0x104),
                      static_cast<u32>(SCRATCH_ADDR + 0x108)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), buffer);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), 8u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x108).value(), 16u);
}

TEST_F(D3D9ManagerTest, CreateQueryEventIssueAndGetData) {
    // Learn: CreateQuery(EVENT, NULL) probes support; Issue(END) + GetData
    // returns BOOL TRUE when the GPU has finished work through the Issue.
    const auto [factory, device] = create_device();
    constexpr u32 D3DQUERYTYPE_EVENT = 8;
    constexpr u32 D3DISSUE_END = 1u << 0;

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_EVENT, 0}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_EVENT,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 query = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(query, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 4, {query}), D3DQUERYTYPE_EVENT);
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 5, {query}), 4u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 6, {query, D3DISSUE_END}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7,
                     {query, static_cast<u32>(SCRATCH_ADDR + 0x10), 4, 1}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x10).value(), 1u);
}

TEST_F(D3D9ManagerTest, CreateQuerySupportsEventOnlyOcclusionUnavailable) {
    // Learn (D3DQUERYTYPE): EVENT=8, OCCLUSION=9, VCACHE=4, TIMESTAMP=10.
    // CreateQuery(Type, NULL) probes support. Vitreo only implements EVENT;
    // occlusion stays D3DERR_NOTAVAILABLE rather than faking a pixel count.
    const auto [factory, device] = create_device();
    constexpr u32 D3DQUERYTYPE_VCACHE = 4;
    constexpr u32 D3DQUERYTYPE_EVENT = 8;
    constexpr u32 D3DQUERYTYPE_OCCLUSION = 9;
    constexpr u32 D3DQUERYTYPE_TIMESTAMP = 10;

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_EVENT, 0}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_OCCLUSION, 0}),
              D3DERR_NOTAVAILABLE);
    memory_.write_u32(SCRATCH_ADDR, 0xDEADBEEFu);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_OCCLUSION,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_VCACHE, 0}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_TIMESTAMP, 0}),
              D3DERR_NOTAVAILABLE);
}

TEST_F(D3D9ManagerTest, ReleasingTheDeviceDestroysTheObjectsItOwns) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 back_buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(back_buffer, 0u);
    const u32 device_vtable = memory_.read_u32(device).value();
    const u32 back_buffer_vtable = memory_.read_u32(back_buffer).value();
    // GetBackBuffer added a reference on top of the device's own.
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 2, {back_buffer}), 1u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 2, {device}), 0u);
    // Destruction removes registry ownership while retaining the IUnknown
    // thunks, so stale Release calls fail safely rather than jumping via 8.
    EXPECT_EQ(memory_.read_u32(device).value(), device_vtable);
    EXPECT_EQ(memory_.read_u32(back_buffer).value(), back_buffer_vtable);
    // Calls on the destroyed pointers no longer resolve.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 3, {device}), D3DERR_INVALIDCALL);
}
