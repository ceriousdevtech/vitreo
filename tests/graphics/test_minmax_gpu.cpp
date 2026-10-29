// SPDX-License-Identifier: LGPL-2.1-only
//
// MINIMUM_ / MAXIMUM_ reduction filtering (D3D11.3 5.9.4.5.6), reduced in
// the translated shader (DxbcTranslationOptions::minmax_samplers), on real
// Metal. The shader is tools/oracle_probe/d3d11_minmax_probe's cs_main (fxc,
// d3d11_minmax_shaders.h) and every expected value is a line of
// d3d11_minmax_probe.expected (Windows, WARP): its 256x16 five-mip texture
// holds (x, y, mip, 1) in every texel, so a result names the extreme texels
// the filter footprint included.
//
//   AnisotropicFootprint...  [aniso] lines: the probe placement (N probes
//                            r / (N + 1) apart along the major axis), axis
//                            and diagonal, one and two mips.
//   ProbePositions...        [reach] lines: the step of a 1/256-texel sweep
//                            where the extreme changes (each probe floored
//                            to 1/256 texel, including at a mip-1 centre on
//                            the half grid).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "../oracle_data/d3d11_minmax_shaders.h"

#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kMinAniso = 0x155;  // D3D11_FILTER_MINIMUM_ANISOTROPIC
constexpr u32 kMaxAniso = 0x1D5;  // D3D11_FILTER_MAXIMUM_ANISOTROPIC
constexpr float kDiag = 0.70710678f;

/// The probe's `Case`: a = coordinate, b / c = gradients, m = (mode, texture,
/// 0, 2 * count); mode 1 is SampleGrad and texture 6 the 256x16 one at t7.
struct Case {
    std::array<float, 4> a{};
    std::array<float, 4> b{};
    std::array<float, 4> c{};
    std::array<u32, 4> m{};
};
static_assert(sizeof(Case) == 64);

Case grad(float u, float v, float dxu, float dxv, float dyu, float dyv) {
    Case k{};
    k.a = {u, v, 0.0f, 0.0f};
    k.b = {dxu, dxv, 0.0f, 0.0f};
    k.c = {dyu, dyv, 0.0f, 0.0f};
    k.m = {1u, 6u, 0u, 0u};
    return k;
}

/// The major axis `length` texels (of the 256x16 mip 0) along the diagonal,
/// the minor one a texel across it, as the probe's section_aniso and
/// section_reach build them.
Case diagonal(float u_texel, float v_texel, float length) {
    return grad(u_texel / 256, v_texel / 16, length * kDiag / 256,
                length * kDiag / 16, -kDiag / 256, kDiag / 16);
}
Case along_u(float u_texel, float v_texel, float length) {
    return grad(u_texel / 256, v_texel / 16, length / 256, 0.0f, 0.0f,
                1.0f / 16);
}

class MinMaxGpu {
public:
    MinMaxGpu() {
        auto& device = MetalDevice::instance();
        DxbcTranslationOptions options{};
        options.minmax_samplers = 1;  // s0
        const auto container = parse_dxbc(g_mm_cs_main, sizeof(g_mm_cs_main));
        EXPECT_TRUE(container.valid());
        const auto spirv = translate_dxbc_to_spirv(container, options);
        EXPECT_TRUE(spirv.valid()) << spirv.failure;
        if (!spirv.valid()) return;
        MslCompileOptions flat{};
        flat.argument_buffers = false;
        const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                              spirv.spirv.size(),
                                              ShaderStage::Compute, flat);
        EXPECT_TRUE(msl.valid());
        BackendComputePipelineDesc pd{};
        pd.compute_shader_msl = msl.msl_text();
        pd.compute_entry_point = "main0";
        pd.cache_key = 0x4D4D0001u;
        m_pipeline = device.create_compute_pipeline(pd);
        EXPECT_TRUE(m_pipeline && m_pipeline->valid()) << msl.msl_text();

        BackendTexture2DDesc td{};
        td.width = 256;
        td.height = 16;
        td.mip_levels = 5;
        td.array_size = 1;
        td.pixel_format = PixelFormat::RGBA32Float;
        td.shader_resource = true;
        std::vector<std::vector<float>> levels;
        for (u32 mip = 0; mip < 5; ++mip) {
            const u32 w = 256u >> mip, h = 16u >> mip;
            std::vector<float> texels(static_cast<usize>(w) * h * 4);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    float* t = texels.data() + (static_cast<usize>(y) * w + x) * 4;
                    t[0] = static_cast<float>(x);
                    t[1] = static_cast<float>(y);
                    t[2] = static_cast<float>(mip);
                    t[3] = 1.0f;
                }
            }
            levels.push_back(std::move(texels));
        }
        m_texture = device.create_texture_2d(td, levels[0].data(), 256 * 16);
        EXPECT_TRUE(m_texture);
        for (u32 mip = 1; m_texture && mip < 5; ++mip) {
            const u32 w = 256u >> mip, h = 16u >> mip;
            EXPECT_TRUE(m_texture->update_region(mip, 0, levels[mip].data(),
                                                 w * 16, 0, 0, w, h));
        }
    }

    /// Each case through s0 = @p filter (CLAMP, MinLOD 0, MaxLOD FLT_MAX)
    /// with @p max_anisotropy; the results' rows.
    std::vector<std::array<float, 4>> run(u32 filter, u32 max_anisotropy,
                                          std::vector<Case> cases) {
        auto& device = MetalDevice::instance();
        const u32 n = static_cast<u32>(cases.size());
        for (auto& k : cases) k.m[3] = 2 * n;
        BackendBufferDesc cd{};
        cd.byte_width = cases.size() * sizeof(Case);
        cd.shader_resource = true;
        auto input = device.create_buffer(cd, cases.data());
        const std::vector<u32> poison(n * 4, 0xDDDDDDDDu);
        BackendBufferDesc od{};
        od.byte_width = n * 16;
        od.shader_resource = true;
        od.unordered_access = true;
        auto output = device.create_buffer(od, poison.data());
        EXPECT_TRUE(input && output && m_pipeline && m_texture);
        if (!input || !output || !m_pipeline || !m_texture) return {};

        BackendDispatchCall call{};
        call.pipeline = m_pipeline.get();
        call.group_count_x = n;
        call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = call.threads_per_group_y =
            call.threads_per_group_z = 1;
        call.textures[BACKEND_SRV_TEXTURE_BASE + 7] = m_texture.get();
        auto& view = call.texture_views[BACKEND_SRV_TEXTURE_BASE + 7];
        view.dimension = BackendTextureViewDimension::Texture2D;
        view.mip_levels = 5;
        view.array_size = 1;
        call.buffers[BACKEND_SRV_BUFFER_BASE + 6] = input.get();
        call.buffers[BACKEND_UAV_BUFFER_BASE] = output.get();
        // The Metal sampler the frontend resolves for a reduction filter
        // (resolve_sampler): point-sampled texels, no anisotropy, no LOD
        // clamp of its own; the shader reads the D3D11 state from push
        // member 13 (fill_minmax_samplers).
        BackendSamplerDesc sampler{};
        sampler.min_filter = sampler.mag_filter = sampler.mip_filter =
            BackendFilter::Nearest;
        sampler.address_u = sampler.address_v = sampler.address_w =
            BackendAddressMode::Clamp;
        sampler.lod_min_clamp = 0.0f;
        sampler.lod_max_clamp = std::numeric_limits<float>::max();
        call.samplers[0] = sampler;
        call.sampler_bound[0] = true;
        std::array<u32, kPushBlockBytes / 4> push{};
        u32* entry = push.data() + kMinMaxSamplerOffset / 4;
        entry[0] = filter;
        entry[1] = std::bit_cast<u32>(0.0f);
        entry[2] = std::bit_cast<u32>(3.402823466e+38f);
        u32 effective = 1;
        while (effective * 2 <= max_anisotropy) effective *= 2;
        entry[3] = effective;
        call.constants = push.data();
        call.constants_size = sizeof(push);
        EXPECT_TRUE(device.dispatch(call));
        EXPECT_TRUE(device.wait_idle());
        std::vector<u32> words(n * 4, 0xDDDDDDDDu);
        EXPECT_TRUE(output->read(0, words.data(), words.size() * 4));
        std::vector<std::array<float, 4>> rows(n);
        for (u32 i = 0; i < n * 4; ++i) {
            rows[i / 4][i % 4] = std::bit_cast<float>(words[i]);
        }
        return rows;
    }

private:
    std::unique_ptr<BackendComputePipeline> m_pipeline;
    std::unique_ptr<BackendTexture2D> m_texture;
};

void expect_row(const std::array<float, 4>& got,
                const std::array<float, 4>& warp, const char* line) {
    for (u32 c = 0; c < 4; ++c) {
        EXPECT_EQ(got[c], warp[c]) << line << " component " << c;
    }
}

}  // namespace

TEST(MinMaxGpu, AnisotropicFootprintMatchesWarp) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    MinMaxGpu gpu;
    // "X [MAX_ANISO a3 x8] grad t6 (0.391797,0.5375,0) 0
    //  d(0.0441942,0.707107,0)(-0.00276214,0.0441942,0): 52 6 1 1"
    // (ratio 16 clamped to 8, LOD 1: eight probes 16/9 texels apart).
    auto rows = gpu.run(kMaxAniso, 8, {diagonal(100.3f, 8.6f, 16.0f)});
    ASSERT_EQ(rows.size(), 1u);
    expect_row(rows[0], {52, 6, 1, 1}, "MAX x8 diagonal 16");
    // "X [MIN_ANISO a3 x8] grad t6 (0.391797,0.5375,0) 0
    //  d(0.0110485,0.176777,0)(-0.00276214,0.0441942,0): 98 7 0 1"
    rows = gpu.run(kMinAniso, 8, {diagonal(100.3f, 8.6f, 4.0f)});
    ASSERT_EQ(rows.size(), 1u);
    expect_row(rows[0], {98, 7, 0, 1}, "MIN x8 diagonal 4");
    // "X [MAX_ANISO a3 x2] grad t6 (0.391797,0.5375,0) 0
    //  d(0.0110485,0.176777,0)(-0.00276214,0.0441942,0): 50 5 1 1"
    // (ratio <= 2: two probes (r - 1) / 2 either side).
    rows = gpu.run(kMaxAniso, 2, {diagonal(100.3f, 8.6f, 4.0f)});
    ASSERT_EQ(rows.size(), 1u);
    expect_row(rows[0], {50, 5, 1, 1}, "MAX x2 diagonal 4");
    // "X [MAX_ANISO a3 x16] grad t6 (0.394531,0.53125,0) 0
    //  d(0.0625,0,0)(0,0.0625,0): 108 8 0 1"
    rows = gpu.run(kMaxAniso, 16, {along_u(101.0f, 8.5f, 16.0f)});
    ASSERT_EQ(rows.size(), 1u);
    expect_row(rows[0], {108, 8, 0, 1}, "MAX x16 along u 16");
    // "X [MIN_ANISO a3 x4] grad t6 (0.393555,0.53125,0) 0
    //  d(0.0117188,0,0)(0,0.0625,0): 99 8 0 1"
    rows = gpu.run(kMinAniso, 4, {along_u(100.75f, 8.5f, 3.0f)});
    ASSERT_EQ(rows.size(), 1u);
    expect_row(rows[0], {99, 8, 0, 1}, "MIN x4 along u 3");
}

TEST(MinMaxGpu, ProbePositionsAreFlooredToA256thOfATexel) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    MinMaxGpu gpu;
    // [reach]: the centre at u = 100 + j/256 texels, v = 8.5; the line lists
    // "step:extreme x". Each pair below straddles one change.
    const auto x_at = [&](u32 filter, u32 aniso, bool diag, float length,
                          u32 j) {
        const float u = 100.0f + static_cast<float>(j) / 256.0f;
        const auto rows = gpu.run(
            filter, aniso,
            {diag ? diagonal(u, 8.5f, length) : along_u(u, 8.5f, length)});
        return rows.empty() ? -1.0f : rows[0][0];
    };
    // "R [MIN_ANISO x8] diag L=5: 0:98 224:99"
    EXPECT_EQ(x_at(kMinAniso, 8, true, 5.0f, 223), 98.0f);
    EXPECT_EQ(x_at(kMinAniso, 8, true, 5.0f, 224), 99.0f);
    // "R [MAX_ANISO x16] diag L=16: 0:105 132:106"
    EXPECT_EQ(x_at(kMaxAniso, 16, true, 16.0f, 131), 105.0f);
    EXPECT_EQ(x_at(kMaxAniso, 16, true, 16.0f, 132), 106.0f);
    // "R [MAX_ANISO x4] axis L=8: 0:51 156:52" (LOD 1: the mip-1 centre is
    // on the half grid, and the probe is floored, not rounded).
    EXPECT_EQ(x_at(kMaxAniso, 4, false, 8.0f, 155), 51.0f);
    EXPECT_EQ(x_at(kMaxAniso, 4, false, 8.0f, 156), 52.0f);
}
