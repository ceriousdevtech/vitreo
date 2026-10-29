// SPDX-License-Identifier: LGPL-2.1-only
//
// Views whose use had no oracle, and the line modes, on real Metal. Every
// expected value is a line of tools/oracle_probe/d3d11_view_line_probe.expected
// (Windows, WARP); the shaders are that probe's fxc output
// (d3d11_view_line_shaders.h).
//
//   ComparisonBorder...   sample_c through a border whose red is not 0 or 1
//                         (D3D11.3 7.18.9.1), completed through the twin.
//   SingleSample...       ld2dms / sampleinfo / samplepos / resinfo of a
//                         Texture2DMS(Array) declaration over one-sample
//                         textures (DxbcTranslationOptions::
//                         single_sample_ms_srvs).
//   BufferTextureRow...   the copies a buffer render-target view draws
//                         through, at texel-aligned (not 16-byte) offsets.
//   VolumeView...         an R32_UINT view of one mip of an RGBA8 volume
//                         (functional spec 5.3.9.5 casts).
//   LineQuad...           the line-quad pass C shaders compile.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "../oracle_data/d3d11_view_line_shaders.h"

#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::unique_ptr<BackendComputePipeline> compile(
    const unsigned char* blob, usize size,
    const DxbcTranslationOptions& options, u64 key, std::string* msl_out) {
    const auto container = parse_dxbc(blob, size);
    EXPECT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return nullptr;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
    EXPECT_TRUE(msl.valid());
    if (msl_out != nullptr) *msl_out = msl.msl_text();
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = key;
    auto pipeline = MetalDevice::instance().create_compute_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();
    return pipeline;
}

std::unique_ptr<BackendBuffer> buffer(usize bytes, const void* data) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = true;
    desc.unordered_access = true;
    return MetalDevice::instance().create_buffer(desc, data);
}

std::vector<u32> read_words(BackendBuffer& b, usize words) {
    std::vector<u32> out(words, 0xDDDDDDDDu);
    EXPECT_TRUE(b.read(0, out.data(), words * 4));
    return out;
}

using Rows = std::array<std::array<float, 4>, 8>;

/// cs_cmp: eight uvs x refs .2/.45/.55/.9 through s0 over a 2x2 D32 texture
/// {.1, .3; .6, .8}, s0 comparing with @p func through a BORDER of red
/// @p border.
Rows compare_through_border(BackendFilter filter, BackendCompare compare,
                            u32 func, float border) {
    auto& device = MetalDevice::instance();
    DxbcTranslationOptions options{};
    options.border_twin_samplers = 1;
    const auto pipeline =
        compile(g_cs_cmp, sizeof(g_cs_cmp), options, 0xC3B00001u, nullptr);
    const std::array<float, 4> texels{0.1f, 0.3f, 0.6f, 0.8f};
    BackendTexture2DDesc td{};
    td.width = td.height = 2;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::D32Float;
    td.shader_resource = true;
    auto depth = device.create_texture_2d(td, texels.data(), 8);
    const std::vector<u32> poison(32, 0xDDDDDDDDu);
    auto results = buffer(32 * 4, poison.data());
    if (!pipeline || !depth || !results) return {};

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.textures[BACKEND_SRV_TEXTURE_BASE] = depth.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].dimension =
        BackendTextureViewDimension::Texture2D;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].array_size = 1;
    BackendSamplerDesc sampler{};
    sampler.min_filter = sampler.mag_filter = filter;
    sampler.mipmaps_enabled = false;
    sampler.address_u = sampler.address_v = sampler.address_w =
        BackendAddressMode::Border;
    sampler.border_color = 0;  // the twin's partner: border 0
    sampler.comparison_enabled = true;
    sampler.comparison = compare;
    call.samplers[0] = sampler;
    call.sampler_bound[0] = true;
    call.border_twin_mask = 1;
    // Push member 1, s0: (red, D3D11_COMPARISON_FUNC, 0, 0), as the
    // frontend fills it (border_twin_push_colour).
    std::array<u32, kPushBlockBytes / 4> push{};
    push[16] = std::bit_cast<u32>(border);
    push[17] = std::bit_cast<u32>(static_cast<float>(func));
    call.constants = push.data();
    call.constants_size = sizeof(push);
    call.buffers[BACKEND_UAV_BUFFER_BASE] = results.get();
    EXPECT_TRUE(device.dispatch(call));
    EXPECT_TRUE(device.wait_idle());
    const auto words = read_words(*results, 32);
    Rows rows{};
    for (u32 i = 0; i < 32; ++i) {
        rows[i / 4][i % 4] = std::bit_cast<float>(words[i]);
    }
    return rows;
}

void expect_rows(const Rows& got, const Rows& warp, const char* what) {
    for (u32 r = 0; r < 8; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            EXPECT_NEAR(got[r][c], warp[r][c], 1.0f / 256.0f)
                << what << " uv#" << r << " ref " << c;
        }
    }
}

}  // namespace

TEST(ViewLineGpu, ComparisonBorderRedMatchesWarp) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
    // [cmp] "R32_FLOAT linear LESS border .5".
    expect_rows(compare_through_border(BackendFilter::Linear,
                                       BackendCompare::Less, 2, 0.5f),
                Rows{{{1, 1, 0, 0}, {0, 0, 0, 0}, {.5f, .5f, 0, 0},
                      {.75f, .75f, 0, 0}, {.25f, .25f, 0, 0},
                      {.75f, .75f, 0, 0}, {1, 1, 0, 0}, {1, 1, .5f, 0}}},
                "linear LESS .5");
    // "R32_FLOAT linear GREATER_EQUAL border .5".
    expect_rows(compare_through_border(BackendFilter::Linear,
                                       BackendCompare::GreaterEqual, 7, 0.5f),
                Rows{{{0, 0, 1, 1}, {1, 1, 1, 1}, {.5f, .5f, 1, 1},
                      {.25f, .25f, 1, 1}, {.75f, .75f, 1, 1},
                      {.25f, .25f, 1, 1}, {0, 0, 1, 1}, {0, 0, .5f, 1}}},
                "linear GREATER_EQUAL .5");
    // "R32_FLOAT point LESS border 1.5": the red is compared unclamped.
    expect_rows(compare_through_border(BackendFilter::Nearest,
                                       BackendCompare::Less, 2, 1.5f),
                Rows{{{1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1},
                      {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1},
                      {1, 1, 1, 1}}},
                "point LESS 1.5");
    // "R32_FLOAT point GREATER border -.5": every ref is greater.
    expect_rows(compare_through_border(BackendFilter::Nearest,
                                       BackendCompare::Greater, 5, -0.5f),
                Rows{{{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1},
                      {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1},
                      {1, 1, 1, 1}}},
                "point GREATER -.5");
}

TEST(ViewLineGpu, SingleSampleMultisampleLoadsMatchWarp) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    DxbcTranslationOptions options{};
    options.single_sample_ms_srvs = 0x3;  // t0 and t1
    std::string msl;
    const auto pipeline =
        compile(g_cs_ms1, sizeof(g_cs_ms1), options, 0x5A510001u, &msl);
    ASSERT_TRUE(pipeline);
    EXPECT_EQ(msl.find("texture2d_ms"), std::string::npos) << msl;
    // The probe's texels: (x, y) of slice s = {0x11 (x + 1), 0x10 y + 2,
    // 0x40 + 0x20 s + i, 0x80}, i = 4y + x.
    std::array<std::array<u32, 16>, 2> texels{};
    for (u32 s = 0; s < 2; ++s) {
        for (u32 i = 0; i < 16; ++i) {
            texels[s][i] = (0x11u * ((i & 3u) + 1u)) |
                           ((0x10u * (i >> 2) + 2u) << 8) |
                           ((0x40u + 0x20u * s + i) << 16) | (0x80u << 24);
        }
    }
    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.shader_resource = true;
    auto single = device.create_texture_2d(td, texels[0].data(), 16);
    td.array_size = 2;
    auto slices = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(single && slices);
    ASSERT_TRUE(slices->update_slice(0, 0, texels[0].data(), 16));
    ASSERT_TRUE(slices->update_slice(0, 1, texels[1].data(), 16));
    const std::vector<u32> poison(27 * 4, 0xDDDDDDDDu);
    auto results = buffer(27 * 16, poison.data());
    ASSERT_TRUE(results);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    // What resolve_shader_resource binds for a 2DMS / 2DMSARRAY view of a
    // one-sample texture: its 2D mip.
    call.textures[BACKEND_SRV_TEXTURE_BASE] = single.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].dimension =
        BackendTextureViewDimension::Texture2D;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE].array_size = 1;
    call.textures[BACKEND_SRV_TEXTURE_BASE + 1] = slices.get();
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 1].dimension =
        BackendTextureViewDimension::Texture2DArray;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 1].mip_levels = 1;
    call.texture_views[BACKEND_SRV_TEXTURE_BASE + 1].array_size = 2;
    call.buffers[BACKEND_UAV_BUFFER_BASE] = results.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    const auto got = read_words(*results, 27 * 4);
    // [ms1] "ms1 cs[..]", Windows x64.
    const std::array<std::array<u32, 4>, 27> warp{{
        {0x00000004, 0x00000004, 0x00000001, 0x00000000},
        {0x00000004, 0x00000004, 0x00000002, 0x00000001},
        {0x3d888889, 0x3c008081, 0x3e808081, 0x3f008081},
        {0x3e088889, 0x3c008081, 0x3e828283, 0x3f008081},
        {0x3e4ccccd, 0x3c008081, 0x3e848485, 0x3f008081},
        {0x3e888889, 0x3c008081, 0x3e868687, 0x3f008081},
        {0x3d888889, 0x3d909091, 0x3e888889, 0x3f008081},
        {0x3e088889, 0x3d909091, 0x3e8a8a8b, 0x3f008081},
        {0x3e4ccccd, 0x3d909091, 0x3e8c8c8d, 0x3f008081},
        {0x3e888889, 0x3d909091, 0x3e8e8e8f, 0x3f008081},
        {0x3d888889, 0x3e088889, 0x3e909091, 0x3f008081},
        {0x3e088889, 0x3e088889, 0x3e929293, 0x3f008081},
        {0x3e4ccccd, 0x3e088889, 0x3e949495, 0x3f008081},
        {0x3e888889, 0x3e088889, 0x3e969697, 0x3f008081},
        {0x3d888889, 0x3e48c8c9, 0x3e989899, 0x3f008081},
        {0x3e088889, 0x3e48c8c9, 0x3e9a9a9b, 0x3f008081},
        {0x3e4ccccd, 0x3e48c8c9, 0x3e9c9c9d, 0x3f008081},
        {0x3e888889, 0x3e48c8c9, 0x3e9e9e9f, 0x3f008081},
        {0x3e088889, 0x3d909091, 0x3e8a8a8b, 0x3f008081},  // sample 1
        {0x3e088889, 0x3d909091, 0x3e8a8a8b, 0x3f008081},  // sample 3
        {0, 0, 0, 0},                                      // x 4: outside
        {0, 0, 0, 0},                                      // samplepos 0, 1
        {0x3e088889, 0x3e088889, 0x3e929293, 0x3f008081},
        {0x3e088889, 0x3e088889, 0x3ed2d2d3, 0x3f008081},
        {0x3e888889, 0x3c008081, 0x3ec6c6c7, 0x3f008081},
        {0, 0, 0, 0},                                      // slice 2
        {0x3e088889, 0x3e088889, 0x3ed2d2d3, 0x3f008081},
    }};
    for (u32 r = 0; r < warp.size(); ++r) {
        for (u32 c = 0; c < 4; ++c) {
            EXPECT_EQ(got[r * 4 + c], warp[r][c])
                << "cs[" << r << "] component " << c;
        }
    }
}

TEST(ViewLineGpu, BufferTextureRowCopiesAtTexelAlignedOffsets) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    // [bufrt] "R16_UINT {5,3}": elements 5..7 start at byte 10, and
    // "R8G8B8A8_UNORM {3,5}" at byte 12 -- neither 16-byte aligned, which
    // is why the view draws through a proxy rather than a linear alias.
    std::vector<u8> bytes(64);
    for (u32 i = 0; i < 64; ++i) bytes[i] = static_cast<u8>(0xA0 + i);
    auto storage = buffer(64, bytes.data());
    BackendTexture2DDesc td{};
    td.width = 3;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::R16Uint;
    td.render_target = true;
    auto proxy = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(storage && proxy);
    ASSERT_TRUE(device.copy_buffer_texture_row(storage.get(), 10, proxy.get(),
                                               3, true));
    ASSERT_TRUE(device.copy_buffer_texture_row(storage.get(), 40, proxy.get(),
                                               3, false));
    EXPECT_FALSE(device.copy_buffer_texture_row(storage.get(), 11,
                                                proxy.get(), 3, false))
        << "an offset that is not a whole texel";
    ASSERT_TRUE(device.wait_idle());
    std::vector<u8> after(64);
    ASSERT_TRUE(storage->read(0, after.data(), after.size()));
    for (u32 i = 0; i < 6; ++i) {
        EXPECT_EQ(after[40 + i], 0xA0 + 10 + i) << "byte " << 40 + i;
    }
    EXPECT_EQ(after[39], 0xA0 + 39) << "outside the window";
    EXPECT_EQ(after[46], 0xA0 + 46) << "outside the window";
}

TEST(ViewLineGpu, VolumeViewCastsOneMipToR32Uint) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    BackendTexture3DDesc vd{};
    vd.width = vd.height = 4;
    vd.depth = 2;
    vd.mip_levels = 2;
    vd.pixel_format = PixelFormat::RGBA8Unorm;
    vd.shader_resource = true;
    vd.unordered_access = true;
    vd.pixel_format_view = true;
    auto volume = device.create_texture_3d(vd, nullptr, 0, 0);
    ASSERT_TRUE(volume && volume->valid());
    EXPECT_EQ(volume->view(PixelFormat::RGBA8Unorm, 0, 2), volume.get());
    BackendTexture3D* cast = volume->view(PixelFormat::R32Uint, 1, 1);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast, volume->view(PixelFormat::R32Uint, 1, 1)) << "kept";
    EXPECT_EQ(cast->desc().pixel_format, PixelFormat::R32Uint);
    EXPECT_EQ(cast->desc().width, 2u);
    EXPECT_EQ(cast->desc().depth, 1u);
    EXPECT_EQ(volume->view(PixelFormat::RGBA16Float, 0, 1), nullptr)
        << "another texel size";

    // Write mip 1 through the cast, as [vol] "R32_UINT cast mip1 mark".
    const std::string msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(texture3d<uint, access::write> v [[texture(" +
        std::to_string(BACKEND_UAV_TEXTURE_BASE) + ")]],\n"
        "    uint3 id [[thread_position_in_grid]]) {\n"
        "    if (id.x < 2u && id.y < 2u && id.z < 1u)\n"
        "        v.write(uint4(0xA0000000u | id.x | (id.y << 8u)), id);\n}\n";
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl;
    pd.cache_key = 0x3D3D0001u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid());
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = 2;
    call.group_count_z = 1;
    call.textures_3d[BACKEND_UAV_TEXTURE_BASE] = cast;
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    std::array<u32, 4> texels{};
    ASSERT_TRUE(volume->readback_w_slice(1, 0, texels.data(), 8));
    EXPECT_EQ(texels, (std::array<u32, 4>{0xA0000000u, 0xA0000001u,
                                          0xA0000100u, 0xA0000101u}));
}

TEST(ViewLineGpu, VolumeUavWindowCountsZFromItsFirstSlice) {
    // [vol] "R32_UINT cast mip0 W{1,1} mark": cs_vol_mark over a view of W
    // slice 1 alone writes slice 1 from its z = 0 threads; its z = 1
    // threads are past the view and write nothing (WARP spills them into
    // mip 1, which D3D leaves undefined). GetDimensions is the mip's.
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    DxbcTranslationOptions options{};
    options.guard_buffer_bounds = true;
    const auto pipeline = compile(g_cs_vol_mark, sizeof(g_cs_vol_mark),
                                  options, 0x3D3D0002u, nullptr);
    ASSERT_TRUE(pipeline);
    BackendTexture3DDesc vd{};
    vd.width = vd.height = 4;
    vd.depth = 2;
    vd.pixel_format = PixelFormat::R32Uint;
    vd.shader_resource = true;
    vd.unordered_access = true;
    std::vector<u32> initial(32);
    for (u32 i = 0; i < 32; ++i) initial[i] = 0x40000000u + i;
    auto volume = device.create_texture_3d(vd, initial.data(), 16, 64);
    const std::vector<u32> poison(4, 0xDDDDDDDDu);
    auto dims = buffer(16, poison.data());
    ASSERT_TRUE(volume && dims);
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = call.threads_per_group_y =
        call.threads_per_group_z = 2;  // [numthreads(2, 2, 2)]
    call.textures_3d[BACKEND_UAV_TEXTURE_BASE] = volume.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE + 1] = dims.get();
    std::array<u32, kPushBlockBytes / 4> push{};
    push[kGuardSizeTableOffset / 4 + kGuardSizeSrvSlots] =
        (1u << kVolumeUavWindowSizeShift) | 1u;  // WSize 1 from slice 1
    push[kGuardSizeTableOffset / 4 + kGuardSizeSrvSlots + 1] = 4u;  // u1
    call.constants = push.data();
    call.constants_size = sizeof(push);
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    EXPECT_EQ(read_words(*dims, 4),
              (std::vector<u32>{4, 4, 2, 0}));
    std::array<u32, 16> slice{};
    ASSERT_TRUE(volume->readback_w_slice(0, 0, slice.data(), 16));
    for (u32 i = 0; i < 16; ++i) {
        EXPECT_EQ(slice[i], 0x40000000u + i) << "slice 0 is outside the view";
    }
    ASSERT_TRUE(volume->readback_w_slice(0, 1, slice.data(), 16));
    const std::array<u32, 16> warp{
        0xa0000000u, 0xa0000001u, 0x40000012u, 0x40000013u,
        0xa0000100u, 0xa0000101u, 0x40000016u, 0x40000017u,
        0x40000018u, 0x40000019u, 0x4000001Au, 0x4000001Bu,
        0x4000001Cu, 0x4000001Du, 0x4000001Eu, 0x4000001Fu};
    EXPECT_EQ(slice, warp);
}

TEST(ViewLineGpu, LineQuadShadersCompile) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    // vs_pos_col's records: o0 SV_Position, o1 COLOR, o2 uint4 ID -- and a
    // clip distance and viewport index to reach every system-value path.
    std::vector<GsPassthroughOutput> outputs(4);
    outputs[0].register_num = 0;
    outputs[0].is_position = true;
    outputs[1].register_num = 1;
    outputs[2].register_num = 2;
    outputs[2].component_type = 1;
    outputs[3].register_num = 3;
    outputs[3].component_system_values = {2, 5, 0, 0};
    const auto fragment =
        std::string("#include <metal_stdlib>\nusing namespace metal;\n") +
        "struct In { float4 c [[user(locn1)]]; float3 d [[user(locn" +
        std::to_string(kLineDistanceLocation) + "), flat]]; };\n"
        "fragment float4 main0(In i [[stage_in]]) { return i.c * i.d.x; }\n";
    const struct {
        LineQuadSource source;
        const char* what;
    } cases[] = {
        {{0, 2}, "lines, ids"},
        {{0, 4}, "lines with adjacency, ids"},
        {{0, 3}, "wireframe triangles, ids"},
        {{8, 2}, "lines, regions"},
        {{9, 3}, "wireframe triangles, regions"},
    };
    u64 key = 0x11E0A0D0u;
    for (const auto& c : cases) {
        const std::string vertex =
            generate_line_quad_vertex_msl(outputs, c.source, true);
        ASSERT_FALSE(vertex.empty()) << c.what;
        BackendPipelineDesc pd{};
        pd.vertex_shader_msl = vertex;
        pd.vertex_entry_point = kGsPassthroughEntryPoint;
        pd.fragment_shader_msl = fragment;
        pd.fragment_entry_point = "main0";
        pd.color_format = PixelFormat::RGBA8Unorm;
        pd.input_primitive_class = BackendPrimitiveClass::Triangle;
        pd.cache_key = ++key;
        auto pipeline = device.create_pipeline(pd);
        EXPECT_TRUE(pipeline && pipeline->valid()) << c.what << "\n" << vertex;
    }
}
