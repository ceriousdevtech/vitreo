// SPDX-License-Identifier: LGPL-2.1-only
//
// Stream output (11.18).
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../graphics/gs_sm5_shaders.h"
#include "../../oracle_data/d3d11_gsso_shaders.h"

#include "d3d11_test_streamout.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11StreamOutTest, DeclarationValidationMatchesWindows) {
    const SoEntrySpec a4{0, "TEXCOORD", 0, 0, 4, 0};
    const SoEntrySpec b2{0, "TEXCOORD", 1, 0, 2, 0};
    struct Case {
        const char* label;
        std::vector<SoEntrySpec> entries;
        std::vector<u32> strides;
        u32 raster;
        u32 hr;
    };
    const u32 NO_RASTER = 0xFFFFFFFFu;
    const std::vector<Case> cases{
        {"a4+b2 stride24", {a4, b2}, {24}, 0, d3d11::S_OK},
        {"a4+b2 stride24 no-raster", {a4, b2}, {24}, NO_RASTER, d3d11::S_OK},
        {"a4+b2 stride24 raster=1", {a4, b2}, {24}, 1, d3d11::S_OK},
        {"a4+b2 stride16 (too small)", {a4, b2}, {16}, 0, d3d11::E_INVALIDARG},
        {"a4+b2 stride18 (not x4)", {a4, b2}, {18}, 0, d3d11::E_INVALIDARG},
        {"a4+b2 no strides", {a4, b2}, {}, 0, d3d11::S_OK},
        {"unknown semantic", {{0, "NOPE", 0, 0, 4, 0}}, {16}, 0,
         d3d11::E_INVALIDARG},
        {"TEXCOORD1 count4 (mask xy)", {{0, "TEXCOORD", 1, 0, 4, 0}}, {16}, 0,
         d3d11::E_INVALIDARG},
        {"start2 count3 (>4)", {{0, "TEXCOORD", 0, 2, 3, 0}}, {12}, 0,
         d3d11::E_INVALIDARG},
        {"output slot 4", {{0, "TEXCOORD", 0, 0, 4, 4}}, {16, 16, 16, 16}, 0,
         d3d11::E_INVALIDARG},
        {"stream 1 from a VS", {{1, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0,
         d3d11::E_INVALIDARG},
        {"gap2 + b2 stride16", {{0, nullptr, 0, 0, 2, 0}, b2}, {16}, 0,
         d3d11::S_OK},
        {"SV_POSITION", {{0, "SV_POSITION", 0, 0, 4, 0}}, {16}, 0,
         d3d11::S_OK},
        {"lowercase semantic", {{0, "texcoord", 0, 0, 4, 0}}, {16}, 0,
         d3d11::S_OK},
        {"count 0", {{0, "TEXCOORD", 0, 0, 0, 0}}, {16}, 0,
         d3d11::E_INVALIDARG},
        {"same element twice stride32", {a4, a4}, {32}, 0,
         d3d11::E_INVALIDARG},
        {"two slots", {a4, {0, "TEXCOORD", 1, 0, 2, 1}}, {16, 8}, 0,
         d3d11::S_OK},
        {"two slots, one stride", {a4, {0, "TEXCOORD", 1, 0, 2, 1}}, {16}, 0,
         d3d11::E_INVALIDARG},
    };
    for (const auto& c : cases) {
        const u32 hr = create_so(c.entries, c.strides, c.raster);
        EXPECT_EQ(hr, c.hr) << c.label;
        const u64 out = memory.read_u64(SHADER_OUT).value();
        if (c.hr == d3d11::S_OK) {
            EXPECT_NE(out, 0u) << c.label;
            EXPECT_NE(out, POISON) << c.label;
            set_arg(0, out);
            d3d11.impl_GeometryShaderMethod(marshaler, 2);  // Release
        } else {
            EXPECT_EQ(out, 0u) << c.label << ": Windows NULLs the out-param";
        }
        // pClassLinkage is arg(8); nothing may be written through it.
        EXPECT_EQ(memory.read_u64(LINKAGE).value(), POISON) << c.label;
        // Validation-only calls: S_FALSE when valid, the error otherwise.
        EXPECT_EQ(create_so(c.entries, c.strides, c.raster, true),
                  c.hr == d3d11::S_OK ? 1u : c.hr)
            << c.label << " (NULL out)";
    }
    // Zero entries: refused (measured).
    EXPECT_EQ(create_so({}, {16}, 0), d3d11::E_INVALIDARG);
}

TEST_F(D3D11StreamOutTest, GeometryProgramsStreamTheirExpansion) {
    // A geometry PROGRAM with a stream-output declaration (11.29): real fxc
    // gs_5_0 output, streaming SV_Position + COLOR with no rasterized stream.
    const std::vector<u8> gs(gs5_gs_quad, gs5_gs_quad + sizeof(gs5_gs_quad));
    ASSERT_TRUE(memory.write_bytes(
        VS_BYTECODE, reinterpret_cast<const Byte*>(gs.data()),
        gs.size()).is_ok());
    vs_blob = gs;
    EXPECT_EQ(create_so({{0, "SV_Position", 0, 0, 4, 0},
                         {0, "COLOR", 0, 0, 4, 0}},
                        {32}, 0xFFFFFFFFu),
              d3d11::S_OK);
    const GuestAddr streaming = memory.read_u64(SHADER_OUT).value();
    EXPECT_NE(streaming, 0u);
    EXPECT_NE(streaming, POISON);
    // A stream the program does not declare is refused (measured,
    // d3d11_gsso_probe [create] "single-stream GS, Stream = 1 entry":
    // E_INVALIDARG, out NULL). This test used to expect E_NOTIMPL, when no
    // stream past 0 was implemented.
    EXPECT_EQ(create_so({{1, "COLOR", 0, 0, 4, 0}}, {16}, 0xFFFFFFFFu),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(SHADER_OUT).value(), 0u);
    // A semantic the program does not output is the caller's error.
    EXPECT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0xFFFFFFFFu),
              d3d11::E_INVALIDARG);
}

TEST_F(D3D11StreamOutTest, BindingStateRoundTripsWithOwningReferences) {
    ASSERT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr x = make_so_buffer(64);
    const GuestAddr y = make_so_buffer(64);

    context_call(23, {gs, 0, 0});  // GSSetShader
    ASSERT_TRUE(memory.write_u64(GET_OUT, POISON).is_ok());
    context_call(82, {GET_OUT, 0, 0});  // GSGetShader
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), gs);
    // app 1 + the Get's own reference 1, then AddRef -> 3. The binding is
    // the runtime's and is not counted (measured, a12 [rc] "bound ...
    // count while bound=1").
    EXPECT_EQ(add_ref(1, gs, &D3D11Manager::impl_GeometryShaderMethod), 3u);

    // Windows: set 2 then set 1 leaves slot 1 NULL.
    so_set_targets({x, y}, {0, 0});
    so_set_targets({x}, {0});
    std::array<u64, 4> poison{POISON, POISON, POISON, POISON};
    ASSERT_TRUE(memory.write_bytes(
        GET_OUT, reinterpret_cast<const Byte*>(poison.data()), 32).is_ok());
    context_call(93, {4, GET_OUT});  // SOGetTargets
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), x);
    EXPECT_EQ(memory.read_u64(GET_OUT + 8).value(), 0u);
    EXPECT_EQ(memory.read_u64(GET_OUT + 16).value(), 0u);
    EXPECT_EQ(memory.read_u64(GET_OUT + 24).value(), 0u);

    // ClearState unbinds both (measured for SO targets).
    context_call(110, {});
    ASSERT_TRUE(memory.write_u64(GET_OUT, POISON).is_ok());
    context_call(93, {1, GET_OUT});
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), 0u);
    ASSERT_TRUE(memory.write_u64(GET_OUT, POISON).is_ok());
    context_call(82, {GET_OUT, 0, 0});
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), 0u);
}

// The shared context getter arm used add_ref(), which only resolves
// device-level interfaces -- so VSGetShader, PSGetShaderResources,
// IAGetInputLayout and the rest handed out references WITHOUT an AddRef. A
// guest that saved state with a Get and Released it afterwards, as state
// save/restore code does, freed objects that were still bound.
TEST_F(D3D11StreamOutTest, ContextGettersReturnOwningReferences) {
    const GuestAddr vs = make_vertex_shader();
    ASSERT_NE(vs, 0u);
    // Counts are the application's (measured, a12 [rc]): a binding adds
    // nothing, a Get adds one.
    context_call(11, {vs, 0, 0});  // VSSetShader: app 1
    context_call(76, {GET_OUT, 0, 0});  // VSGetShader: +1
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), vs);
    EXPECT_EQ(add_ref(1, vs, &D3D11Manager::impl_VertexShaderMethod), 3u)
        << "VSGetShader must AddRef what it returns";

    const GuestAddr buffer = make_so_buffer(64, 0x4 /* CONSTANT_BUFFER */);
    ASSERT_TRUE(memory.write_u64(TARGETS, buffer).is_ok());
    context_call(7, {0, 1, TARGETS});      // VSSetConstantBuffers: +0
    context_call(72, {0, 1, GET_OUT});     // VSGetConstantBuffers: +1
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), buffer);
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 1);  // AddRef
    EXPECT_EQ(cpu.gpr.rax, 3u) << "VSGetConstantBuffers must AddRef";

    // IAGetVertexBuffers had its own copy of the same no-op AddRef.
    const GuestAddr vb = make_so_buffer(64);
    ASSERT_TRUE(memory.write_u64(TARGETS, vb).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS + 4, 0).is_ok());
    context_call(18, {0, 1, TARGETS, OFFSETS, OFFSETS + 4});  // +0
    context_call(79, {0, 1, GET_OUT, 0, 0});                  // +1
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), vb);
    set_arg(0, vb);
    d3d11.impl_BufferMethod(marshaler, 1);
    EXPECT_EQ(cpu.gpr.rax, 3u) << "IAGetVertexBuffers must AddRef";
}

TEST_F(D3D11StreamOutTest, DrawCapturesThenRepacksInWindowsOrder) {
    // TEXCOORD0.xyzw to slot 0, stride 16, nothing rasterized.
    ASSERT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0xFFFFFFFFu),
              d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    const GuestAddr target = make_so_buffer(16 * 12);
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {5});  // TRIANGLESTRIP
    so_set_targets({target}, {0});

    const unsigned draws_before = backend.state.draws;
    context_call(13, {5, 0});  // Draw(5, 0)
    EXPECT_EQ(backend.state.capture_draws, 1u);
    EXPECT_EQ(backend.state.draws - draws_before, 1u)
        << "no rasterized stream: only the capture draw";
    EXPECT_GE(backend.state.rasterization_disabled_pipelines, 1u);
    EXPECT_EQ(backend.state.last_capture_vertex_count, 5u);
    ASSERT_EQ(backend.state.repack_controls.size(), 1u);
    const auto& ctl = backend.state.repack_controls.back();
    ASSERT_GE(ctl.size(), graphics::SO_CTL_ENTRIES + 4u);
    EXPECT_EQ(ctl[graphics::SO_CTL_VERTS_PER_PRIM], 3u);
    EXPECT_EQ(ctl[graphics::SO_CTL_STRIP], 1u);
    EXPECT_EQ(ctl[graphics::SO_CTL_RECORD_VEC4S], 3u) << "o0, o1, o2";
    EXPECT_EQ(ctl[graphics::SO_CTL_STRIDES], 16u);
    EXPECT_EQ(ctl[graphics::SO_CTL_ENTRIES + 2], 1u) << "o1 is record slot 1";
    EXPECT_EQ(ctl[graphics::SO_CTL_ENTRIES + 3], 0u | (4u << 8));
    // The accounting (vitreo_so_prepare, run here by its model): Windows
    // writes a 5-vertex strip as 3 triangles, from offset 0.
    ASSERT_EQ(backend.state.prepare_ops.size(), 1u);
    EXPECT_EQ(backend.state.prepare_params.back().primitives, 3u);
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_VERTEX_COUNT],
              9u);
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_DST_OFFSETS],
              0u);
    // The target the kernel writes is the guest's buffer.
    EXPECT_NE(backend.state.last_dispatch_buffers[graphics::SO_REPACK_TARGET_BASE],
              nullptr);

    // A second draw appends: the filled size advanced by 9 x 16.
    context_call(24, {1});  // POINTLIST
    context_call(13, {2, 0});
    ASSERT_EQ(backend.state.repack_controls.size(), 2u);
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_DST_OFFSETS],
              144u);
}

TEST_F(D3D11StreamOutTest, OverflowDropsWholePrimitivesAndUnboundSlotsWriteNothing) {
    // Two slots; slot 1 holds two 8-byte vertices.
    ASSERT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0},
                         {0, "TEXCOORD", 1, 0, 2, 1}},
                        {16, 8}, 0xFFFFFFFFu),
              d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    const GuestAddr big = make_so_buffer(16 * 8);
    const GuestAddr small = make_so_buffer(8 * 2);
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});  // POINTLIST
    so_set_targets({big, small}, {0, 0});
    context_call(13, {5, 0});
    ASSERT_EQ(backend.state.repack_controls.size(), 1u);
    // Windows: 5 points, slot 1 holds 2 -> 2 written to BOTH slots.
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_VERTEX_COUNT],
              2u);

    // Both full now: nothing more is written. The filled sizes are the
    // GPU's, so the draw is still captured and accounted there -- to zero.
    context_call(13, {1, 0});
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_VERTEX_COUNT],
              0u);

    // Windows: a declared slot with no buffer bound writes NOTHING anywhere
    // (the storage needed is still counted: no capture, no repack, only the
    // accounting).
    const unsigned captures = backend.state.capture_draws;
    const usize repacks = backend.state.repack_controls.size();
    const GuestAddr fresh = make_so_buffer(16 * 8);
    so_set_targets({fresh}, {0});
    context_call(13, {3, 0});
    EXPECT_EQ(backend.state.capture_draws, captures);
    EXPECT_EQ(backend.state.repack_controls.size(), repacks);
    EXPECT_EQ(backend.state.prepare_params.back().write_nothing, 1u);
    EXPECT_EQ(backend.state.prepare_params.back().primitives, 3u);
}

TEST_F(D3D11StreamOutTest, FilledSizeFollowsOffsetsAndDrivesDrawAuto) {
    ASSERT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0xFFFFFFFFu),
              d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    const GuestAddr a = make_so_buffer(16 * 8);
    const GuestAddr b = make_so_buffer(16 * 8);
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});

    // -1 on a fresh buffer starts at 0; five points fill 80 bytes.
    so_set_targets({a}, {0xFFFFFFFFu});
    context_call(13, {5, 0});
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_DST_OFFSETS],
              0u);
    // Rebinding with -1 appends.
    so_set_targets({a}, {0xFFFFFFFFu});
    context_call(13, {1, 0});
    EXPECT_EQ(backend.state.prepare_ops.back()[graphics::SO_OP_DST_OFFSETS],
              80u);

    // DrawAuto from `a` (96 bytes filled) at IA offset 16, stride 16:
    // Windows draws (96 - 16) / 16 = 5. The count is the GPU's (the filled
    // size is), handed to an indirect draw.
    so_set_targets({b}, {0});
    const u32 stride = 16, offset = 16;
    ASSERT_TRUE(memory.write_u64(TARGETS, a).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS, stride).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS + 4, offset).is_ok());
    context_call(18, {0, 1, TARGETS, OFFSETS, OFFSETS + 4});  // IASetVertexBuffers
    context_call(38, {});
    ASSERT_FALSE(backend.state.drawauto_counts.empty());
    EXPECT_EQ(backend.state.drawauto_counts.back(), 5u);

    // An explicit offset sets the filled size before anything is written:
    // SOSetTargets(a, 32) then DrawAuto draws (32 - 16) / 16 = 1.
    so_set_targets({a}, {32});
    so_set_targets({b}, {0});
    // Binding `a` for stream output unbound it from IA slot 0: an SO target
    // is an output, and Windows unbinds that buffer's vertex binding (measured,
    // d3d11_a05_context_state_probe "n so bSo bound: vb0: - ... strides 0").
    // Bind it again to draw from it.
    ASSERT_TRUE(memory.write_u64(TARGETS, a).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS, stride).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS + 4, offset).is_ok());
    context_call(18, {0, 1, TARGETS, OFFSETS, OFFSETS + 4});
    context_call(38, {});
    EXPECT_EQ(backend.state.drawauto_counts.back(), 1u);

    // A buffer never streamed to draws nothing.
    const GuestAddr never = make_so_buffer(64);
    ASSERT_TRUE(memory.write_u64(TARGETS, never).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSETS + 4, 0).is_ok());
    context_call(18, {0, 1, TARGETS, OFFSETS, OFFSETS + 4});
    context_call(38, {});
    EXPECT_EQ(backend.state.drawauto_counts.back(), 0u);
}

// D3D11_QUERY_SO_STATISTICS and the overflow predicate, against the probe's
// Windows numbers.
TEST_F(D3D11StreamOutTest, StatisticsAndOverflowPredicateMatchWindows) {
    constexpr GuestAddr QUERY_DESC = GET_OUT + 0x20;
    constexpr GuestAddr DATA = GET_OUT + 0x30;
    const auto create_query = [&](u32 type, usize slot) {
        EXPECT_TRUE(memory.write_u32(QUERY_DESC, type).is_ok());
        EXPECT_TRUE(memory.write_u32(QUERY_DESC + 4, 0).is_ok());
        set_arg(0, device);
        set_arg(1, QUERY_DESC);
        set_arg(2, GET_OUT);
        d3d11.impl_DeviceMethod(marshaler, slot);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "type " << type;
        return memory.read_u64(GET_OUT).value();
    };
    const GuestAddr stats = create_query(6, 24);      // CreateQuery
    // d3d11.h interleaves the per-stream values: 10 is SO_STATISTICS_STREAM1
    // (9 is SO_OVERFLOW_PREDICATE_STREAM0, which this test used to take for
    // it -- measured, d3d11_a10_queries_probe [create]).
    const GuestAddr stats1 = create_query(10, 24);    // SO_STATISTICS_STREAM1
    const GuestAddr overflow = create_query(7, 25);   // CreatePredicate
    const auto read_stats = [&](GuestAddr query) {
        context_call(29, {query, DATA, 16, 0});
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return std::pair<u64, u64>{memory.read_u64(DATA).value(),
                                   memory.read_u64(DATA + 8).value()};
    };
    // The counts are the GPU's (the accounting kernel's stats records):
    // final once the work before each End has run.
    const auto gpu_done = [&]() {
        for (const auto& point : backend.state.gpu_points) {
            point->is_ready = true;
        }
    };

    ASSERT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0xFFFFFFFFu),
              d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {4});  // TRIANGLELIST

    // Windows: "3 tris into room for 1.33" -> written 1, needed 3.
    const GuestAddr small = make_so_buffer(16 * 4);
    so_set_targets({small}, {0});
    for (const GuestAddr q : {stats, stats1, overflow}) context_call(27, {q});
    context_call(13, {9, 0});
    for (const GuestAddr q : {stats, stats1, overflow}) context_call(28, {q});
    // Not final before the GPU ran the draw's accounting.
    context_call(29, {stats, DATA, 16, 0});
    EXPECT_EQ(cpu.gpr.rax, 1u) << "S_FALSE until the GPU work completed";
    gpu_done();
    EXPECT_EQ(read_stats(stats), (std::pair<u64, u64>{1, 3}));
    EXPECT_EQ(read_stats(stats1), (std::pair<u64, u64>{0, 0}))
        << "a vertex shader writes stream 0 only";
    context_call(29, {overflow, DATA, 4, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(DATA).value(), 1u) << "it overflowed";

    // Begin resets. A draw that fits reports no overflow.
    const GuestAddr roomy = make_so_buffer(16 * 12);
    so_set_targets({roomy}, {0});
    for (const GuestAddr q : {stats, overflow}) context_call(27, {q});
    context_call(13, {6, 0});
    for (const GuestAddr q : {stats, overflow}) context_call(28, {q});
    gpu_done();
    EXPECT_EQ(read_stats(stats), (std::pair<u64, u64>{2, 2}));
    context_call(29, {overflow, DATA, 4, 0});
    EXPECT_EQ(memory.read_u32(DATA).value(), 0u);

    // A statistics struct is two UINT64s; less is refused.
    context_call(29, {stats, DATA, 8, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
}

TEST_F(D3D11StreamOutTest, MultiStreamDeclarationsFollowWindows) {
    // d3d11_gsso_probe [create]: gs_streams4 writes SV_Position+TEXCOORD0 to
    // stream 0, SV_Position+TEXCOORD1 to 1, TEXCOORD2 to 2, TEXCOORD3 to 3.
    const SoEntrySpec t0{0, "TEXCOORD", 0, 0, 4, 0};
    const SoEntrySpec t1{1, "TEXCOORD", 1, 0, 4, 1};
    const SoEntrySpec t2{2, "TEXCOORD", 2, 0, 4, 2};
    const SoEntrySpec t3{3, "TEXCOORD", 3, 0, 4, 3};
    const std::vector<u32> s16{16, 16, 16, 16};
    const u32 NONE = 0xFFFFFFFFu;
    struct Case {
        const char* label;
        std::vector<SoEntrySpec> entries;
        std::vector<u32> strides;
        u32 raster;
        u32 hr;
    };
    use_large_bytecode(gsso_gs_streams4, sizeof(gsso_gs_streams4));
    const std::vector<Case> streams4{
        {"4 streams -> 4 slots, raster NONE", {t0, t1, t2, t3}, s16, NONE,
         d3d11::S_OK},
        {"4 streams, raster 0", {t0, t1, t2, t3}, s16, 0, d3d11::S_OK},
        {"4 streams, raster 1", {t0, t1, t2, t3}, s16, 1, d3d11::S_OK},
        // RasterizedStream is only range-checked: a stream with no position
        // is accepted.
        {"4 streams, raster 2 (no position)", {t0, t1, t2, t3}, s16, 2,
         d3d11::S_OK},
        {"4 streams, raster 3 (no position)", {t0, t1, t2, t3}, s16, 3,
         d3d11::S_OK},
        {"4 streams, raster 4", {t0, t1, t2, t3}, s16, 4, d3d11::E_INVALIDARG},
        {"streams 0 and 1 into slot 0",
         {t0, {1, "TEXCOORD", 1, 0, 4, 0}}, s16, NONE, d3d11::E_INVALIDARG},
        {"stream 1 names stream 0's TEXCOORD0",
         {{1, "TEXCOORD", 0, 0, 4, 0}}, s16, NONE, d3d11::E_INVALIDARG},
        {"stream 2 names SV_Position (not in stream 2)",
         {{2, "SV_Position", 0, 0, 4, 0}}, s16, NONE, d3d11::E_INVALIDARG},
        {"stream 1 SV_Position -> slot 1", {{1, "SV_Position", 0, 0, 4, 1}},
         s16, NONE, d3d11::S_OK},
        {"Stream = 4", {{4, "TEXCOORD", 0, 0, 4, 0}}, s16, NONE,
         d3d11::E_INVALIDARG},
        {"only stream 1 streamed, raster 0", {t1}, s16, 0, d3d11::S_OK},
        {"stream 2 split over slots 2 and 3",
         {{2, "TEXCOORD", 2, 0, 2, 2}, {2, "TEXCOORD", 2, 2, 2, 3}}, s16, NONE,
         d3d11::S_OK},
        {"stream 1 gap then TEXCOORD1 -> slot 1",
         {{1, nullptr, 0, 0, 2, 1}, t1}, s16, NONE, d3d11::E_INVALIDARG},
    };
    const auto check = [&](const Case& c) {
        EXPECT_EQ(create_so(c.entries, c.strides, c.raster), c.hr) << c.label;
        const u64 out = memory.read_u64(SHADER_OUT).value();
        if (c.hr == d3d11::S_OK) {
            EXPECT_NE(out, 0u) << c.label;
            set_arg(0, out);
            d3d11.impl_GeometryShaderMethod(marshaler, 2);  // Release
        } else {
            EXPECT_EQ(out, 0u) << c.label << ": Windows NULLs the out-param";
        }
    };
    for (const auto& c : streams4) check(c);
    // Validation only: S_FALSE.
    EXPECT_EQ(create_so({t0, t1, t2, t3}, s16, NONE, true), 1u);

    // A single-stream program, and a vertex shader, have no stream 1.
    use_large_bytecode(gsso_gs_quad, sizeof(gsso_gs_quad));
    check({"single-stream GS, Stream = 1 entry", {t1}, s16, NONE,
           d3d11::E_INVALIDARG});
    check({"single-stream GS, raster 1", {t0}, {16}, 1, d3d11::S_OK});
    use_large_bytecode(gsso_vs_ids, sizeof(gsso_vs_ids));
    check({"vertex shader, raster 1", {t0}, {16}, 1, d3d11::S_OK});
    check({"vertex shader, raster NONE", {t0}, {16}, NONE, d3d11::S_OK});
    check({"vertex shader, Stream = 1 entry", {t1}, s16, NONE,
           d3d11::E_INVALIDARG});
}

TEST(D3D11StreamOutQueries, EachCountsItsStreamAndPredicatesAnyStream) {
    // d3d11_gsso_probe [streams] "gs_streams4 Draw(2), slot 2 room for 4":
    // per stream {needed, written} = {2,2} {4,4} {6,4} {8,8}. Windows
    // reported SO_STATISTICS as stream 0's numbers and SO_OVERFLOW_PREDICATE
    // TRUE because stream 2 overflowed. 6 SO_STATISTICS, 7
    // SO_OVERFLOW_PREDICATE, then per stream s 8 + 2s SO_STATISTICS_STREAMs
    // and 9 + 2s SO_OVERFLOW_PREDICATE_STREAMs.
    const std::array<u64, 4> needed{2, 4, 6, 8};
    const std::array<u64, 4> written{2, 4, 4, 8};
    struct Row {
        u32 type;
        u64 written, needed;
        bool overflowed;
    };
    for (const Row& r : {Row{6, 2, 2, true}, Row{8, 2, 2, false},
                         Row{10, 4, 4, false}, Row{12, 4, 6, true},
                         Row{14, 8, 8, false}}) {
        const auto counts = stream_out_query_counts(r.type, needed, written);
        EXPECT_EQ(counts.written, r.written) << r.type;
        EXPECT_EQ(counts.needed, r.needed) << r.type;
        // The predicate of the same stream (type + 1) answers the same.
        EXPECT_EQ(stream_out_query_counts(r.type + 1, needed, written)
                      .overflowed,
                  r.overflowed)
            << r.type + 1;
    }
    // "slot 2 unbound": stream 2 {needed=6, written=0}; streams 0/1/3 fine.
    EXPECT_TRUE(stream_out_query_counts(7, needed, {2, 4, 0, 8}).overflowed);
    EXPECT_FALSE(stream_out_query_counts(7, needed, needed).overflowed);
}

TEST_F(D3D11StreamOutTest, IndirectStreamOutResolvesItsCountsOnTheGpu) {
    // DrawInstancedIndirect with a stream-output target: the counts live in
    // GPU memory, so the draw is assembled there (vitreo_ia_resolve) and the
    // accounting reads the primitive count the resolve wrote (count_mode 1),
    // never a CPU readback of the argument buffer.
    ASSERT_EQ(create_so({{0, "TEXCOORD", 0, 0, 4, 0}}, {16}, 0xFFFFFFFFu),
              d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {3});  // LINESTRIP
    const GuestAddr target = make_so_buffer(16 * 64);
    so_set_targets({target}, {0});
    const GuestAddr args = make_indirect_args({3, 2, 4, 0});
    const usize entries = backend.state.dispatched_entries.size();
    const unsigned reads = backend.state.buffer_reads;
    const unsigned capture_draws = backend.state.capture_draws;
    const unsigned indirect_draws = backend.state.indirect_draws;
    context_call(40, {args, 0});  // DrawInstancedIndirect

    const std::vector<std::string> ran(
        backend.state.dispatched_entries.begin() +
            static_cast<std::ptrdiff_t>(entries),
        backend.state.dispatched_entries.end());
    ASSERT_GE(ran.size(), 3u);
    EXPECT_EQ(ran.front(), graphics::kIaResolveEntryPoint);
    EXPECT_NE(std::find(ran.begin(), ran.end(),
                        std::string(graphics::kStreamOutPrepareEntryPoint)),
              ran.end());
    EXPECT_EQ(ran.back(), graphics::kStreamOutRepackEntryPoint);
    EXPECT_EQ(backend.state.capture_draws, capture_draws + 1);
    EXPECT_EQ(backend.state.indirect_draws, indirect_draws + 1)
        << "the capture draw takes the resolved argument block";
    ASSERT_FALSE(backend.state.prepare_params.empty());
    EXPECT_EQ(backend.state.prepare_params.back().count_mode, 1u)
        << "primitives from the resolve's GPU word";
    EXPECT_EQ(backend.state.prepare_params.back().vertices_per_primitive, 2u);
    EXPECT_EQ(backend.state.buffer_reads, reads) << "no CPU readback";

    // An argument buffer without DRAWINDIRECT_ARGS draws nothing.
    const usize before = backend.state.dispatched_entries.size();
    const GuestAddr untagged = make_so_buffer(16);
    context_call(40, {untagged, 0});
    EXPECT_EQ(backend.state.dispatched_entries.size(), before);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
