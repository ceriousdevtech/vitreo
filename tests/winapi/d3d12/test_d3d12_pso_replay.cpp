// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d12_pso_replay.cpp
/// @brief VITREO_D3D12_DUMP_PSOS round trip: pipelines created with the
///        variable set are dumped with every guest range they read, and
///        tests/tools/d3d12_pso_replay_core.hpp replays each dump to the
///        same pipeline (the same translated shaders).

#include "../../tools/d3d12_pso_replay_core.hpp"
#include "winapi/graphics/d3d12/d3d12_root_signature.hpp"

#include "../../oracle_data/d3d12_shaders/d3d12_shaders.h"

#include <gtest/gtest.h>

#include <unistd.h>

namespace vitreo::winapi {
namespace {

using namespace d3d12;
using namespace pso_replay;

class D3D12PsoReplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
        std::string error;
        replayer = std::make_unique<Replayer>(error);
        if (!d3d12_enabled()) GTEST_SKIP() << "the D3D12 gate is off";
        ASSERT_TRUE(error.empty()) << error;
        h = replayer->h.get();
        dir = std::filesystem::temp_directory_path() /
              ("vitreo-d3d12-dump-psos-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
    }
    void TearDown() override {
        ::unsetenv("VITREO_D3D12_DUMP_PSOS");
        if (!dir.empty()) std::filesystem::remove_all(dir);
    }

    GuestAddr root(Frame& f, const RootSignatureDesc& rs) {
        std::vector<u8> blob;
        std::string error;
        EXPECT_EQ(serialize_root_signature(rs, ROOT_SIGNATURE_VERSION_1_0, blob, error), S_OK);
        (void)h->memory.write_bytes(f.base + 0x30000, blob.data(), blob.size());
        return h->made(f, h->com(f, h->device, DeviceSlot::CreateRootSignature,
                                 {0, f.base + 0x30000, blob.size(),
                                  h->iid(f, IID_ID3D12ROOTSIGNATURE), f.out()}));
    }

    void write(GuestAddr at, const void* data, usize size) {
        (void)h->memory.write_bytes(at, static_cast<const u8*>(data), size);
    }

    /// vs_color + ps_color through a description with a two-element input
    /// layout (POSITION, COLOR), and cs_madd through a stream.
    void create_originals() {
        Frame f = h->frame(0);
        const GuestAddr vs = f.base + 0x2000, ps = f.base + 0x8000, cs = f.base + 0x10000;
        const GuestAddr elements = f.base + 0x18000, names = f.base + 0x18100;
        const GuestAddr desc = f.base + 0x19000, stream = f.base + 0x1A000;
        const GuestAddr stream_desc = f.base + 0x1B000;
        write(vs, g_sh_vs_color, sizeof(g_sh_vs_color));
        write(ps, g_sh_ps_color, sizeof(g_sh_ps_color));
        write(cs, g_sh_cs_madd, sizeof(g_sh_cs_madd));
        write(names, "POSITION", 9);
        write(names + 0x20, "COLOR", 6);
        const auto& il = layout::D3D12_INPUT_ELEMENT_DESC[true];
        const std::vector<u8> zero(0x1000, 0);
        write(elements, zero.data(), il.size * 2u);
        (void)h->memory.write_u64(elements + il.SemanticName, names);
        (void)h->memory.write_u32(elements + il.Format, DXGI_FORMAT_R32G32_FLOAT);
        (void)h->memory.write_u64(elements + il.size + il.SemanticName, names + 0x20);
        (void)h->memory.write_u32(elements + il.size + il.Format, DXGI_FORMAT_R32G32B32A32_FLOAT);
        (void)h->memory.write_u32(elements + il.size + il.AlignedByteOffset, 8);
        RootSignatureDesc graphics_rs;
        graphics_rs.flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        const GuestAddr graphics_root = root(f, graphics_rs);
        RootSignatureDesc compute_rs;
        RootParameter constants;
        constants.parameter_type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.num_32bit_values = 4;
        RootParameter table;
        table.parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        RootDescriptorRange uav;
        uav.range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav.num_descriptors = 1;
        uav.base_shader_register = 2;
        table.ranges.push_back(uav);
        compute_rs.parameters = {constants, table};
        const GuestAddr compute_root = root(f, compute_rs);
        ASSERT_NE(graphics_root, 0u);
        ASSERT_NE(compute_root, 0u);

        const auto& gl = layout::D3D12_GRAPHICS_PIPELINE_STATE_DESC[true];
        const auto& blend = layout::D3D12_RENDER_TARGET_BLEND_DESC[true];
        write(desc, zero.data(), gl.size);
        (void)h->memory.write_u64(desc + gl.pRootSignature, graphics_root);
        (void)h->memory.write_u64(desc + gl.VS_pShaderBytecode, vs);
        (void)h->memory.write_u64(desc + gl.VS_BytecodeLength, sizeof(g_sh_vs_color));
        (void)h->memory.write_u64(desc + gl.PS_pShaderBytecode, ps);
        (void)h->memory.write_u64(desc + gl.PS_BytecodeLength, sizeof(g_sh_ps_color));
        const GuestAddr rt0 = desc + gl.BlendState_RenderTarget;
        (void)h->memory.write_u32(rt0 + blend.SrcBlend, D3D12_BLEND_ONE);
        (void)h->memory.write_u32(rt0 + blend.DestBlend, D3D12_BLEND_ZERO);
        (void)h->memory.write_u32(rt0 + blend.BlendOp, D3D12_BLEND_OP_ADD);
        (void)h->memory.write_u32(rt0 + blend.SrcBlendAlpha, D3D12_BLEND_ONE);
        (void)h->memory.write_u32(rt0 + blend.DestBlendAlpha, D3D12_BLEND_ZERO);
        (void)h->memory.write_u32(rt0 + blend.BlendOpAlpha, D3D12_BLEND_OP_ADD);
        (void)h->memory.write_u32(rt0 + blend.RenderTargetWriteMask, D3D12_COLOR_WRITE_ENABLE_ALL);
        (void)h->memory.write_u32(desc + gl.SampleMask, 0xFFFFFFFFu);
        (void)h->memory.write_u32(desc + gl.RasterizerState_FillMode, D3D12_FILL_MODE_SOLID);
        (void)h->memory.write_u32(desc + gl.RasterizerState_CullMode, D3D12_CULL_MODE_NONE);
        (void)h->memory.write_u32(desc + gl.RasterizerState_DepthClipEnable, 1);
        (void)h->memory.write_u64(desc + gl.InputLayout_pInputElementDescs, elements);
        (void)h->memory.write_u32(desc + gl.InputLayout_NumElements, 2);
        (void)h->memory.write_u32(desc + gl.PrimitiveTopologyType,
                                  D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        (void)h->memory.write_u32(desc + gl.NumRenderTargets, 1);
        (void)h->memory.write_u32(desc + gl.RTVFormats, DXGI_FORMAT_R8G8B8A8_UNORM);
        (void)h->memory.write_u32(desc + gl.SampleDesc_Count, 1);
        graphics = h->made(f, h->com(f, h->device, DeviceSlot::CreateGraphicsPipelineState,
                                     {desc, h->iid(f, IID_ID3D12PIPELINESTATE), f.out()}));
        ASSERT_NE(graphics, 0u);

        // Stream: ROOT_SIGNATURE, CS (each subobject pointer-aligned).
        write(stream, zero.data(), 64);
        (void)h->memory.write_u32(stream + 0, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE);
        (void)h->memory.write_u64(stream + 8, compute_root);
        (void)h->memory.write_u32(stream + 16, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS);
        (void)h->memory.write_u64(stream + 24, cs);
        (void)h->memory.write_u64(stream + 32, sizeof(g_sh_cs_madd));
        const auto& sl = layout::D3D12_PIPELINE_STATE_STREAM_DESC[true];
        (void)h->memory.write_u64(stream_desc + sl.SizeInBytes, 40);
        (void)h->memory.write_u64(stream_desc + sl.pPipelineStateSubobjectStream, stream);
        compute = h->made(f, h->com(f, h->device, DeviceSlot::CreatePipelineState,
                                    {stream_desc, h->iid(f, IID_ID3D12PIPELINESTATE), f.out()}));
        ASSERT_NE(compute, 0u);
    }

    std::string msl_of(GuestAddr address) {
        Frontend& frontend = h->manager->frontend();
        FrontendLock lock(frontend);
        const PipelineState* pso = frontend.find<PipelineState>(address, Kind::PipelineState);
        if (pso == nullptr) return {};
        return pso->vs.msl + "|" + pso->ps.msl + "|" + pso->cs.msl;
    }

    std::unique_ptr<Replayer> replayer;
    Harness* h{nullptr};
    std::filesystem::path dir;
    GuestAddr graphics{0}, compute{0};
};

TEST_F(D3D12PsoReplayTest, DumpedPipelinesReplayToTheSameShaders) {
    ::setenv("VITREO_D3D12_DUMP_PSOS", dir.c_str(), 1);
    create_originals();
    ::unsetenv("VITREO_D3D12_DUMP_PSOS");
    std::string error;
    const std::vector<Dump> dumps = load_dumps(dir, error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(dumps.size(), 2u);
    for (const Dump& d : dumps) {
        EXPECT_FALSE(d.legacy) << d.name << " has no .mem.bin";
        EXPECT_NE(d.root, 0u) << d.name;
        EXPECT_FALSE(d.root_blob.empty()) << d.name;
        if (d.api.find("Graphics") != std::string::npos) {
            // The description, both shaders, the input layout and its two
            // semantic names.
            EXPECT_EQ(d.regions.size(), 6u) << d.name;
        } else {
            // The stream description, the stream and the CS.
            EXPECT_EQ(d.regions.size(), 3u) << d.name;
        }
    }
    ASSERT_TRUE(replayer->prepare(dumps, error)) << error;
    const RunResult run = replayer->run(2, /*draw=*/true, /*release=*/false);
    ASSERT_EQ(run.pipelines.size(), 2u);
    std::vector<std::string> original = {msl_of(graphics), msl_of(compute)};
    std::vector<std::string> replayed;
    for (const PsoResult& r : run.pipelines) {
        EXPECT_FALSE(r.skipped) << r.name << ": " << r.skip_reason;
        EXPECT_EQ(r.hr, S_OK) << r.name;
        replayed.push_back(msl_of(r.pso));
    }
    std::sort(original.begin(), original.end());
    std::sort(replayed.begin(), replayed.end());
    EXPECT_EQ(original, replayed);
    EXPECT_FALSE(original[0].empty());
}

}  // namespace
}  // namespace vitreo::winapi
