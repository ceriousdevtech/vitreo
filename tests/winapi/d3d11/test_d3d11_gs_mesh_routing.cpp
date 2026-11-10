// SPDX-License-Identifier: LGPL-2.1-only
//
// WS5 Task 8: routing of GS draws between the mesh path and the three-pass
// path on the fake backend (host-offload plan WS5).

#include <gtest/gtest.h>

#include "d3d11_test_streamout.hpp"
#include "winapi/graphics/d3d11/d3d11_gs_mesh_select.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

using gs_mesh::Force;

/// gs_4_0: point in, triangle strip out, maxvertexcount 3, three emits of
/// (v[0][0], v[0][1]) -- mesh-eligible (point OUTPUT stays three-pass).
std::vector<u8> point_to_strip_shader() {
    std::vector<u32> tokens{
        0x00020040u, 0u,
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,  // dcl_input_siv v[1][0], position
        0x0400005Fu, 0x002010F2u, 1u, 1u,      // dcl_input v[1][1]
        0x0100085Du,                           // dcl_inputprimitive point
        0x0100285Cu,                           // dcl_outputtopology trianglestrip
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, 3u,                       // dcl_maxout 3
    };
    for (int e = 0; e < 3; ++e) {
        tokens.insert(tokens.end(), {0x06000036u, 0x001020F2u, 0u, 0x00201E46u, 0u, 0u,
                                     0x06000036u, 0x001020F2u, 1u, 0x00201E46u, 0u, 1u,
                                     0x01000013u});
    }
    tokens.push_back(0x0100003Eu);
    tokens[1] = static_cast<u32>(tokens.size());
    return dxbc_with_signatures(
        tokens, {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}},
        {{"SV_Position", 0, 1, 3, 0, 0xF}, {"TEXCOORD", 0, 0, 3, 1, 0xF}});
}

class D3D11GsMeshRoutingTest : public D3D11StreamOutTest {
protected:
    static constexpr GuestAddr GS_BYTECODE = SO_BASE + 0x900;  // < 0x300
    static constexpr GuestAddr BLEND_DESC = SO_BASE + 0xC00;   // < 0x200

    void SetUp() override {
        D3D11StreamOutTest::SetUp();
        backend.state.mesh_supported = true;
    }
    void TearDown() override {
        gs_mesh::force_for_test(Force::None);
        D3D11StreamOutTest::TearDown();
    }

    GuestAddr create_gs(const std::vector<u8>& blob) {
        EXPECT_LE(blob.size(), 0x300u);
        EXPECT_TRUE(memory.write_bytes(GS_BYTECODE, reinterpret_cast<const Byte*>(blob.data()),
                                       blob.size()).is_ok());
        set_arg(0, device);
        set_arg(1, GS_BYTECODE);
        set_arg(2, blob.size());
        set_arg(3, 0);
        set_arg(4, SHADER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 13);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(SHADER_OUT).value();
    }

    void bind_render_target() {
        const std::array<u32, 11> desc{64, 64, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
        ASSERT_TRUE(memory.write_bytes(TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
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

    /// OMSetBlendState: RT0 with `write_mask`, blending `op` (0 = off).
    void set_blend(u32 write_mask, u32 src, u32 dst, u32 op) {
        std::array<u32, 66> desc{};
        desc[2] = op != 0 ? 1u : 0u;
        desc[3] = desc[6] = src;
        desc[4] = desc[7] = dst;
        desc[5] = desc[8] = op != 0 ? op : 1u;
        desc[9] = write_mask;
        ASSERT_TRUE(memory.write_bytes(BLEND_DESC, reinterpret_cast<const Byte*>(desc.data()),
                                       sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, BLEND_DESC);
        set_arg(2, SHADER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 20);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        context_call(35, {memory.read_u64(SHADER_OUT).value(), 0, 0xFFFFFFFFu});
    }

    void bind_pipeline() {
        const GuestAddr gs = create_gs(point_to_strip_shader());
        const GuestAddr vs = make_vertex_shader();
        bind_render_target();
        context_call(11, {vs, 0, 0});
        context_call(23, {gs, 0, 0});
        context_call(24, {1});  // POINTLIST
    }
};

TEST_F(D3D11GsMeshRoutingTest, EligibleDirectDrawRunsInPassCaptureAndOneMeshDraw) {
    bind_pipeline();
    set_blend(0, 2, 1, 0);  // RT write mask 0: no order-dependent output
    const unsigned captures = backend.state.capture_draws;
    const unsigned dispatches = backend.state.dispatches;
    const unsigned meshes = backend.state.mesh_draws;
    const u64 counted = d3d11.debug_gs_mesh_draws();
    context_call(13, {4, 0});  // Draw(4, 0)
    if (gs_mesh::kill_switch_off()) GTEST_SKIP() << "VITREO_WS5=0";
    EXPECT_EQ(backend.state.capture_draws, captures + 1);
    EXPECT_TRUE(backend.state.last_capture_in_pass) << "the capture runs in the guest's pass";
    EXPECT_EQ(backend.state.dispatches, dispatches) << "no expansion kernel";
    EXPECT_EQ(backend.state.mesh_draws, meshes + 1);
    EXPECT_TRUE(backend.state.last_mesh_barrier);
    // vpi 3, 2 records: K = 85 invocations per group, so 4 points need 1.
    EXPECT_EQ(backend.state.last_mesh_groups, 1u);
    EXPECT_EQ(backend.state.mesh_pipelines_created, 1u);
    EXPECT_EQ(d3d11.debug_gs_mesh_draws(), counted + 1);
}

// Order-dependent blending. A draw that is one mesh (its invocations fit the
// shader's threadgroup: K = 85 here) is rasterized in primitive order, D3D's
// order, and takes the mesh path; one invocation more needs a second
// threadgroup, whose order against the first is undocumented: three-pass.
TEST_F(D3D11GsMeshRoutingTest, BlendedDrawThatIsOneMeshTakesTheMeshPath) {
    bind_pipeline();
    set_blend(0xF, 5, 6, 1);  // SRC_ALPHA, INV_SRC_ALPHA, ADD
    const unsigned dispatches = backend.state.dispatches;
    const unsigned meshes = backend.state.mesh_draws;
    context_call(13, {85, 0});
    if (gs_mesh::kill_switch_off()) GTEST_SKIP() << "VITREO_WS5=0";
    EXPECT_EQ(backend.state.mesh_draws, meshes + 1);
    EXPECT_EQ(backend.state.last_mesh_groups, 1u);
    EXPECT_EQ(backend.state.dispatches, dispatches) << "no expansion kernel";
}

TEST_F(D3D11GsMeshRoutingTest, BlendedDrawAcrossThreadgroupsStaysThreePass) {
    bind_pipeline();
    set_blend(0xF, 5, 6, 1);  // SRC_ALPHA, INV_SRC_ALPHA, ADD
    const unsigned dispatches = backend.state.dispatches;
    const unsigned meshes = backend.state.mesh_draws;
    context_call(13, {86, 0});
    EXPECT_EQ(backend.state.mesh_draws, meshes);
    EXPECT_EQ(backend.state.dispatches, dispatches + 1);
    EXPECT_FALSE(backend.state.last_capture_in_pass);
    EXPECT_EQ(backend.state.mesh_pipelines_created, 0u);
}

TEST_F(D3D11GsMeshRoutingTest, KillSwitchAndForceKeepThreePass) {
    bind_pipeline();
    set_blend(0, 2, 1, 0);
    gs_mesh::force_for_test(Force::ThreePass);
    const unsigned dispatches = backend.state.dispatches;
    context_call(13, {4, 0});
    EXPECT_EQ(backend.state.mesh_pipelines_created, 0u);
    EXPECT_EQ(backend.state.mesh_draws, 0u);
    EXPECT_EQ(backend.state.dispatches, dispatches + 1);
}

TEST_F(D3D11GsMeshRoutingTest, IndirectGsDrawNeverUsesMesh) {
    bind_pipeline();
    set_blend(0, 2, 1, 0);
    const GuestAddr args = make_indirect_args({4, 2, 0, 0});
    context_call(40, {args, 0});  // DrawInstancedIndirect
    EXPECT_EQ(backend.state.mesh_draws, 0u);
    EXPECT_EQ(backend.state.mesh_pipelines_created, 0u);
}

TEST_F(D3D11GsMeshRoutingTest, MeshPipelineFailureFallsBackToThreePass) {
    bind_pipeline();
    set_blend(0, 2, 1, 0);
    backend.state.mesh_pipeline_fails = true;
    const unsigned dispatches = backend.state.dispatches;
    context_call(13, {4, 0});
    if (gs_mesh::kill_switch_off()) GTEST_SKIP() << "VITREO_WS5=0";
    EXPECT_EQ(backend.state.mesh_pipelines_created, 1u);
    EXPECT_EQ(backend.state.mesh_draws, 0u);
    EXPECT_EQ(backend.state.dispatches, dispatches + 1) << "drawn three-pass instead";
    context_call(13, {4, 0});
    EXPECT_EQ(backend.state.mesh_pipelines_created, 1u) << "a refused pipeline is not retried";
    EXPECT_EQ(backend.state.dispatches, dispatches + 2);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
