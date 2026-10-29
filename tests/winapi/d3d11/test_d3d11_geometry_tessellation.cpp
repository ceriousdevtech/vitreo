// SPDX-License-Identifier: LGPL-2.1-only
//
// Geometry-shader emulation and hull/domain tessellation.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../graphics/gs_sm5_shaders.h"
#include "../../graphics/tessellation_shaders.h"
#include "../../oracle_data/d3d11_gsso_shaders.h"
#include "../../oracle_data/d3d11_gsso_shaders.h"
// In a namespace of its own: d3d11_refused_shaders.h already defines one of
// its programs (a09b_ps_stencil_ref) at global scope.
namespace a09b_blobs {
#include "../../oracle_data/d3d11_a09b_shaders.h"
}  // namespace a09b_blobs

#include "d3d11_test_streamout.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Geometry shaders (11.16): capture -> compute expansion -> indirect draw.
// The GPU half is proven on real Metal by
// GeneratedMslTest.GeometryEmulationRendersThroughAllThreePasses; these check
// that the frontend creates the objects and issues the passes.
// ---------------------------------------------------------------------------


/// gs_4_0: point in, point list out, `mov o0, v[0][0]; mov o1, v[0][1]; emit`.
/// It reads registers 0 and 1 of a vertex shader that writes 0, 1 and 2 --
/// the record-layout case the kernel must be compiled for.
/// @p extra_output_sv adds an o2.x output with that D3D_NAME (0: none).
[[nodiscard]] std::vector<u8> point_geometry_shader(u32 extra_output_sv) {
    const std::vector<u32> tokens{
        0x00020040u, 36u,
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,  // dcl_input_siv v[1][0], position
        0x0400005Fu, 0x002010F2u, 1u, 1u,      // dcl_input v[1][1]
        0x0100085Du,                           // dcl_inputprimitive point
        0x0100085Cu,                           // dcl_outputtopology pointlist
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, 1u,                       // dcl_maxout 1
        0x06000036u, 0x001020F2u, 0u, 0x00201E46u, 0u, 0u,
        0x06000036u, 0x001020F2u, 1u, 0x00201E46u, 0u, 1u,
        0x01000013u,                           // emit
        0x0100003Eu,
    };
    std::vector<SignatureElementSpec> outputs{
        {"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}};
    if (extra_output_sv == 2u) {
        outputs.push_back({"SV_ClipDistance", 0, 2, 3, 2, 0x1});
    } else if (extra_output_sv == 3u) {
        outputs.push_back({"SV_CullDistance", 0, 3, 3, 2, 0x1});
    }
    return dxbc_with_signatures(
        tokens,
        {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}},
        outputs);
}


class D3D11GeometryTest : public D3D11StreamOutTest {
protected:
    static constexpr GuestAddr GS_BYTECODE = SO_BASE + 0x900;  // < 0x300

    u32 create_gs(const std::vector<u8>& blob, bool null_out = false) {
        EXPECT_TRUE(memory.write_bytes(
            GS_BYTECODE, reinterpret_cast<const Byte*>(blob.data()),
            blob.size()).is_ok());
        EXPECT_TRUE(memory.write_u64(SHADER_OUT, POISON).is_ok());
        set_arg(0, device);
        set_arg(1, GS_BYTECODE);
        set_arg(2, blob.size());
        set_arg(3, 0);
        set_arg(4, null_out ? 0 : SHADER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 13);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// A 64x64 render target, bound: pass C is an ordinary draw into it.
    void bind_render_target() {
        const std::array<u32, 11> desc{64, 64, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
        ASSERT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, TEXTURE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 5);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        set_arg(0, device);
        set_arg(1, memory.read_u64(TEXTURE_OUT).value());
        set_arg(2, 0);
        set_arg(3, VIEW_OUT);
        d3d11.impl_DeviceMethod(marshaler, 9);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        context_call(33, {1, VIEW_OUT, 0});  // OMSetRenderTargets
    }
};

TEST_F(D3D11GeometryTest, CreationServesProgramsAndRefusesWhatItCannotExpress) {
    EXPECT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    EXPECT_NE(gs, 0u);
    EXPECT_NE(gs, POISON);
    // Validation-only call.
    EXPECT_EQ(create_gs(point_geometry_shader(0u), true), 1u);

    // A clip distance is delivered by the pass-through stage (11.27)...
    EXPECT_EQ(create_gs(point_geometry_shader(2u)), d3d11::S_OK);
    const GuestAddr clipping = memory.read_u64(SHADER_OUT).value();
    EXPECT_NE(clipping, 0u);
    // ...a cull distance cannot be (MSL has none): refuse, with the
    // out-parameter NULLed, rather than draw without culling.
    EXPECT_EQ(create_gs(point_geometry_shader(3u)), d3d11::E_NOTIMPL);
    EXPECT_EQ(memory.read_u64(SHADER_OUT).value(), 0u);
    // Real fxc gs_5_0: OSG5 signature, dcl_stream m0 / emit_stream m0,
    // [instance(4)], layer/viewport/clip outputs.
    const std::vector<u8> sm5(gs5_gs_layered,
                              gs5_gs_layered + sizeof(gs5_gs_layered));
    EXPECT_EQ(create_gs(sm5), d3d11::S_OK);
    const GuestAddr layered = memory.read_u64(SHADER_OUT).value();
    EXPECT_NE(layered, 0u);
    // Vertex-shader bytecode is not a geometry shader.
    EXPECT_EQ(create_gs(vs_blob), d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(SHADER_OUT).value(), 0u);

    for (const GuestAddr shader : {gs, clipping, layered}) {
        set_arg(0, shader);
        d3d11.impl_GeometryShaderMethod(marshaler, 2);  // Release
        EXPECT_EQ(cpu.gpr.rax, 0u);
    }
}

TEST_F(D3D11GeometryTest, DrawRunsCaptureExpansionAndRegionListDraw) {
    ASSERT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    // A geometry-stage constant buffer must reach the expansion kernel.
    const GuestAddr cb = make_so_buffer(64, 0x4);
    ASSERT_TRUE(memory.write_u64(TARGETS, cb).is_ok());
    context_call(22, {0, 1, TARGETS});  // GSSetConstantBuffers
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});  // POINTLIST

    const unsigned draws = backend.state.draws;
    const unsigned dispatches = backend.state.dispatches;
    const usize copies = backend.state.buffer_region_copies.size();
    context_call(13, {4, 0});  // Draw(4, 0)

    EXPECT_EQ(backend.state.capture_draws, 1u) << "pass A";
    EXPECT_EQ(backend.state.last_capture_vertex_count, 4u);
    EXPECT_EQ(backend.state.dispatches, dispatches + 1) << "pass B";
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_NE(backend.state.last_dispatch_buffers[base + i], nullptr)
            << "expansion buffer " << i;
    }
    EXPECT_NE(backend.state.last_dispatch_buffers[
                  graphics::BACKEND_CONSTANT_BUFFER_BASE],
              nullptr)
        << "GSSetConstantBuffers must reach the kernel";
    EXPECT_EQ(backend.state.last_dispatch_groups[0], 4u)
        << "one thread per invocation: 4 points x 1 instance";
    EXPECT_EQ(backend.state.buffer_region_copies.size(), copies)
        << "no GPU count to copy: each invocation owns a fixed region";
    EXPECT_EQ(backend.state.draws - draws, 2u) << "capture + pass C";
    EXPECT_EQ(backend.state.indirect_draws, 0u) << "pass C is a direct draw";
    EXPECT_EQ(backend.state.last_draw_vertex_count, 4u)
        << "every invocation's whole region: 4 x maxvertexcount 1";
    EXPECT_EQ(backend.state.last_draw_topology,
              graphics::BackendPrimitiveTopology::PointList);
    EXPECT_EQ(backend.state.last_draw_vertex_srv0,
              backend.state.last_dispatch_buffers[base + 1])
        << "pass C reads what the kernel wrote";
    EXPECT_EQ(backend.state.last_draw_vertex_srv1,
              backend.state.last_dispatch_buffers[base + 2])
        << "and the per-invocation counts that say how much of it";
}

std::vector<u32> words_of(const std::vector<u8>& bytes, usize count) {
    std::vector<u32> out(count, 0xDEADBEEFu);
    std::memcpy(out.data(), bytes.data(),
                std::min(bytes.size(), count * sizeof(u32)));
    return out;
}

TEST_F(D3D11GeometryTest, AnInstancedDrawRunsOneCaptureOneExpansionAndOneDraw) {
    // Every instance of a direct draw goes through ONE capture, ONE expansion
    // and ONE draw -- three passes per draw, not three per instance. The
    // layout is the one the indirect path's GPU resolve writes
    // (graphics/shaders/ia_resolve.hpp): records keyed by the draw-relative
    // vertex (the vertex index past StartVertexLocation, D3D11.3 8.16) plus
    // instance x keys-per-instance behind a {stride, capacity} header;
    // primitives instance-major; SV_PrimitiveID restarting with every
    // instance (8.17).
    ASSERT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});  // POINTLIST

    const unsigned draws = backend.state.draws;
    const unsigned dispatches = backend.state.dispatches;
    const unsigned captures = backend.state.capture_draws;
    context_call(21, {4, 3, 2, 5});  // DrawInstanced(4, 3, start 2, inst 5)

    EXPECT_EQ(backend.state.capture_draws - captures, 1u)
        << "one capture for every instance";
    EXPECT_EQ(backend.state.last_capture_call.instance_count, 3u);
    EXPECT_EQ(backend.state.last_capture_call.first_instance, 5u)
        << "per-instance data steps from the draw's StartInstanceLocation";
    EXPECT_EQ(backend.state.last_capture_call.first_vertex, 2u);
    EXPECT_EQ(backend.state.last_capture_vertex_count, 4u);
    ASSERT_EQ(backend.state.last_capture_header.size(), 16u);
    u32 header[4] = {};
    std::memcpy(header, backend.state.last_capture_header.data(), 16);
    EXPECT_EQ(header[0], 4u) << "keys per instance";
    EXPECT_EQ(header[1], 12u) << "capacity: 4 keys x 3 instances";

    EXPECT_EQ(backend.state.dispatches - dispatches, 1u) << "one expansion";
    EXPECT_EQ(backend.state.last_dispatch_groups[0], 12u)
        << "one thread per invocation: 4 points x 3 instances";
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    EXPECT_EQ(words_of(backend.state.last_dispatch_buffer_bytes[base + 3], 12),
              (std::vector<u32>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}))
        << "instance-major record keys";
    const auto control =
        words_of(backend.state.last_dispatch_buffer_bytes[base + 2],
                 graphics::kGsInvocationCountBase);
    EXPECT_NE(control[graphics::kGsControlFlags] &
                  graphics::kGsFlagPrimitiveIdModulo,
              0u)
        << "SV_PrimitiveID restarts with every instance";
    EXPECT_EQ(control[graphics::kGsControlPrimitivesPerInstance], 4u);
    EXPECT_EQ(control[graphics::kGsControlInvocationLimit], 12u);

    EXPECT_EQ(backend.state.draws - draws, 2u) << "capture + pass C";
    EXPECT_EQ(backend.state.last_draw_vertex_count, 12u)
        << "every invocation's whole region: 12 x maxvertexcount 1";
}

TEST_F(D3D11GeometryTest, AnIndexedInstancedDrawKeysRecordsByIndexValue) {
    // Indexed: a record's key is the index value (SV_VertexID's value,
    // VertexIndex - BaseVertexLocation, D3D11.3 8.16), keys per instance are
    // the largest index + 1, and the capture runs the draw's own index values
    // with BaseVertexLocation applied by the draw.
    ASSERT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});  // POINTLIST
    // A 16-bit index buffer {9, 2, 0, 5}; the draw reads from element 1.
    constexpr GuestAddr INIT = SO_BASE + 0xE00;
    constexpr GuestAddr SUBRESOURCE = SO_BASE + 0xE80;
    const std::array<u16, 4> indices{9, 2, 0, 5};
    ASSERT_TRUE(memory.write_bytes(
        INIT, reinterpret_cast<const Byte*>(indices.data()),
        sizeof(indices)).is_ok());
    ASSERT_TRUE(memory.write_u64(SUBRESOURCE, INIT).is_ok());
    ASSERT_TRUE(memory.write_u64(SUBRESOURCE + 8, 0).is_ok());
    const std::array<u32, 6> desc{sizeof(indices), 0, 0x2 /* INDEX */, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUFFER_DESC);
    set_arg(2, SUBRESOURCE);
    set_arg(3, BUFFER_OUT_);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr ib = memory.read_u64(BUFFER_OUT_).value();
    context_call(19, {ib, 57 /* R16_UINT */, 0});  // IASetIndexBuffer

    const unsigned captures = backend.state.capture_draws;
    // DrawIndexedInstanced(3, 2, StartIndex 1, BaseVertex 7, StartInstance 0)
    context_call(20, {3, 2, 1, 7, 0});

    EXPECT_EQ(backend.state.capture_draws - captures, 1u);
    EXPECT_EQ(backend.state.last_capture_call.instance_count, 2u);
    EXPECT_EQ(backend.state.last_capture_call.base_vertex, 7);
    EXPECT_EQ(backend.state.last_capture_vertex_count, 3u)
        << "the draw's three index values";
    u32 header[4] = {};
    ASSERT_EQ(backend.state.last_capture_header.size(), 16u);
    std::memcpy(header, backend.state.last_capture_header.data(), 16);
    EXPECT_EQ(header[0], 6u) << "keys per instance: largest index 5, plus 1";
    EXPECT_EQ(header[1], 12u);
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    EXPECT_EQ(words_of(backend.state.last_dispatch_buffer_bytes[base + 3], 6),
              (std::vector<u32>{2, 0, 5, 8, 6, 11}))
        << "index values, then index values + one instance's keys";
    EXPECT_EQ(backend.state.last_dispatch_groups[0], 6u);
}

TEST_F(D3D11GeometryTest, DrawsItCannotServeDrawNothing) {
    ASSERT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    const unsigned draws = backend.state.draws;
    const unsigned dispatches = backend.state.dispatches;

    // A triangle list into a POINT geometry shader: no primitive to hand it.
    context_call(24, {4});
    context_call(13, {3, 0});
    EXPECT_EQ(backend.state.draws, draws);
    EXPECT_EQ(backend.state.dispatches, dispatches);

    // Unbinding the geometry shader restores the ordinary draw.
    context_call(23, {0, 0, 0});
    context_call(13, {3, 0});
    EXPECT_EQ(backend.state.draws, draws + 1);
    EXPECT_EQ(backend.state.capture_draws, 0u);
}

// gs-ring (graphics/contracts/slim_record.hpp): a draw's transient buffers
// -- capture records, primitive ids, expansion, counter block -- serve a later
// draw once the GPU has retired the generation they were used in, never while
// it may still be using them, and a steady scene stops creating buffers. Runs
// with the switch as the environment sets it (default on): under
// VITREO_METAL_SLIM_RECORD=0 it fails, its RED.
TEST_F(D3D11GeometryTest, TransientBuffersAreReusedOnlyOnceTheirGenerationRetires) {
    static_assert(TransientBufferRing::size_class(1) == 4096);
    static_assert(TransientBufferRing::size_class(4096) == 4096);
    static_assert(TransientBufferRing::size_class(4097) == 5120);
    static_assert(TransientBufferRing::size_class(8192) == 8192);
    static_assert(TransientBufferRing::size_class(8193) == 10240);
    // 2^16 < 100000 <= 2^17: steps of 2^14, so 7 x 16384.
    static_assert(TransientBufferRing::size_class(100000) == 114688);
    ASSERT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});  // POINTLIST
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    const auto draw = [&] {
        context_call(13, {4, 0});  // Draw(4, 0)
        return std::pair{backend.state.last_dispatch_buffers[base + 1],
                         backend.state.last_dispatch_buffers[base + 2]};
    };

    // No generations (the contract's default): nothing can be waited on, so
    // every buffer is made for its draw as before.
    const auto untracked = d3d11.transient_ring_stats();
    (void)draw();
    EXPECT_GT(d3d11.transient_ring_stats().bypassed, untracked.bypassed);
    EXPECT_EQ(d3d11.transient_ring_stats().created, untracked.created);

    backend.state.generations = true;
    const auto retire = [&] {  // the recording submitted, and the GPU done
        backend.state.completed_gen = backend.state.recording_gen;
        ++backend.state.recording_gen;
    };
    const TransientBufferRing::Stats start = d3d11.transient_ring_stats();
    const usize made_before = backend.state.created_buffers.size();
    // Three draws in flight together: never one buffer twice.
    std::vector<std::pair<const graphics::BackendBuffer*,
                          const graphics::BackendBuffer*>>
        in_flight{draw(), draw(), draw()};
    // Every buffer those draws made came from the ring, which holds them.
    const std::vector<TestBackendBuffer*> made(
        backend.state.created_buffers.begin() +
            static_cast<std::ptrdiff_t>(made_before),
        backend.state.created_buffers.end());
    ASSERT_FALSE(made.empty());
    ASSERT_EQ(d3d11.transient_ring_stats().created - start.created,
              made.size());
    ASSERT_EQ(d3d11.transient_ring_stats().idle - start.idle, made.size())
        << "each one back in the ring, waiting for its generation";
    for (usize i = 0; i < in_flight.size(); ++i) {
        ASSERT_NE(in_flight[i].first, nullptr);
        ASSERT_NE(in_flight[i].second, nullptr);
        // ASSERT: past here the test writes into these buffers, which must
        // therefore still be alive (held by the ring), not freed.
        for (usize j = 0; j < i; ++j) {
            ASSERT_NE(in_flight[i].first, in_flight[j].first) << "expansion";
            ASSERT_NE(in_flight[i].second, in_flight[j].second) << "counter";
        }
    }
    EXPECT_EQ(d3d11.transient_ring_stats().reused, start.reused)
        << "nothing retired, nothing reused";
    // The "GPU" leaves its results in all of them.
    for (TestBackendBuffer* buffer : made) {
        std::fill(buffer->live().begin(), buffer->live().end(), u8{0xEE});
    }
    retire();

    // Retired: the next draw takes them back -- buffers of one shape serve
    // any role of that shape -- and its counter block is written whole.
    const auto reused = draw();
    ASSERT_GT(d3d11.transient_ring_stats().reused, start.reused);
    const bool was_in_flight =
        std::find(made.begin(), made.end(), reused.second) != made.end();
    ASSERT_TRUE(was_in_flight) << "a retired buffer serves as the counter";
    const u32* control =
        static_cast<const TestBackendBuffer*>(reused.second)->live_words(0);
    EXPECT_EQ(control[1], 2u);
    EXPECT_EQ(control[4], 1u) << "a point: one vertex a primitive";
    EXPECT_EQ(control[graphics::kGsControlInvocationLimit], 4u);
    EXPECT_EQ(control[graphics::kGsControlStreamStride], 4u);
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(control[graphics::kGsInvocationCountBase + i], 0u)
            << "invocation " << i << "'s count is reset, not the old one";
    }

    // A steady scene creates nothing after warm-up.
    for (int frame = 0; frame < 4; ++frame) {
        (void)draw();
        (void)draw();
        retire();
    }
    const unsigned warm = backend.state.buffer_creations;
    const u64 created = d3d11.transient_ring_stats().created;
    for (int frame = 0; frame < 8; ++frame) {
        (void)draw();
        (void)draw();
        retire();
    }
    EXPECT_EQ(d3d11.transient_ring_stats().created, created);
    EXPECT_EQ(backend.state.buffer_creations, warm)
        << "a GS draw made a buffer after warm-up";
}

TEST_F(D3D11GeometryTest, IndirectDrawAssemblesAndExpandsOnTheGpuTimeline) {
    // DrawInstancedIndirect through a geometry program: the vertex and
    // instance counts are GPU memory, so the draw is assembled by
    // vitreo_ia_resolve in command order, the capture draw takes the
    // resolved argument block, and the expansion kernel reads its
    // primitive count from the GPU -- no stall, no stale CPU readback.
    ASSERT_EQ(create_gs(point_geometry_shader(0u)), d3d11::S_OK);
    const GuestAddr gs = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(23, {gs, 0, 0});
    context_call(24, {1});  // POINTLIST
    const GuestAddr args = make_indirect_args({4, 2, 0, 0});
    const usize entries = backend.state.dispatched_entries.size();
    const unsigned reads = backend.state.buffer_reads;
    const unsigned capture_draws = backend.state.capture_draws;
    const unsigned indirect_draws = backend.state.indirect_draws;
    context_call(40, {args, 0});  // DrawInstancedIndirect

    const std::vector<std::string> ran(
        backend.state.dispatched_entries.begin() +
            static_cast<std::ptrdiff_t>(entries),
        backend.state.dispatched_entries.end());
    EXPECT_EQ(ran, (std::vector<std::string>{graphics::kIaResolveEntryPoint,
                                             "main0"}));
    EXPECT_EQ(backend.state.capture_draws, capture_draws + 1) << "pass A";
    EXPECT_GE(backend.state.indirect_draws, indirect_draws + 1)
        << "the capture draw takes the resolved argument block";
    // The expansion kernel is told to stop at the GPU's invocation count.
    constexpr unsigned base = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE;
    EXPECT_NE(backend.state.last_dispatch_buffers[base + 2], nullptr);
    EXPECT_EQ(backend.state.buffer_reads, reads) << "no CPU readback";
}

// ---------------------------------------------------------------------------
// Hull and domain shaders (11.17). Real fxc output
// (tools/oracle_probe/tessellation_shaders.hlsl); pixels are the oracle
// scenes' job, this pins the COM surface, the binding state and the three
// passes a patch draw becomes.
// ---------------------------------------------------------------------------

class D3D11TessellationTest : public D3D11GeometryTest {
protected:
    u32 create_stage(const unsigned char* bytes, usize size, bool hull,
                     bool null_out = false) {
        EXPECT_TRUE(memory.write_bytes(GS_BYTECODE,
                                       reinterpret_cast<const Byte*>(bytes),
                                       size).is_ok());
        EXPECT_TRUE(memory.write_u64(SHADER_OUT, POISON).is_ok());
        set_arg(0, device);
        set_arg(1, GS_BYTECODE);
        set_arg(2, size);
        set_arg(3, 0);
        set_arg(4, null_out ? 0 : SHADER_OUT);
        d3d11.impl_DeviceMethod(marshaler, hull ? 16 : 17);
        return static_cast<u32>(cpu.gpr.rax);
    }
};

TEST_F(D3D11TessellationTest, CreationAndBindingFollowTheShaderContract) {
    ASSERT_EQ(create_stage(tess_hs_tri, sizeof(tess_hs_tri), true),
              d3d11::S_OK);
    const GuestAddr hs = memory.read_u64(SHADER_OUT).value();
    ASSERT_EQ(create_stage(tess_ds_tri, sizeof(tess_ds_tri), false),
              d3d11::S_OK);
    const GuestAddr ds = memory.read_u64(SHADER_OUT).value();
    EXPECT_NE(hs, 0u);
    EXPECT_NE(ds, 0u);
    EXPECT_NE(hs, ds);
    // Validation-only calls.
    EXPECT_EQ(create_stage(tess_hs_quad, sizeof(tess_hs_quad), true, true), 1u);
    // The wrong stage's bytecode is the caller's error, out NULLed.
    EXPECT_EQ(create_stage(tess_ds_tri, sizeof(tess_ds_tri), true),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(SHADER_OUT).value(), 0u);
    EXPECT_EQ(create_stage(tess_hs_tri, sizeof(tess_hs_tri), false),
              d3d11::E_INVALIDARG);

    context_call(60, {hs, 0, 0});  // HSSetShader
    context_call(64, {ds, 0, 0});  // DSSetShader
    ASSERT_TRUE(memory.write_u64(GET_OUT, POISON).is_ok());
    context_call(98, {GET_OUT, 0, 0});  // HSGetShader
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), hs);
    ASSERT_TRUE(memory.write_u64(GET_OUT, POISON).is_ok());
    context_call(102, {GET_OUT, 0, 0});  // DSGetShader
    EXPECT_EQ(memory.read_u64(GET_OUT).value(), ds);
    // app 1 + the Get's own reference 1, then AddRef -> 3; the binding is
    // the runtime's and is not counted (measured, a12 [rc]).
    EXPECT_EQ(add_ref(1, hs, &D3D11Manager::impl_HullShaderMethod), 3u);
    EXPECT_EQ(add_ref(1, ds, &D3D11Manager::impl_DomainShaderMethod), 3u);

    // Constant buffers round-trip on both stages.
    const GuestAddr cb = make_so_buffer(64, 0x4);
    ASSERT_TRUE(memory.write_u64(TARGETS, cb).is_ok());
    context_call(62, {0, 1, TARGETS});  // HSSetConstantBuffers
    context_call(66, {0, 1, TARGETS});  // DSSetConstantBuffers
    for (const usize get : {usize{100}, usize{104}}) {
        ASSERT_TRUE(memory.write_u64(GET_OUT, POISON).is_ok());
        context_call(get, {0, 1, GET_OUT});
        EXPECT_EQ(memory.read_u64(GET_OUT).value(), cb) << get;
    }
}

TEST_F(D3D11TessellationTest, APatchDrawRunsCaptureHullKernelAndPatches) {
    ASSERT_EQ(create_stage(tess_hs_tri, sizeof(tess_hs_tri), true),
              d3d11::S_OK);
    const GuestAddr hs = memory.read_u64(SHADER_OUT).value();
    ASSERT_EQ(create_stage(tess_ds_tri, sizeof(tess_ds_tri), false),
              d3d11::S_OK);
    const GuestAddr ds = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(60, {hs, 0, 0});
    context_call(64, {ds, 0, 0});
    context_call(24, {35});  // 3_CONTROL_POINT_PATCHLIST

    const unsigned draws = backend.state.draws;
    const unsigned dispatches = backend.state.dispatches;
    context_call(13, {6, 0});  // Draw(6, 0): two patches

    EXPECT_EQ(backend.state.capture_draws, 1u) << "pass A";
    EXPECT_EQ(backend.state.dispatches, dispatches + 1) << "pass B";
    EXPECT_EQ(backend.state.last_dispatch_groups[0], 2u) << "one per patch";
    EXPECT_EQ(backend.state.draws - draws, 2u) << "capture + patch draw";
    EXPECT_EQ(backend.state.last_draw_patch_count, 2u);
    EXPECT_EQ(backend.state.last_draw_patch_control_points, 3u);
    EXPECT_EQ(backend.state.last_draw_tess_factors,
              backend.state.last_dispatch_buffers[
                  graphics::BACKEND_GS_EXPANSION_BUFFER_BASE + 5])
        << "the patch draw reads the factors the hull kernel wrote";

    // A patch list the hull shader does not declare draws nothing; so does a
    // patch list with no tessellation stages bound.
    context_call(24, {36});  // 4_CONTROL_POINT_PATCHLIST
    const unsigned before = backend.state.draws;
    context_call(13, {8, 0});
    EXPECT_EQ(backend.state.draws, before);
    context_call(60, {0, 0, 0});
    context_call(64, {0, 0, 0});
    context_call(13, {8, 0});
    EXPECT_EQ(backend.state.draws, before);
}

TEST_F(D3D11TessellationTest, TheSoftwareTessellatorServesWhatMetalCannot) {
    // Isoline domains and point output (11.7.15) have no Metal tessellator
    // form; they used to be refused with E_NOTIMPL. They run on the
    // software tessellator: hull kernel, count, scan, write, then the
    // compute-lowered domain shader, and the tessellator's index list drawn
    // indirectly -- never a patch draw.
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    struct Case {
        const char* label;
        const unsigned char* hs;
        usize hs_size;
        const unsigned char* ds;
        usize ds_size;
        u32 topology;
    };
    for (const Case& c :
         {Case{"isoline (hs_iso_even)", gsso_hs_iso_even,
               sizeof(gsso_hs_iso_even), gsso_ds_iso, sizeof(gsso_ds_iso),
               34 /* 2_CONTROL_POINT_PATCHLIST */},
          Case{"point output (hs_tri_odd_pt)", gsso_hs_tri_odd_pt,
               sizeof(gsso_hs_tri_odd_pt), gsso_ds_tri, sizeof(gsso_ds_tri),
               35}}) {
        ASSERT_EQ(create_stage(c.hs, c.hs_size, true), d3d11::S_OK) << c.label;
        const GuestAddr hs = memory.read_u64(SHADER_OUT).value();
        ASSERT_EQ(create_stage(c.ds, c.ds_size, false), d3d11::S_OK)
            << c.label;
        const GuestAddr ds = memory.read_u64(SHADER_OUT).value();
        context_call(60, {hs, 0, 0});
        context_call(64, {ds, 0, 0});
        context_call(24, {c.topology});
        const usize entries = backend.state.dispatched_entries.size();
        const unsigned patch_pipelines = backend.state.tessellation_pipelines;
        const unsigned indirect_draws = backend.state.indirect_draws;
        context_call(13, {c.topology - 32u, 0});  // Draw: one patch

        const std::vector<std::string> ran(
            backend.state.dispatched_entries.begin() +
                static_cast<std::ptrdiff_t>(entries),
            backend.state.dispatched_entries.end());
        const std::vector<std::string> expected{
            "main0", graphics::tess::kTessPatchEntryPoint,
            graphics::tess::kTessScanEntryPoint,
            graphics::tess::kTessPatchEntryPoint, "main0"};
        EXPECT_EQ(ran, expected) << c.label;
        EXPECT_EQ(backend.state.tessellation_pipelines, patch_pipelines)
            << c.label << ": no Metal patch draw";
        EXPECT_EQ(backend.state.indirect_draws, indirect_draws + 1)
            << c.label << ": the domain records, by the tessellator's count";
        EXPECT_EQ(backend.state.last_draw_patch_count, 0u) << c.label;
    }
}

TEST_F(D3D11TessellationTest, PowerOfTwoPartitioningTakesTheIntegerTessellator) {
    // pow2 is integer partitioning with factors rounded up to a power of
    // two (D3D11.3 11.7.6), which the hull kernel does before Metal's
    // integer tessellator runs -- the WARP hash of hs_tri_pow2 in the a09b
    // probe matches. This used to be refused.
    using a09b_blobs::a09b_ds_tri;
    using a09b_blobs::a09b_hs_tri_pow2;
    ASSERT_EQ(create_stage(a09b_hs_tri_pow2, sizeof(a09b_hs_tri_pow2), true),
              d3d11::S_OK);
    const GuestAddr hs = memory.read_u64(SHADER_OUT).value();
    ASSERT_EQ(create_stage(a09b_ds_tri, sizeof(a09b_ds_tri), false),
              d3d11::S_OK);
    const GuestAddr ds = memory.read_u64(SHADER_OUT).value();
    const GuestAddr vs = make_vertex_shader();
    bind_render_target();
    context_call(11, {vs, 0, 0});
    context_call(60, {hs, 0, 0});
    context_call(64, {ds, 0, 0});
    context_call(24, {35});
    const unsigned patch_pipelines = backend.state.tessellation_pipelines;
    context_call(13, {3, 0});
    EXPECT_EQ(backend.state.last_draw_patch_count, 1u) << "a Metal patch draw";
    EXPECT_EQ(backend.state.tessellation_pipelines, patch_pipelines + 1);
    EXPECT_EQ(backend.state.last_tessellation_partition,
              graphics::BackendTessellationPartition::Integer);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
