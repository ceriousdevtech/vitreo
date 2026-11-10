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

// Expected values: tools/oracle_probe/d3d9_query_probe{32,64}
// .winoracle-gpu.expected (RTX 4080 HAL device) and Microsoft Learn
// IDirect3DQuery9 / D3DQUERYTYPE.
namespace {
constexpr u32 D3DQUERYTYPE_VCACHE = 4;
constexpr u32 D3DQUERYTYPE_EVENT = 8;
constexpr u32 D3DQUERYTYPE_OCCLUSION = 9;
constexpr u32 D3DQUERYTYPE_TIMESTAMP = 10;
constexpr u32 D3DQUERYTYPE_TIMESTAMPDISJOINT = 11;
constexpr u32 D3DQUERYTYPE_TIMESTAMPFREQ = 12;
constexpr u32 D3DQUERYTYPE_MEMORYPRESSURE = 19;
constexpr u32 D3DISSUE_END = 1u << 0;
constexpr u32 D3DISSUE_BEGIN = 1u << 1;
constexpr u32 D3DGETDATA_FLUSH = 1u << 0;
constexpr u32 S_FALSE_TEST = 1;
}  // namespace

TEST_F(D3D9ManagerTest, CreateQuerySupportMatchesTheMeasuredTypes) {
    const auto [factory, device] = create_device();
    for (u32 type = 0; type <= 20; ++type) {
        const bool supported =
            type == D3DQUERYTYPE_EVENT || type == D3DQUERYTYPE_OCCLUSION ||
            type == D3DQUERYTYPE_TIMESTAMP ||
            type == D3DQUERYTYPE_TIMESTAMPDISJOINT ||
            type == D3DQUERYTYPE_TIMESTAMPFREQ;
        SCOPED_TRACE(type);
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118, {device, type, 0}),
                  supported ? D3D_OK : D3DERR_NOTAVAILABLE);
        // A refused create leaves *ppQuery untouched (out_null=0).
        ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 0x1u).is_ok());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                         {device, type, static_cast<u32>(SCRATCH_ADDR)}),
                  supported ? D3D_OK : D3DERR_NOTAVAILABLE);
        if (!supported) {
            EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 0x1u);
        }
    }
    // VCACHE and MEMORYPRESSURE are driver options Vitreo does not offer.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_VCACHE, 0}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_MEMORYPRESSURE, 0}),
              D3DERR_NOTAVAILABLE);
}

TEST_F(D3D9ManagerTest, QueryTypesReportTheirResultSizesAndAcceptEveryIssueFlag) {
    const auto [factory, device] = create_device();
    const struct { u32 type; u32 size; } expected[] = {
        {D3DQUERYTYPE_EVENT, 4}, {D3DQUERYTYPE_OCCLUSION, 4},
        {D3DQUERYTYPE_TIMESTAMP, 8}, {D3DQUERYTYPE_TIMESTAMPDISJOINT, 4},
        {D3DQUERYTYPE_TIMESTAMPFREQ, 8}};
    for (const auto& e : expected) {
        SCOPED_TRACE(e.type);
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 118,
                         {device, e.type, static_cast<u32>(SCRATCH_ADDR)}),
                  D3D_OK);
        const u32 query = memory_.read_u32(SCRATCH_ADDR).value();
        EXPECT_EQ(invoke(D3D9InterfaceId::Query, 4, {query}), e.type);
        EXPECT_EQ(invoke(D3D9InterfaceId::Query, 5, {query}), e.size);
        // Signaled before any Issue.
        EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7,
                         {query, static_cast<u32>(SCRATCH_ADDR + 0x10), 8, 0}),
                  D3D_OK);
        for (const u32 flags : {0u, D3DISSUE_END, D3DISSUE_BEGIN, D3DISSUE_END,
                                D3DISSUE_BEGIN | D3DISSUE_END}) {
            EXPECT_EQ(invoke(D3D9InterfaceId::Query, 6, {query, flags}), D3D_OK)
                << flags;
        }
        EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7,
                         {query, static_cast<u32>(SCRATCH_ADDR + 0x10),
                          e.size, D3DGETDATA_FLUSH}),
                  D3D_OK);
        EXPECT_EQ(invoke(D3D9InterfaceId::Query, 2, {query}), 0u);
    }
}

TEST_F(D3D9ManagerTest, EventQueryCompletesWhenTheGpuPassesItsPoint) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_EVENT,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 query = memory_.read_u32(SCRATCH_ADDR).value();
    const auto data = static_cast<u32>(SCRATCH_ADDR + 0x10);

    backend_.record.timestamps_ready = false;
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {query, D3DISSUE_END}), D3D_OK);
    ASSERT_TRUE(memory_.write_u32(data, 0xCCCCCCCCu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, data, 4, D3DGETDATA_FLUSH}),
              S_FALSE_TEST);
    EXPECT_EQ(memory_.read_u32(data).value(), 0xCCCCCCCCu);

    backend_.record.timestamps_ready = true;
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, data, 4, D3DGETDATA_FLUSH}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(), 1u);  // TRUE
    // NULL pData asks only whether the result is ready.
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, 0, 0, 0}), D3D_OK);
}

TEST_F(D3D9ManagerTest, OcclusionQueryCountsOnlyTheDrawsItBrackets) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_OCCLUSION,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 query = memory_.read_u32(SCRATCH_ADDR).value();
    const auto data = static_cast<u32>(SCRATCH_ADDR + 0x10);
    ASSERT_NE(bind_triangle_stream(device), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);

    // Outside the bracket: not counted.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {query, D3DISSUE_BEGIN}), D3D_OK);
    // Building: S_FALSE.
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, data, 4, 0}),
              S_FALSE_TEST);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {query, D3DISSUE_END}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, data, 4, D3DGETDATA_FLUSH}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(),
              2 * backend_.record.samples_per_draw);
    EXPECT_GE(backend_.record.counted_passes, 1u);
    // The completed result is returned again.
    ASSERT_TRUE(memory_.write_u32(data, 0).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, data, 4, 0}), D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(),
              2 * backend_.record.samples_per_draw);

    // END without BEGIN brackets nothing: a ready zero.
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {query, D3DISSUE_END}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {query, data, 4, 0}), D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(), 0u);
}

TEST_F(D3D9ManagerTest, OverlappingOcclusionQueriesEachSumTheirOwnIntervals) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_OCCLUSION,
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_OCCLUSION,
                      static_cast<u32>(SCRATCH_ADDR + 4)}),
              D3D_OK);
    const u32 outer = memory_.read_u32(SCRATCH_ADDR).value();
    const u32 inner = memory_.read_u32(SCRATCH_ADDR + 4).value();
    const auto data = static_cast<u32>(SCRATCH_ADDR + 0x10);
    ASSERT_NE(bind_triangle_stream(device), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {outer, D3DISSUE_BEGIN}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {inner, D3DISSUE_BEGIN}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {inner, D3DISSUE_END}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {outer, D3DISSUE_END}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);

    const u64 per_draw = backend_.record.samples_per_draw;
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {inner, data, 4, D3DGETDATA_FLUSH}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(), per_draw);
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {outer, data, 4, D3DGETDATA_FLUSH}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(), 3 * per_draw);
}

TEST_F(D3D9ManagerTest, OcclusionIsUnavailableWhenTheBackendCannotCount) {
    backend_.record.fail_occlusion = true;
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                     {device, D3DQUERYTYPE_OCCLUSION, 0}),
              D3DERR_NOTAVAILABLE);
}

TEST_F(D3D9ManagerTest, TimestampQueriesAreMonotonicNanosecondsAtOneGigahertz) {
    const auto [factory, device] = create_device();
    const auto create = [&](u32 type, u32 at) {
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 118,
                         {device, type, static_cast<u32>(SCRATCH_ADDR + at)}),
                  D3D_OK);
        return memory_.read_u32(SCRATCH_ADDR + at).value();
    };
    const u32 disjoint = create(D3DQUERYTYPE_TIMESTAMPDISJOINT, 0);
    const u32 t0 = create(D3DQUERYTYPE_TIMESTAMP, 4);
    const u32 t1 = create(D3DQUERYTYPE_TIMESTAMP, 8);
    const u32 freq = create(D3DQUERYTYPE_TIMESTAMPFREQ, 12);
    const auto data = static_cast<u32>(SCRATCH_ADDR + 0x20);

    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {disjoint, D3DISSUE_BEGIN}), D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::Query, 7, {disjoint, data, 4, 0}),
              S_FALSE_TEST);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {t0, D3DISSUE_END}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {t1, D3DISSUE_END}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {disjoint, D3DISSUE_END}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 6, {freq, D3DISSUE_END}), D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 7, {t0, data, 8, D3DGETDATA_FLUSH}), D3D_OK);
    const u64 a = memory_.read_u64(data).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 7, {t1, data, 8, D3DGETDATA_FLUSH}), D3D_OK);
    const u64 b = memory_.read_u64(data).value();
    EXPECT_GE(b, a);
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 7, {freq, data, 8, 0}), D3D_OK);
    EXPECT_EQ(memory_.read_u64(data).value(), 1'000'000'000ull);
    ASSERT_TRUE(memory_.write_u32(data, 2).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 7, {disjoint, data, 4, 0}), D3D_OK);
    EXPECT_EQ(memory_.read_u32(data).value(), 0u);  // FALSE
    // A short buffer receives only the bytes it has room for.
    ASSERT_TRUE(memory_.write_u64(data, ~0ull).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Query, 7, {freq, data, 4, 0}), D3D_OK);
    EXPECT_EQ(memory_.read_u64(data).value(),
              0xFFFFFFFF00000000ull | (1'000'000'000ull & 0xFFFFFFFFull));
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
