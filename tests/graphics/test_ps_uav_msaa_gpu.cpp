// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_ps_uav_msaa_gpu.cpp
/// @brief A pixel shader's UAV side effects on a 4x target, on real Metal,
///        against Windows (WARP).
///
/// D3D11.3 16.3: a pixel shader that reads neither SV_SampleIndex nor a
/// `sample`-interpolated input runs once per pixel, so its u# writes and
/// atomics happen once per covered pixel. Metal runs a fragment function that
/// writes memory once per covered sample; the translator performs the side
/// effects in the invocation at the pixel's lowest covered sample.
///
/// Every expected line is copied from
/// tools/oracle_probe/d3d11_ps_uav_msaa_probe.expected; the shaders are that
/// probe's fxc output (d3d11_ps_uav_msaa_shaders.h, d3d11_ps_uav_msaa.hlsl).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace psuav {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../oracle_data/d3d11_ps_uav_msaa_shaders.h"
#pragma clang diagnostic pop
}  // namespace psuav

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::string translate(const unsigned char* bytecode, usize size,
                      ShaderStage stage) {
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.fixed_function_push = stage == ShaderStage::Fragment;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? std::string(msl.msl_text()) : std::string{};
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

struct Drawn {
    std::vector<u32> px;       ///< 8 words per pixel (the probe's layout)
    std::vector<u32> samples;  ///< 4 samples per pixel
    std::vector<u32> list;     ///< ps_imm's appended elements
    u32 allocated{0};          ///< ps_imm's counter
};

/// The probe's draw: its triangle (depth 0.5) over an 8x8 4x R32_UINT target
/// with @p ps; u1 is the per-pixel record, u2 (with a counter) the list.
bool draw(const unsigned char* ps, usize ps_size, Drawn& out) {
    auto& device = MetalDevice::instance();
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = translate(psuav::g_vs_tri, sizeof(psuav::g_vs_tri),
                                     ShaderStage::Vertex);
    pd.fragment_shader_msl = translate(ps, ps_size, ShaderStage::Fragment);
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::R32Uint;
    pd.sample_count = 4;
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0x95AA0000u + static_cast<u64>(ps_size);
    auto pipeline = device.create_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << pd.fragment_shader_msl;
    if (!pipeline || !pipeline->valid()) return false;

    BackendTexture2DDesc td{};
    td.width = td.height = 8;
    td.mip_levels = td.array_size = 1;
    td.sample_count = 4;
    td.pixel_format = PixelFormat::R32Uint;
    td.render_target = true;
    td.shader_resource = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    const std::array<float, 12> tv{0.3f / 4 - 1, 1 - 0.2f / 4, 0.5f, 1,
                                   7.7f / 4 - 1, 1 - 1.9f / 4, 0.5f, 1,
                                   2.1f / 4 - 1, 1 - 7.6f / 4, 0.5f, 1};
    auto cb = buffer(sizeof(tv), tv.data(), false, true);
    const std::vector<u32> zero(64 * 8, 0u);
    auto px = buffer(zero.size() * 4, zero.data(), true);
    auto list = buffer(64 * 4, zero.data(), true);
    auto counter = buffer(4, zero.data(), true);
    EXPECT_TRUE(target && cb && px && list && counter);
    if (!target || !cb || !px || !list || !counter) return false;

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::R32Uint;
    pass_desc.preserve_color_contents = true;  // a new texture reads 0
    auto pass = device.create_render_pass_standalone(pass_desc);
    EXPECT_TRUE(pass && pass->valid());
    if (!pass || !pass->valid()) return false;
    std::array<u32, kPushBlockBytes / 4> push{};
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.instance_count = 1;
    call.viewport = {0, 0, 8, 8, 0, 1};
    call.vertex_constant_buffers[0] = cb.get();
    call.vertex_constants = push.data();
    call.vertex_constants_size = sizeof(push);
    call.fragment_constants = push.data();
    call.fragment_constants_size = sizeof(push);
    call.fragment_uav_buffers[1] = px.get();
    call.fragment_uav_buffers[2] = list.get();
    call.fragment_uav_counters[0] = counter.get();
    EXPECT_TRUE(pass->draw(call) && pass->end());

    BackendComputePipelineDesc rd{};
    rd.compute_entry_point = "read_samples";
    rd.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void read_samples(texture2d_ms<uint> t [[texture(0)]],
                                 device uint* out [[buffer(0)]],
                                 uint2 p [[thread_position_in_grid]]) {
            for (uint s = 0; s < 4; ++s) {
                out[(p.y * 8 + p.x) * 4 + s] = t.read(p, s).x;
            }
        })";
    rd.cache_key = 0x95AA1000u;
    auto reader = device.create_compute_pipeline(rd);
    EXPECT_TRUE(reader && reader->valid());
    if (!reader || !reader->valid()) return false;
    const std::vector<u32> fill(256, 0xDDDDDDDDu);
    auto samples = buffer(fill.size() * 4, fill.data(), true);
    BackendDispatchCall read{};
    read.pipeline = reader.get();
    read.group_count_x = read.group_count_y = read.group_count_z = 1;
    read.threads_per_group_x = read.threads_per_group_y = 8;
    read.threads_per_group_z = 1;
    read.textures[0] = target.get();
    read.buffers[0] = samples.get();
    EXPECT_TRUE(device.dispatch(read));
    EXPECT_TRUE(device.wait_idle());
    out.px = read_words(*px, 64 * 8);
    out.samples = read_words(*samples, 256);
    out.list = read_words(*list, 64);
    out.allocated = read_words(*counter, 1)[0];
    return true;
}

std::string hex(u32 v) {
    char text[16];
    std::snprintf(text, sizeof text, "%x", v);
    return text;
}

/// "<tag> uav yN:" and "<tag> rt yN:" lines, formatted as the probe prints.
std::vector<std::string> lines(const std::string& tag, const Drawn& d) {
    std::vector<std::string> out;
    for (u32 y = 0; y < 8; ++y) {
        std::string line = tag + " uav y" + std::to_string(y) + ":";
        for (u32 x = 0; x < 8; ++x) {
            const u32* p = d.px.data() + (y * 8 + x) * 8;
            line += " " + hex(p[0]) + "/" + std::to_string(p[1]);
        }
        out.push_back(line);
    }
    for (u32 y = 0; y < 8; ++y) {
        std::string line = tag + " rt y" + std::to_string(y) + ":";
        for (u32 x = 0; x < 8; ++x) {
            const u32* s = d.samples.data() + (y * 8 + x) * 4;
            line += " " + hex(s[0]) + "." + hex(s[1]) + "." + hex(s[2]) + "." +
                    hex(s[3]);
        }
        out.push_back(line);
    }
    return out;
}

void expect_lines(const std::vector<std::string>& got,
                  const std::vector<std::string>& warp) {
    ASSERT_EQ(got.size(), warp.size());
    for (usize i = 0; i < got.size(); ++i) EXPECT_EQ(got[i], warp[i]);
}

}  // namespace

// The probe's "late" section: ps_count (InterlockedOr coverage, InterlockedAdd
// a count, output coverage | 0x100) runs once per covered pixel.
TEST(PsUavMsaaGpu, PixelFrequencyShaderWritesUavsOncePerPixel) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_texture_sample_count(4)) GTEST_SKIP() << "no 4x";
    Drawn d;
    ASSERT_TRUE(draw(psuav::g_ps_count, sizeof(psuav::g_ps_count), d));
    expect_lines(lines("late", d), {
        "late uav y0: a/1 c/1 c/1 0/0 0/0 0/0 0/0 0/0",
        "late uav y1: 2/1 f/1 f/1 f/1 e/1 c/1 c/1 0/0",
        "late uav y2: 2/1 f/1 f/1 f/1 f/1 f/1 f/1 1/1",
        "late uav y3: 0/0 b/1 f/1 f/1 f/1 f/1 1/1 0/0",
        "late uav y4: 0/0 b/1 f/1 f/1 f/1 1/1 0/0 0/0",
        "late uav y5: 0/0 2/1 f/1 f/1 1/1 0/0 0/0 0/0",
        "late uav y6: 0/0 2/1 f/1 1/1 0/0 0/0 0/0 0/0",
        "late uav y7: 0/0 0/0 1/1 0/0 0/0 0/0 0/0 0/0",
        "late rt y0: 0.10a.0.10a 0.0.10c.10c 0.0.10c.10c 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
        "late rt y1: 0.102.0.0 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 0.10e.10e.10e 0.0.10c.10c 0.0.10c.10c 0.0.0.0",
        "late rt y2: 0.102.0.0 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 101.0.0.0",
        "late rt y3: 0.0.0.0 10b.10b.0.10b 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 101.0.0.0 0.0.0.0",
        "late rt y4: 0.0.0.0 10b.10b.0.10b 10f.10f.10f.10f 10f.10f.10f.10f 10f.10f.10f.10f 101.0.0.0 0.0.0.0 0.0.0.0",
        "late rt y5: 0.0.0.0 0.102.0.0 10f.10f.10f.10f 10f.10f.10f.10f 101.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
        "late rt y6: 0.0.0.0 0.102.0.0 10f.10f.10f.10f 101.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
        "late rt y7: 0.0.0.0 0.0.0.0 101.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
    });
}

// The probe's "sample" section: a shader reading SV_SampleIndex runs once per
// covered sample, and every invocation's SV_Coverage is the pixel's whole
// raster coverage (0xf on a fully covered pixel), not its own sample's bit.
TEST(PsUavMsaaGpu, SampleFrequencyShaderRunsPerSampleWithPixelCoverage) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_texture_sample_count(4)) GTEST_SKIP() << "no 4x";
    Drawn d;
    ASSERT_TRUE(draw(psuav::g_ps_count_sample, sizeof(psuav::g_ps_count_sample), d));
    expect_lines(lines("sample", d), {
        "sample uav y0: a/2 c/2 c/2 0/0 0/0 0/0 0/0 0/0",
        "sample uav y1: 2/1 f/4 f/4 f/4 e/3 c/2 c/2 0/0",
        "sample uav y2: 2/1 f/4 f/4 f/4 f/4 f/4 f/4 1/1",
        "sample uav y3: 0/0 b/3 f/4 f/4 f/4 f/4 1/1 0/0",
        "sample uav y4: 0/0 b/3 f/4 f/4 f/4 1/1 0/0 0/0",
        "sample uav y5: 0/0 2/1 f/4 f/4 1/1 0/0 0/0 0/0",
        "sample uav y6: 0/0 2/1 f/4 1/1 0/0 0/0 0/0 0/0",
        "sample uav y7: 0/0 0/0 1/1 0/0 0/0 0/0 0/0 0/0",
        "sample rt y0: 0.110a.0.310a 0.0.210c.310c 0.0.210c.310c 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
        "sample rt y1: 0.1102.0.0 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 0.110e.210e.310e 0.0.210c.310c 0.0.210c.310c 0.0.0.0",
        "sample rt y2: 0.1102.0.0 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 101.0.0.0",
        "sample rt y3: 0.0.0.0 10b.110b.0.310b 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 101.0.0.0 0.0.0.0",
        "sample rt y4: 0.0.0.0 10b.110b.0.310b 10f.110f.210f.310f 10f.110f.210f.310f 10f.110f.210f.310f 101.0.0.0 0.0.0.0 0.0.0.0",
        "sample rt y5: 0.0.0.0 0.1102.0.0 10f.110f.210f.310f 10f.110f.210f.310f 101.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
        "sample rt y6: 0.0.0.0 0.1102.0.0 10f.110f.210f.310f 101.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
        "sample rt y7: 0.0.0.0 0.0.0.0 101.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0 0.0.0.0",
    });
}

// The probe's "imm" section: imm_atomic_iadd / exch / cmp_exch return the
// value before the pixel's one operation (stored + 0x10), the memory takes
// one operation each, and imm_atomic_alloc allocates once per covered pixel.
TEST(PsUavMsaaGpu, ImmediateAtomicsHappenOncePerPixel) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_texture_sample_count(4)) GTEST_SKIP() << "no 4x";
    Drawn d;
    ASSERT_TRUE(draw(psuav::g_ps_imm, sizeof(psuav::g_ps_imm), d));
    const std::array<const char*, 8> warp{
        "11100000", "11111110", "11111111", "01111110",
        "01111100", "01111000", "01110000", "00100000"};
    u32 covered = 0, sum = 0;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const u32* p = d.px.data() + (y * 8 + x) * 8;
            const std::array<u32, 7> want =
                warp[y][x] == '1'
                    ? std::array<u32, 7>{1, 1, 0x10, 7, 0x10, 5, 0x10}
                    : std::array<u32, 7>{0, 0, 0, 0, 0, 0, 0};
            for (u32 i = 0; i < 7; ++i) {
                EXPECT_EQ(p[i + 1], want[i])
                    << "pixel (" << x << ", " << y << ") word " << i + 1;
            }
            if (warp[y][x] == '1') {
                ++covered;
                sum += y * 8 + x + 1;
            }
        }
    }
    // "imm alloc: count=37 sum=991"
    EXPECT_EQ(covered, 37u);
    EXPECT_EQ(d.allocated, 37u);
    u32 listed = 0;
    for (u32 i = 0; i < d.allocated && i < 64; ++i) listed += d.list[i];
    EXPECT_EQ(listed, 991u);
    EXPECT_EQ(sum, 991u);
}

// The three container predicates the gate is decided by, on the probe's own
// shaders (their /Fc listings: dcl_globalFlags, dcl_input_ps_sgv
// sampleIndex, atomic_* / imm_atomic_* / store_*).
TEST(PsUavMsaa, ContainerPredicates) {
    const auto of = [](const unsigned char* code, usize size) {
        return parse_dxbc(code, size);
    };
    const auto count = of(psuav::g_ps_count, sizeof(psuav::g_ps_count));
    const auto early =
        of(psuav::g_ps_count_early, sizeof(psuav::g_ps_count_early));
    const auto sample =
        of(psuav::g_ps_count_sample, sizeof(psuav::g_ps_count_sample));
    const auto imm = of(psuav::g_ps_imm, sizeof(psuav::g_ps_imm));
    const auto vs = of(psuav::g_vs_tri, sizeof(psuav::g_vs_tri));
    EXPECT_TRUE(dxbc_writes_memory(count));
    EXPECT_TRUE(dxbc_writes_memory(early));
    EXPECT_TRUE(dxbc_writes_memory(sample));
    EXPECT_TRUE(dxbc_writes_memory(imm));
    EXPECT_FALSE(dxbc_writes_memory(vs));
    EXPECT_FALSE(dxbc_pixel_runs_per_sample(count));
    EXPECT_FALSE(dxbc_pixel_runs_per_sample(early));
    EXPECT_TRUE(dxbc_pixel_runs_per_sample(sample));
    EXPECT_FALSE(dxbc_pixel_runs_per_sample(imm));
    EXPECT_FALSE(dxbc_forces_early_depth_stencil(count));
    EXPECT_TRUE(dxbc_forces_early_depth_stencil(early));
    EXPECT_FALSE(dxbc_forces_early_depth_stencil(sample));
}

// A pixel-frequency shader with UAV side effects gets the gate and reads
// Metal's [[sample_mask]] unreduced; a sample-frequency one gets no gate, and
// its coverage input is not reduced to its own sample either.
TEST(PsUavMsaa, GateAndCoverageInTheMsl) {
    const std::string count = translate(
        psuav::g_ps_count, sizeof(psuav::g_ps_count), ShaderStage::Fragment);
    EXPECT_NE(count.find("vitreo_uav_leader"), std::string::npos) << count;
    EXPECT_NE(count.find("[[sample_id]]"), std::string::npos) << count;
    EXPECT_EQ(count.find("(1 << gl_SampleID)"), std::string::npos) << count;
    const std::string sample =
        translate(psuav::g_ps_count_sample, sizeof(psuav::g_ps_count_sample),
                  ShaderStage::Fragment);
    EXPECT_EQ(sample.find("vitreo_uav_leader"), std::string::npos) << sample;
    EXPECT_EQ(sample.find("(1 << gl_SampleID)"), std::string::npos) << sample;
}
