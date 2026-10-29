// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_fl111_gpu.cpp
/// @brief The Direct3D 11.1 optional capabilities this device reports at
///        FL 11_0 produce, on real Metal, the values Windows (WARP)
///        produces.
///
/// Every expected value is a line of
/// tools/oracle_probe/d3d11_fl111_probe.expected; the shaders are that
/// probe's fxc output (d3d11_fl111_shaders.h, from d3d11_fl111.hlsl):
///   * msad (SAD4ShaderInstructions, D3D11.3 22.12.15), wrapping on overflow
///     as WARP does;
///   * a cb# read past the *SetConstantBuffers1 window reads 0 (7.5,
///     5.3.4.3.2; ConstantBufferOffsetting);
///   * the sixteen output-merger logic ops on a UINT target, through
///     framebuffer fetch (17.7; OutputMergerLogicOp);
///   * ForcedSampleCount 1 on a 4x target: every sample at the pixel centre
///     (3.5.6.2; MultisampleRTVWithForcedSampleCountOne).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <string>
#include <vector>

namespace fl111 {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../oracle_data/d3d11_fl111_shaders.h"
#pragma clang diagnostic pop
}  // namespace fl111

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::string translate(const unsigned char* bytecode, usize size,
                      ShaderStage stage,
                      const DxbcTranslationOptions& options = {},
                      bool framebuffer_fetch = false) {
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    flat.framebuffer_fetch = framebuffer_fetch;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? std::string(msl.msl_text()) : std::string{};
}

std::unique_ptr<BackendComputePipeline> compute(const std::string& msl,
                                                const char* entry, u64 key) {
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = entry;
    pd.compute_shader_msl = msl;
    pd.cache_key = key;
    return MetalDevice::instance().create_compute_pipeline(pd);
}

std::unique_ptr<BackendBuffer> buffer(usize bytes, const void* data,
                                      bool uav = false, bool cb = false) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = !cb;
    desc.unordered_access = uav;
    desc.constant_buffer = cb;
    return MetalDevice::instance().create_buffer(desc, data);
}

std::vector<u32> read_words(BackendBuffer& b, usize words) {
    std::vector<u32> out(words, 0xDDDDDDDDu);
    EXPECT_TRUE(b.read(0, out.data(), words * 4));
    return out;
}

}  // namespace

TEST(Fl111Gpu, MsadMatchesWarp) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const std::string msl = translate(fl111::g_cs_msad, sizeof(fl111::g_cs_msad),
                                      ShaderStage::Compute);
    auto pipeline = compute(msl, "main0", 0xF1110001u);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl;
    // fl111 "msad ref=... src=...,... accum=...": each row is x = ref,
    // (y, z) = the 8-byte source, w = accum; the shader adds 0, 1, 2 and
    // 0x100 to accum for the four alignments.
    const std::array<u32, 32> in{
        0x04030201, 0x08070605, 0x0C0B0A09, 0,
        0x00FF00FF, 0x12345678, 0x9ABCDEF0, 100,
        0x00000000, 0xFFFFFFFF, 0x12345678, 7,
        0xFFFFFFFF, 0x00000000, 0x00000000, 0,
        0x80008000, 0x7F7F7F7F, 0x01010101, 0xFFFFFFF0,
        0x01020304, 0x01020304, 0x01020304, 0x10,
        0xDEADBEEF, 0xCAFEBABE, 0x0BADF00D, 0x55,
        0x00010000, 0xFF00FF00, 0x00FF00FF, 1,
    };
    const std::array<u32, 32> warp{
        0x10, 0x15, 0x1a, 0x11c,
        0x1b6, 0x1fb, 0x140, 0x272,
        0x7, 0x8, 0x9, 0x107,
        0x3fc, 0x3fd, 0x3fe, 0x4fc,
        0xfffffff2, 0x71, 0x72, 0x1ee,  // wraps (the spec allows either)
        0x10, 0x17, 0x1a, 0x116,
        0xef, 0x1b9, 0x124, 0x29f,
        0x2, 0x100, 0x101, 0x102,
    };
    auto input = buffer(sizeof(in), in.data());
    const std::vector<u32> fill(32, 0xCDCDCDCDu);
    auto output = buffer(fill.size() * 4, fill.data(), true);
    ASSERT_TRUE(input && output);
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 8;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_SRV_BUFFER_BASE] = input.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = output.get();
    ASSERT_TRUE(device.dispatch(call));
    device.wait_idle();
    const auto got = read_words(*output, 32);
    for (usize i = 0; i < warp.size(); ++i) {
        EXPECT_EQ(got[i], warp[i]) << "row " << i / 4 << " component " << i % 4;
    }
}

TEST(Fl111Gpu, ConstantBufferReadsPastTheWindowReadZero) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    DxbcTranslationOptions options{};
    options.guard_constant_buffers = true;
    const std::string msl = translate(fl111::g_cs_cbwin,
                                      sizeof(fl111::g_cs_cbwin),
                                      ShaderStage::Compute, options);
    auto pipeline = compute(msl, "main0", 0xF1110002u);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl;
    // Element i of the 128-element buffer is (0x10000 + 4i, ... + 3).
    std::vector<u32> elements(128 * 4);
    for (u32 i = 0; i < elements.size(); ++i) elements[i] = 0x10000 + i;
    auto cb = buffer(elements.size() * 4, elements.data(), false, true);
    const std::vector<u32> fill(64 * 4, 0xEEEEEEEEu);
    auto output = buffer(fill.size() * 4, fill.data(), true);
    ASSERT_TRUE(cb && output);
    const auto run = [&](u32 first, u32 readable) {
        std::array<u32, kPushBlockBytes / 4> push{};
        push[kConstantBufferLimitOffset / 4] =
            kConstantBufferLimitKnown | readable;
        BackendDispatchCall call{};
        call.pipeline = pipeline.get();
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = 64;
        call.threads_per_group_y = call.threads_per_group_z = 1;
        call.buffers[BACKEND_CONSTANT_BUFFER_BASE] = cb.get();
        call.buffer_offsets[BACKEND_CONSTANT_BUFFER_BASE] = first * 16u;
        call.buffers[BACKEND_UAV_BUFFER_BASE] = output.get();
        call.constants = push.data();
        call.constants_size = sizeof(push);
        EXPECT_TRUE(device.dispatch(call));
        device.wait_idle();
        return read_words(*output, 64 * 4);
    };
    // fl111 "cbwin cs set1 16+16: 16 17 ... 31 - - ...": the frontend binds
    // from FirstConstant and gives the shader 16 readable elements.
    auto got = run(16, 16);
    for (u32 i = 0; i < 64; ++i) {
        const u32 want = i < 16 ? 0x10000 + (16 + i) * 4 : 0u;
        EXPECT_EQ(got[i * 4], want) << "element " << i;
        EXPECT_EQ(got[i * 4 + 3], i < 16 ? want + 3 : 0u) << "element " << i;
    }
    // "cbwin cs set1 112+32 (past end): 112 ... 127 - ...": 16 readable.
    got = run(112, 16);
    EXPECT_EQ(got[15 * 4], 0x10000u + 127u * 4u);
    EXPECT_EQ(got[16 * 4], 0u);
    // "cbwin cs set1 16+0": nothing is readable.
    got = run(16, 0);
    for (u32 i = 0; i < 64; ++i) EXPECT_EQ(got[i * 4], 0u) << "element " << i;
}

namespace {

/// Draws a full-target triangle (fl111 vs_full + ps_logic writing cb0.src)
/// into a 4x1 RGBA8Uint target holding @p dst under logic op @p op, and
/// returns texel 0 as its bytes (R, G, B, A).
std::array<u8, 4> logic_draw(u32 op, const std::array<u8, 4>& dst,
                             const std::array<u32, 4>& src) {
    auto& device = MetalDevice::instance();
    DxbcTranslationOptions ps_options{};
    ps_options.logic_op = op;
    ps_options.logic_op_targets = 1;
    ps_options.logic_op_component_masks[0] = {0xFF, 0xFF, 0xFF, 0xFF};
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = translate(fl111::g_vs_full, sizeof(fl111::g_vs_full),
                                     ShaderStage::Vertex);
    pd.fragment_shader_msl =
        translate(fl111::g_ps_logic, sizeof(fl111::g_ps_logic),
                  ShaderStage::Fragment, ps_options, /*framebuffer_fetch=*/true);
    EXPECT_NE(pd.fragment_shader_msl.find("[[color(0)]]"), std::string::npos);
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::RGBA8Uint;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0xF1110100u + op;
    auto pipeline = device.create_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << pd.fragment_shader_msl;
    if (!pipeline || !pipeline->valid()) return {};
    std::array<u8, 16> texels{};
    for (u32 x = 0; x < 4; ++x) std::copy(dst.begin(), dst.end(), &texels[x * 4]);
    BackendTexture2DDesc td{};
    td.width = 4;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Uint;
    td.render_target = true;
    td.shader_resource = true;
    auto target = device.create_texture_2d(td, texels.data(), 16);
    auto cb = buffer(16, src.data(), false, true);
    EXPECT_TRUE(target && cb);
    if (!target || !cb) return {};
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Uint;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    EXPECT_TRUE(pass && pass->valid());
    if (!pass || !pass->valid()) return {};
    const std::array<u32, kPushBlockBytes / 4> push{};
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.instance_count = 1;
    call.viewport = {0, 0, 4, 1, 0, 1};
    call.fragment_constant_buffers[0] = cb.get();
    call.fragment_constants = push.data();
    call.fragment_constants_size = sizeof(push);
    call.vertex_constants = push.data();
    call.vertex_constants_size = sizeof(push);
    EXPECT_TRUE(pass->draw(call) && pass->end());
    std::array<u8, 16> out{};
    EXPECT_TRUE(target->readback_slice(0, 0, out.data(), 16));
    for (u32 x = 1; x < 4; ++x) {
        EXPECT_TRUE(std::equal(out.begin(), out.begin() + 4, &out[x * 4]))
            << "texel " << x;
    }
    return {out[0], out[1], out[2], out[3]};
}

}  // namespace

TEST(Fl111Gpu, LogicOpsMatchWarp) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_framebuffer_fetch()) {
        GTEST_SKIP() << "no framebuffer fetch: OutputMergerLogicOp is FALSE";
    }
    // fl111 "logic rgba8 dst=cc550fcc src=aa,aa,f0,0": op -> R G B A.
    const std::array<std::array<u8, 4>, 16> warp{{
        {0x00, 0x00, 0x00, 0x00}, {0xff, 0xff, 0xff, 0xff},
        {0xaa, 0xaa, 0xf0, 0x00}, {0x55, 0x55, 0x0f, 0xff},
        {0xcc, 0x55, 0x0f, 0xcc}, {0x33, 0xaa, 0xf0, 0x33},
        {0x88, 0x00, 0x00, 0x00}, {0x77, 0xff, 0xff, 0xff},
        {0xee, 0xff, 0xff, 0xcc}, {0x11, 0x00, 0x00, 0x33},
        {0x66, 0xff, 0xff, 0xcc}, {0x99, 0x00, 0x00, 0x33},
        {0x22, 0xaa, 0xf0, 0x00}, {0x44, 0x55, 0x0f, 0xcc},
        {0xbb, 0xaa, 0xf0, 0x33}, {0xdd, 0x55, 0x0f, 0xff},
    }};
    for (u32 op = 0; op < 16; ++op) {
        EXPECT_EQ(logic_draw(op, {0xcc, 0x55, 0x0f, 0xcc}, {0xaa, 0xaa, 0xf0, 0}),
                  warp[op])
            << "D3D11_LOGIC_OP " << op;
    }
    // "logic rgba8 COPY src 1aa,100,ffffffff,7f: aa00ff7f": the source is
    // truncated to the component's bits.
    EXPECT_EQ(logic_draw(2, {0x0f, 0x0f, 0x0f, 0x0f},
                         {0x1aa, 0x100, 0xffffffff, 0x7f}),
              (std::array<u8, 4>{0xaa, 0x00, 0xff, 0x7f}));
}

TEST(Fl111Gpu, ForcedSingleSampleRasterizesPixelCentres) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_programmable_sample_positions() ||
        !device.supports_texture_sample_count(4)) {
        GTEST_SKIP() << "MultisampleRTVWithForcedSampleCountOne is FALSE";
    }
    DxbcTranslationOptions ps_options{};
    ps_options.fixed_function_push = true;
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = translate(fl111::g_vs_tri, sizeof(fl111::g_vs_tri),
                                     ShaderStage::Vertex);
    pd.fragment_shader_msl =
        translate(fl111::g_ps_cov_rt, sizeof(fl111::g_ps_cov_rt),
                  ShaderStage::Fragment, ps_options);
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::R32Uint;
    pd.sample_count = 4;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0xF1110200u;
    auto pipeline = device.create_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << pd.fragment_shader_msl;
    BackendTexture2DDesc td{};
    td.width = td.height = 8;
    td.mip_levels = td.array_size = 1;
    td.sample_count = 4;
    td.pixel_format = PixelFormat::R32Uint;
    td.render_target = true;
    td.shader_resource = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    // The probe's triangle, in pixels (0.3, 0.2), (7.7, 1.9), (2.1, 7.6).
    const std::array<float, 12> tv{0.3f / 4 - 1, 1 - 0.2f / 4, 0, 1,
                                   7.7f / 4 - 1, 1 - 1.9f / 4, 0, 1,
                                   2.1f / 4 - 1, 1 - 7.6f / 4, 0, 1};
    auto cb = buffer(sizeof(tv), tv.data(), false, true);
    ASSERT_TRUE(target && cb);  // a new texture reads 0 (texture_zero_init)
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::R32Uint;
    pass_desc.preserve_color_contents = true;
    pass_desc.center_sample_positions = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_TRUE(pass && pass->valid());
    std::array<u32, kPushBlockBytes / 4> push{};
    push[kRasterizerSampleCountOffset / 4] = 1;  // the forced count
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.instance_count = 1;
    call.viewport = {0, 0, 8, 8, 0, 1};
    call.vertex_constant_buffers[0] = cb.get();
    call.fragment_constants = push.data();
    call.fragment_constants_size = sizeof(push);
    call.vertex_constants = push.data();
    call.vertex_constants_size = sizeof(push);
    ASSERT_TRUE(pass->draw(call) && pass->end());

    // Read every sample back.
    auto reader = compute(R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void read_samples(texture2d_ms<uint> t [[texture(0)]],
                                 device uint* out [[buffer(0)]],
                                 uint2 p [[thread_position_in_grid]]) {
            for (uint s = 0; s < 4; ++s) {
                out[(p.y * 8 + p.x) * 4 + s] = t.read(p, s).x;
            }
        })", "read_samples", 0xF1110201u);
    ASSERT_TRUE(reader && reader->valid());
    const std::vector<u32> fill(256, 0xDDDDDDDDu);
    auto samples = buffer(fill.size() * 4, fill.data(), true);
    BackendDispatchCall read{};
    read.pipeline = reader.get();
    read.group_count_x = read.group_count_y = read.group_count_z = 1;
    read.threads_per_group_x = read.threads_per_group_y = 8;
    read.threads_per_group_z = 1;
    read.textures[0] = target.get();
    read.buffers[0] = samples.get();
    ASSERT_TRUE(device.dispatch(read));
    device.wait_idle();
    const auto got = read_words(*samples, 256);
    // fl111 "forced rt forced=1 samples=4 y0..y7": where the pixel centre is
    // covered every sample holds SV_Coverage 1 | 0x100, elsewhere 0.
    const std::array<const char*, 8> warp{
        "11000000", "01111100", "01111110", "01111100",
        "01111000", "00110000", "00100000", "00000000"};
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const u32 want = warp[y][x] == '1' ? 0x101u : 0u;
            for (u32 s = 0; s < 4; ++s) {
                EXPECT_EQ(got[(y * 8 + x) * 4 + s], want)
                    << "pixel (" << x << ", " << y << ") sample " << s;
            }
        }
    }
}
