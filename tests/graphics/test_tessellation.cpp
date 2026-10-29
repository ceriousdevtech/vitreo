// SPDX-License-Identifier: LGPL-2.1-only
//
// Hull / domain shaders lowered for Metal (11.17,
// .docs/design-tessellation-emulation.md), from real fxc output
// (tools/oracle_probe/tessellation_shaders.hlsl): every hull shader must
// become a compute kernel Metal accepts, every domain shader a
// post-tessellation vertex function.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "tessellation_shaders.h"
#include "../oracle_data/d3d11_gsso_shaders.h"

#include <string>
#include <tuple>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

struct Lowered {
    DxbcToSpirvResult spirv;
    std::string msl;
};

Lowered lower(const unsigned char* bytecode, usize size, ShaderStage stage) {
    Lowered out;
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.emulate_tessellation = true;
    out.spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(out.spirv.valid()) << out.spirv.failure;
    if (!out.spirv.valid()) return out;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(
        out.spirv.spirv.data(), out.spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    if (msl.valid()) out.msl = msl.msl_text();
    return out;
}

TEST(Tessellation, HullShadersBecomeMetalComputeKernels) {
    auto& device = MetalDevice::instance();
    struct Case {
        const char* name;
        const unsigned char* bytes;
        usize size;
        u32 domain, partitioning, output, in_cp, out_cp;
    };
    const Case cases[] = {
        {"hs_tri", tess_hs_tri, sizeof(tess_hs_tri), 2, 1, 3, 3, 3},
        {"hs_quad", tess_hs_quad, sizeof(tess_hs_quad), 3, 3, 4, 4, 4},
        {"hs_phases", tess_hs_phases, sizeof(tess_hs_phases), 2, 4, 3, 3, 3},
    };
    for (const auto& c : cases) {
        const auto lowered = lower(c.bytes, c.size, ShaderStage::Compute);
        ASSERT_TRUE(lowered.spirv.valid()) << c.name;
        EXPECT_TRUE(lowered.spirv.hs_lowered_to_compute) << c.name;
        EXPECT_EQ(lowered.spirv.tess_domain, c.domain) << c.name;
        EXPECT_EQ(lowered.spirv.tess_partitioning, c.partitioning) << c.name;
        EXPECT_EQ(lowered.spirv.tess_output_primitive, c.output) << c.name;
        EXPECT_EQ(lowered.spirv.hs_input_control_points, c.in_cp) << c.name;
        EXPECT_EQ(lowered.spirv.hs_output_control_points, c.out_cp) << c.name;
        ASSERT_FALSE(lowered.msl.empty()) << c.name;
        if (!device.available()) continue;
        BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = lowered.msl;
        desc.compute_entry_point = "main0";
        desc.cache_key = std::hash<std::string>{}(lowered.msl);
        auto pipeline = device.create_compute_pipeline(desc);
        EXPECT_TRUE(pipeline && pipeline->valid())
            << c.name << "\n" << lowered.msl;
    }
}

TEST(Tessellation, DomainShadersBecomePostTessellationVertexFunctions) {
    for (const auto& [name, bytes, size] :
         {std::tuple{"ds_tri", tess_ds_tri, sizeof(tess_ds_tri)},
          std::tuple{"ds_quad", tess_ds_quad, sizeof(tess_ds_quad)}}) {
        const auto lowered = lower(bytes, size, ShaderStage::Domain);
        ASSERT_TRUE(lowered.spirv.valid()) << name;
        EXPECT_TRUE(lowered.spirv.ds_lowered) << name;
        const std::string patch = std::string("[[ patch(") +
                                  (std::string(name) == "ds_tri" ? "triangle"
                                                                 : "quad") +
                                  (std::string(name) == "ds_tri" ? ", 3)"
                                                                 : ", 4)");
        EXPECT_NE(lowered.msl.find(patch), std::string::npos)
            << name << "\n" << lowered.msl;
        EXPECT_NE(lowered.msl.find("position_in_patch"), std::string::npos)
            << name << "\n" << lowered.msl;
    }
}

// The software tessellator's form: every domain shader D3D11 allows --
// isoline domains and point/line output included, which Metal's fixed
// tessellator cannot produce (11.7.15) -- becomes a compute kernel, one
// thread per tessellated point, writing capture-layout records.
TEST(Tessellation, DomainShadersLowerToComputeKernelsForEveryDomain) {
    auto& device = MetalDevice::instance();
    for (const auto& [name, bytes, size] :
         {std::tuple{"tess_ds_tri", tess_ds_tri, sizeof(tess_ds_tri)},
          std::tuple{"tess_ds_quad", tess_ds_quad, sizeof(tess_ds_quad)},
          std::tuple{"gsso_ds_tri", gsso_ds_tri, sizeof(gsso_ds_tri)},
          std::tuple{"gsso_ds_iso", gsso_ds_iso, sizeof(gsso_ds_iso)}}) {
        const auto container = parse_dxbc(bytes, size);
        ASSERT_TRUE(container.valid()) << name;
        DxbcTranslationOptions options{};
        options.emulate_tessellation = true;
        options.domain_to_compute = true;
        const auto spirv = translate_dxbc_to_spirv(container, options);
        ASSERT_TRUE(spirv.valid()) << name << ": " << spirv.failure;
        EXPECT_TRUE(spirv.ds_lowered_to_compute) << name;
        EXPECT_FALSE(spirv.capture_registers.empty()) << name;
        MslCompileOptions flat{};
        flat.argument_buffers = false;
        const auto msl = compile_spirv_to_msl(
            spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
        ASSERT_TRUE(msl.valid()) << name;
        const std::string text = msl.msl_text();
        EXPECT_EQ(text.find("[[ patch("), std::string::npos) << name;
        if (!device.available()) continue;
        BackendComputePipelineDesc desc{};
        desc.compute_shader_msl = msl.msl_text();
        desc.compute_entry_point = "main0";
        desc.cache_key = std::hash<std::string>{}(desc.compute_shader_msl);
        auto pipeline = device.create_compute_pipeline(desc);
        EXPECT_TRUE(pipeline && pipeline->valid())
            << name << "\n" << msl.msl_text();
    }
}

}  // namespace

namespace {

// The whole Metal path at the backend: control-point records -> the lowered
// hull kernel (tess_hs_tri, factors from cb0) -> drawPatches through the
// lowered domain shader and ps_color. The domain shader colours each vertex
// (u, v, w) * 0.5, so the corners show which control point Metal's
// position_in_patch (1,0,0) / (0,1,0) / (0,0,1) is.
TEST(Tessellation, APatchDrawRendersTheDomainShader) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    // Hull kernel.
    auto hs_container = parse_dxbc(tess_hs_tri, sizeof(tess_hs_tri));
    DxbcTranslationOptions options{};
    options.emulate_tessellation = true;
    options.gs_input_record_registers = {0, 1};
    const auto hs = translate_dxbc_to_spirv(hs_container, options);
    ASSERT_TRUE(hs.valid()) << hs.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto hs_msl = compile_spirv_to_msl(hs.spirv.data(), hs.spirv.size(),
                                             ShaderStage::Compute, flat);
    ASSERT_TRUE(hs_msl.valid());
    BackendComputePipelineDesc kd{};
    kd.compute_shader_msl = hs_msl.msl_text();
    kd.compute_entry_point = "main0";
    kd.cache_key = 0x7E550001u;
    auto kernel = device.create_compute_pipeline(kd);
    ASSERT_TRUE(kernel && kernel->valid()) << hs_msl.msl_text();

    // Patch-draw pipeline.
    const auto ds = lower(tess_ds_tri, sizeof(tess_ds_tri), ShaderStage::Domain);
    const auto ps_container = parse_dxbc(tess_ps_color, sizeof(tess_ps_color));
    const auto ps = translate_dxbc_to_spirv(ps_container);
    ASSERT_TRUE(ps.valid()) << ps.failure;
    const auto ps_msl = compile_spirv_to_msl(ps.spirv.data(), ps.spirv.size(),
                                             ShaderStage::Fragment, flat);
    ASSERT_TRUE(ps_msl.valid());
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = ds.msl;
    pd.fragment_shader_msl = ps_msl.msl_text();
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::RGBA8Unorm;
    pd.tessellation = true;
    pd.tessellation_partition = BackendTessellationPartition::Integer;
    pd.tessellation_clockwise = true;
    pd.cache_key = 0x7E550002u;
    auto pipeline = device.create_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << ds.msl;

    // Buffers. Control points: CPPOS (o0) and COLOR (o1) of vs_cp.
    const std::array<float, 24> points{
        -0.8f, -0.8f, 0.5f, 1.0f,  1, 0, 0, 1,
         0.0f,  0.8f, 0.5f, 1.0f,  0, 1, 0, 1,
         0.8f, -0.8f, 0.5f, 1.0f,  0, 0, 1, 1,
    };
    const u32 cp_stride = hs.hs_cp_record_vec4s;
    const u32 pc_stride = hs.hs_pc_record_vec4s;
    std::array<u32, 8> control{0, 2, 0, 0, 3, cp_stride, pc_stride, 0};
    const std::array<u32, 3> ids{0, 1, 2};
    const std::array<float, 8> factors_cb{1, 1, 1, 0, 1, 1, 0, 0};
    BackendBufferDesc bd{};
    bd.shader_resource = bd.unordered_access = true;
    const auto make = [&](const void* data, usize bytes) {
        bd.byte_width = bytes;
        return device.create_buffer(bd, data);
    };
    auto cp_in = make(points.data(), sizeof(points));
    auto cp_out = make(nullptr, 3 * cp_stride * 16);
    auto control_buffer = make(control.data(), sizeof(control));
    auto id_buffer = make(ids.data(), sizeof(ids));
    auto pc_out = make(nullptr, pc_stride * 16);
    auto factor_buffer = make(nullptr, 16);
    BackendBufferDesc cbd{};
    cbd.byte_width = sizeof(factors_cb);
    cbd.constant_buffer = true;
    auto cb = device.create_buffer(cbd, factors_cb.data());
    ASSERT_TRUE(cp_in && cp_out && control_buffer && id_buffer && pc_out &&
                factor_buffer && cb);

    BackendDispatchCall dispatch{};
    dispatch.pipeline = kernel.get();
    dispatch.group_count_x = dispatch.group_count_y = dispatch.group_count_z = 1;
    constexpr unsigned base = BACKEND_GS_EXPANSION_BUFFER_BASE;
    dispatch.buffers[base + 0] = cp_in.get();
    dispatch.buffers[base + 1] = cp_out.get();
    dispatch.buffers[base + 2] = control_buffer.get();
    dispatch.buffers[base + 3] = id_buffer.get();
    dispatch.buffers[base + 4] = pc_out.get();
    dispatch.buffers[base + 5] = factor_buffer.get();
    dispatch.buffers[BACKEND_CONSTANT_BUFFER_BASE] = cb.get();
    ASSERT_TRUE(device.dispatch(dispatch));

    BackendTexture2DDesc td{};
    td.width = td.height = 32;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(target->clear_render_target({0.0f, 0.0f, 0.0f, 0.0f}));
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.tessellation_factors = factor_buffer.get();
    call.patch_count = 1;
    call.patch_control_points = 3;
    call.instance_count = 1;
    // The domain shader's buffers sit at the expansion window: 22 + binding.
    call.vertex_buffers[1] = cp_out.get();
    call.vertex_buffers[2] = control_buffer.get();
    call.vertex_buffers[4] = pc_out.get();
    call.viewport = {0, 0, 32, 32, 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    std::vector<u32> texels(32 * 32);
    ASSERT_TRUE(target->readback(0, texels.data(), 128));
    const auto at = [&](u32 x, u32 y) { return texels[y * 32 + x]; };
    u32 covered = 0;
    for (const u32 t : texels) covered += t != 0 ? 1 : 0;
    EXPECT_GT(covered, 200u) << "the patch rasterized";
    // Near each corner, the channel of the barycentric that is ~1 dominates.
    const auto channel = [](u32 texel, u32 c) { return (texel >> (8 * c)) & 0xFFu; };
    const u32 bottom_left = at(5, 27);   // cp0 (-0.8, -0.8)
    const u32 top = at(16, 5);           // cp1 (0, 0.8)
    const u32 bottom_right = at(26, 27); // cp2 (0.8, -0.8)
    std::printf("corners: cp0 %08x cp1 %08x cp2 %08x\n", bottom_left, top,
                bottom_right);
    EXPECT_GT(channel(bottom_left, 0), 100u);
    EXPECT_GT(channel(top, 1), 100u);
    EXPECT_GT(channel(bottom_right, 2), 100u);

    // The patch runs clockwise on screen (cp0 bottom-left, cp1 top, cp2
    // bottom-right) and the hull declares triangle_cw, so its triangles are
    // front faces under FrontCounterClockwise FALSE: CULL_BACK keeps them,
    // CULL_FRONT removes them (Windows d3d11_tess_winding_probe
    // "tri_integer_cw A cull=back ccw=0: front=28", "cull=front ccw=0:
    // front=0 back=0").
    const auto covered_with = [&](BackendCullMode cull) {
        EXPECT_TRUE(target->clear_render_target({0.0f, 0.0f, 0.0f, 0.0f}));
        auto culled_pass = device.create_render_pass_standalone(pass_desc);
        call.cull_mode = cull;
        call.front_counter_clockwise = false;
        EXPECT_TRUE(culled_pass->draw(call));
        EXPECT_TRUE(culled_pass->end());
        EXPECT_TRUE(target->readback(0, texels.data(), 128));
        u32 n = 0;
        for (const u32 t : texels) n += t != 0 ? 1 : 0;
        return n;
    };
    EXPECT_EQ(covered_with(BackendCullMode::Back), covered);
    EXPECT_EQ(covered_with(BackendCullMode::Front), 0u);
}

}  // namespace

// fxc writes `float e[3] : SV_TessFactor` from a fork phase instanced three
// times, as o[vForkInstanceID.x + 0].x (tools/oracle_probe/
// d3d11_a07_fixed_function.hlsl hs_const). Each instance must reach its own
// register: storing to o0 alone left edges 1 and 2 at 0, which culls every
// patch -- d3d11_a07_ia_probe `D patchlist` drew nothing, where Windows
// draws both patches.
#include "../oracle_data/d3d11_a07_shaders.h"

TEST(Tessellation, AForkPhaseWritesTheRegisterItsInstanceIndexes) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const auto container = parse_dxbc(g_hs_main_3, sizeof(g_hs_main_3));
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.emulate_tessellation = true;
    options.gs_input_record_registers = {0};  // vs_cp writes o0 only
    const auto hs = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(hs.valid()) << hs.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(hs.spirv.data(), hs.spirv.size(),
                                          ShaderStage::Compute, flat);
    ASSERT_TRUE(msl.valid());
    BackendComputePipelineDesc kd{};
    kd.compute_shader_msl = msl.msl_text();
    kd.compute_entry_point = "main0";
    kd.cache_key = 0x7E55A07u;
    auto kernel = device.create_compute_pipeline(kd);
    ASSERT_TRUE(kernel && kernel->valid()) << msl.msl_text();

    // Three control points, vs_cp's o0 = (vid, attr, 0, 0) for vid 4..6.
    const std::array<u32, 12> points{4, 1004, 0, 0, 5, 1005, 0, 0,
                                     6, 1006, 0, 0};
    const u32 cp_stride = hs.hs_cp_record_vec4s;
    const u32 pc_stride = hs.hs_pc_record_vec4s;
    const std::array<u32, 8> control{0, 2, 0, 0, 3, cp_stride, pc_stride, 0};
    const std::array<u32, 3> ids{0, 1, 2};
    BackendBufferDesc bd{};
    bd.shader_resource = bd.unordered_access = true;
    const auto make = [&](const void* data, usize bytes) {
        bd.byte_width = bytes;
        return device.create_buffer(bd, data);
    };
    auto cp_in = make(points.data(), sizeof(points));
    auto cp_out = make(nullptr, cp_stride * 16);
    auto control_buffer = make(control.data(), sizeof(control));
    auto id_buffer = make(ids.data(), sizeof(ids));
    auto pc_out = make(nullptr, pc_stride * 16);
    auto factor_buffer = make(nullptr, 16);
    ASSERT_TRUE(cp_in && cp_out && control_buffer && id_buffer && pc_out &&
                factor_buffer);
    BackendDispatchCall dispatch{};
    dispatch.pipeline = kernel.get();
    dispatch.group_count_x = dispatch.group_count_y = dispatch.group_count_z = 1;
    constexpr unsigned base = BACKEND_GS_EXPANSION_BUFFER_BASE;
    dispatch.buffers[base + 0] = cp_in.get();
    dispatch.buffers[base + 1] = cp_out.get();
    dispatch.buffers[base + 2] = control_buffer.get();
    dispatch.buffers[base + 3] = id_buffer.get();
    dispatch.buffers[base + 4] = pc_out.get();
    dispatch.buffers[base + 5] = factor_buffer.get();
    ASSERT_TRUE(device.dispatch(dispatch));
    device.wait_idle();

    // Three edges and the inside factor, all 1.0 as half: 0x3C00.
    std::array<u16, 4> factors{};
    ASSERT_TRUE(factor_buffer->read(0, factors.data(), sizeof(factors)));
    EXPECT_EQ(factors, (std::array<u16, 4>{0x3C00, 0x3C00, 0x3C00, 0x3C00}));
    // INFO = (first, last, sum, SV_PrimitiveID) after the factor registers.
    std::vector<u32> constants(pc_stride * 4);
    ASSERT_TRUE(pc_out->read(0, constants.data(), constants.size() * 4));
    const usize info = (pc_stride - 1) * 4;
    EXPECT_EQ(constants[info + 0], 4u);
    EXPECT_EQ(constants[info + 1], 6u);
    EXPECT_EQ(constants[info + 2], 15u);
}
