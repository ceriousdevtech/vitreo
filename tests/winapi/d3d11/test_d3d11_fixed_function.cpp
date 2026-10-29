// SPDX-License-Identifier: LGPL-2.1-only
//
// Fixed-function pipeline behaviour.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11FixedFunctionTest, IntegerTargetBlendingReachesTheBackendAsIs) {
    // The frontend passes BlendEnable through; the Metal backend drops it for
    // a non-blendable (integer) attachment (metal_device.mm, proven by
    // GeneratedMslTest.BlendingOnAnIntegerTargetIsIgnored). Windows stores
    // the source: d3d11_a07_om_probe `O9 R32_UINT blend ONE/ONE ADD src=5
    // dst=7: 00000005`.
    bind_target(42);  // R32_UINT
    bind_attr_inst_streams(1);
    context_call(24, {1});
    write_words(BIG, {0, 0});
    write_words(BIG + 8, {1, 2, 2, 1, 2, 2, 1, 0xF});  // ONE/ONE ADD
    ASSERT_EQ(device_call(20, {BIG, OUT}), d3d11::S_OK);
    context_call(35, {memory.read_u64(OUT).value(), 0, 0xFFFFFFFFu});
    context_call(13, {1, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_EQ(backend.state.last_draw_pipeline_desc.color_formats[0],
              graphics::PixelFormat::R32Uint);
}

TEST_F(D3D11FixedFunctionTest, ScissorEnabledWithNoRectUsesTheEmptyDefault) {
    // D3D11.3 15.7: the default scissor rect is {0,0,0,0}; Windows draws
    // nothing (d3d11_a07_rs_probe `R3 scissor on, no rect set`).
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {1});
    context_call(43, {rasterizer({3, 1, 0, 0, 0, 0, 1, 1, 0, 0})});
    context_call(45, {0, 0});
    context_call(13, {1, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_TRUE(backend.state.last_draw_call.scissor_enabled);
    EXPECT_EQ(backend.state.last_draw_call.scissor,
              (std::array<int, 4>{0, 0, 0, 0}));
    // An inverted rect stays inverted: the backend draws nothing for it.
    write_words(DESC, {10, 0, 5, 16});
    context_call(45, {1, DESC});
    context_call(13, {1, 0});
    EXPECT_TRUE(backend.state.last_draw_call.scissor_enabled);
    EXPECT_EQ(backend.state.last_draw_call.scissor,
              (std::array<int, 4>{10, 0, 5, 16}));
}

TEST_F(D3D11FixedFunctionTest, ViewportsWithInvalidRangesSkipTheCall) {
    // D3D11.3 15.6.1: MinDepth/MaxDepth in [0,1], MinDepth <= MaxDepth, and
    // extents within [-32768, 32767], or "the runtime ... skip[s] the call".
    // Measured: MinDepth 0.8 > MaxDepth 0.2 keeps the previous viewport
    // (d3d11_a07_rs_probe `R2 vp MinDepth 0.8 > MaxDepth 0.2`).
    const auto viewport_width = [&]() {
        ASSERT_TRUE(memory.write_u32(OUT, 1).is_ok());
        context_call(95, {OUT, OUT2});
    };
    write_words(DESC, {0, 0, 0x41800000u, 0x41800000u, 0, 0x3F800000u});
    context_call(44, {1, DESC});  // 16x16, 0..1
    const std::initializer_list<std::initializer_list<u32>> invalid{
        {0, 0, 0x41000000u, 0x41000000u, 0x3F4CCCCDu, 0x3E4CCCCDu},  // .8>.2
        {0, 0, 0x41000000u, 0x41000000u, 0, 0x3FC00000u},  // MaxDepth 1.5
        {0, 0, 0x41000000u, 0x41000000u, 0xBF800000u, 0},  // MinDepth -1
        {0xC7200000u, 0, 0x41000000u, 0x41000000u, 0, 0x3F800000u},  // x -40960
        {0, 0, 0x47000000u, 0x41000000u, 0, 0x3F800000u},  // x + w = 32768
        {0, 0, 0x41000000u, 0x41000000u, 0x7FC00000u, 0x3F800000u},  // NaN
    };
    for (const auto& words : invalid) {
        write_words(DESC, words);
        context_call(44, {1, DESC});
        viewport_width();
        EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
        EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 0x41800000u)
            << "the previous viewport stays bound";
    }
    // A valid viewport after them binds; MinDepth == MaxDepth is valid.
    write_words(DESC, {0xBF800000u, 0, 0x41000000u, 0x41000000u, 0x3F000000u,
                       0x3F000000u});
    context_call(44, {1, DESC});
    viewport_width();
    EXPECT_EQ(memory.read_u32(OUT2 + 8).value(), 0x41000000u);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0xBF800000u);
}

TEST_F(D3D11FixedFunctionTest, StepRateZeroNeverAdvances) {
    // D3D11.3 8.3.1: a per-instance element with step rate 0 fetches element
    // StartInstanceLocation for every instance (d3d11_a07_ia_probe `B step=0
    // DrawInstanced(1,7,0,3)`: b3 for all seven). The backend contract says
    // so for rate 0; it used to arrive as 1.
    bind_target();
    bind_attr_inst_streams(0);
    context_call(24, {1});
    context_call(21, {1, 7, 0, 3});  // DrawInstanced
    ASSERT_EQ(backend.state.draws, 1u);
    const auto& desc = backend.state.last_draw_pipeline_desc;
    EXPECT_TRUE(desc.vertex_buffer_per_instance[1]);
    EXPECT_EQ(desc.vertex_buffer_step_rates[1], 0u);
    EXPECT_EQ(backend.state.last_draw_call.first_instance, 3u);
}

TEST_F(D3D11FixedFunctionTest, AnUnboundDeclaredSlotReadsZeros) {
    // An unbound slot is a buffer of size 0 (D3D11.3 8.19.2): the draw runs
    // and fetches 0 (d3d11_a07_ia_probe `B slot1 unbound`: b-1 = attr 0).
    bind_target();
    bind_attr_inst_streams(1, /*bind_inst=*/false);
    context_call(24, {1});
    context_call(21, {2, 2, 0, 0});
    ASSERT_EQ(backend.state.draws, 1u) << "the draw is not skipped";
    ASSERT_NE(backend.state.last_draw_vertex_stream_widths[1], 0u);
    const auto& zeros = backend.state.last_draw_vertex_stream_bytes[1];
    ASSERT_FALSE(zeros.empty());
    EXPECT_TRUE(std::all_of(zeros.begin(), zeros.end(),
                            [](u8 b) { return b == 0; }));
    EXPECT_TRUE(backend.state.last_draw_pipeline_desc.vertex_buffer_constant[1])
        << "every instance reads the same zero element";
}

TEST_F(D3D11FixedFunctionTest, FetchPastTheBufferEndReadsZeros) {
    // D3D11.3 8.19.2: 0 for a fetch past the end (d3d11_a07_ia_probe `A draw
    // POINTLIST n=6 start=61 (vb has 64)`: 3/- 4/- 5/-). The slot is served
    // from a zero-filled buffer holding the bound bytes at its start.
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {1});
    backend.state.buffer_region_copies.clear();
    context_call(13, {6, 61});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_GE(backend.state.last_draw_vertex_stream_widths[0], 67u * 4u);
    EXPECT_EQ(backend.state.last_draw_call.vertex_buffer_offsets[0], 0u);
    ASSERT_FALSE(backend.state.buffer_region_copies.empty());
    EXPECT_EQ(backend.state.buffer_region_copies.back().bytes, 256u)
        << "the whole bound range, then zeros";
    // In bounds: the guest's own buffer, untouched.
    backend.state.buffer_region_copies.clear();
    context_call(13, {3, 61});
    EXPECT_EQ(backend.state.last_draw_vertex_stream_widths[0], 256u);
    EXPECT_TRUE(backend.state.buffer_region_copies.empty());
    // Indexed: the largest index sizes it (`A idx32 POINTLIST indices past vb
    // end`: 64, 65 and 16960 read 0).
    const GuestAddr ib = buffer(0x2, {62, 63, 64, 65, 16960});
    context_call(19, {ib, 42, 0});
    context_call(12, {5, 0, 0});
    EXPECT_GE(backend.state.last_draw_vertex_stream_widths[0], 16961u * 4u);
}

TEST_F(D3D11FixedFunctionTest, AdjacencyWithoutAGeometryShaderDrawsThePrimitive) {
    // D3D11.3 8.15: adjacent vertices are visible only to a GS; with none the
    // primitive itself is drawn (d3d11_a07_ia_probe `E *_ADJ no GS`: a
    // TRIANGLELIST_ADJ draws (v0, v2, v4), a LINELIST_ADJ the line v1-v2).
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {12});  // TRIANGLELIST_ADJ
    context_call(13, {12, 3});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_EQ(backend.state.last_draw_call.topology,
              graphics::BackendPrimitiveTopology::TriangleList);
    EXPECT_EQ(drawn_indices(), (std::vector<u32>{0, 2, 4, 6, 8, 10}));
    EXPECT_EQ(backend.state.last_draw_call.base_vertex, 3)
        << "StartVertexLocation is the base: fetch and SV_VertexID unchanged";
    EXPECT_EQ(backend.state.last_draw_pipeline_desc.input_primitive_class,
              graphics::BackendPrimitiveClass::Triangle);

    context_call(24, {10});  // LINELIST_ADJ
    context_call(13, {9, 0});
    EXPECT_EQ(backend.state.last_draw_call.topology,
              graphics::BackendPrimitiveTopology::LineList);
    EXPECT_EQ(drawn_indices(), (std::vector<u32>{1, 2, 5, 6}));
    EXPECT_EQ(backend.state.last_draw_pipeline_desc.input_primitive_class,
              graphics::BackendPrimitiveClass::Line);

    context_call(24, {11});  // LINESTRIP_ADJ n=5: lines 1-2, 2-3
    context_call(13, {5, 0});
    EXPECT_EQ(drawn_indices(), (std::vector<u32>{1, 2, 2, 3}));

    // TRIANGLESTRIP_ADJ n=8 (`E TRIANGLESTRIP_ADJ n=8 no GS`: leading
    // vertices 0 and 2): the measured table's inputs 0, 2, 4.
    context_call(24, {13});
    context_call(13, {8, 0});
    EXPECT_EQ(drawn_indices(), (std::vector<u32>{0, 2, 4, 2, 6, 4}));

    // Indexed, through the index values.
    const GuestAddr ib = buffer(0x2, {10, 11, 12, 13, 14, 15});
    context_call(19, {ib, 42, 0});
    context_call(24, {12});
    context_call(12, {6, 0, 5});
    EXPECT_EQ(drawn_indices(), (std::vector<u32>{10, 12, 14}));
    EXPECT_EQ(backend.state.last_draw_call.base_vertex, 5);
}

TEST_F(D3D11FixedFunctionTest, TheCutValueInAListRestartsAssembly) {
    // D3D11.3 8.12/8.13 (d3d11_a07_ia_probe `E idx16 TRIANGLELIST
    // {0,1,2,FFFF,4,5,6}`): the triangles are (0,1,2) and (4,5,6).
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {4});
    // R16 {0,1,2,FFFF,4,5,6}, padded to a word.
    const GuestAddr ib = buffer(0x2, {0x00010000u, 0xFFFF0002u, 0x00050004u,
                                      0x00000006u});
    context_call(19, {ib, 57, 0});
    context_call(12, {7, 0, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_TRUE(backend.state.last_draw_call.index_32bit);
    EXPECT_EQ(drawn_indices(), (std::vector<u32>{0, 1, 2, 4, 5, 6}));
    // `A idx16 LINELIST containing 0xFFFF`: {10,11,FFFF,12,13,14,15,FFFF,16,17}.
    const GuestAddr lines = buffer(0x2, {0x000B000Au, 0x000CFFFFu, 0x000E000Du,
                                         0xFFFF000Fu, 0x00110010u});
    context_call(19, {lines, 57, 0});
    context_call(24, {2});
    context_call(12, {10, 0, 0});
    EXPECT_EQ(drawn_indices(),
              (std::vector<u32>{10, 11, 12, 13, 14, 15, 16, 17}));
    // No cut: the guest's own 16-bit stream.
    const GuestAddr plain = buffer(0x2, {0x00010000u, 0x00030002u});
    context_call(19, {plain, 57, 0});
    context_call(24, {4});
    context_call(12, {3, 0, 0});
    EXPECT_FALSE(backend.state.last_draw_call.index_32bit);
}

TEST_F(D3D11FixedFunctionTest, D24DepthBiasIsConstantInUnormUnits) {
    // D3D11.3 15.10, UNORM: Bias = DepthBias * r, r = 1/(2^24 - 1), whatever
    // z is (Windows: z=.25 bias 100 -> +100 units, `R4 D24S8 bias=100`).
    // D24 lives in a float depth buffer, so the constant moves to the vertex
    // stages (push member 4) and Metal keeps only the slope part.
    bind_target(28, depth_view(45));  // D24_UNORM_S8_UINT
    bind_attr_inst_streams(1);
    context_call(24, {4});
    const auto draw_with = [&](u32 bias, float clamp, float slope) {
        u32 clamp_bits = 0, slope_bits = 0;
        std::memcpy(&clamp_bits, &clamp, 4);
        std::memcpy(&slope_bits, &slope, 4);
        context_call(43, {rasterizer({3, 1, 0, bias, clamp_bits, slope_bits,
                                      1, 0, 0, 0})});
        context_call(13, {3, 0});
    };
    constexpr float r = 1.0f / 16777215.0f;
    const auto offset = [&]() {
        return push_word<float>(backend.state.last_draw_vertex_constants,
                                graphics::kFixedFunctionOffset);
    };
    draw_with(100, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(offset(), 100.0f * r);
    EXPECT_EQ(backend.state.last_draw_call.depth_bias, 0.0f);
    // Clamp with no slope: min(DepthBiasClamp, DepthBias * r).
    draw_with(1000, 0.0001f, 0.0f);
    EXPECT_FLOAT_EQ(offset(), 1000.0f * r);
    draw_with(static_cast<u32>(-1000), -0.00001f, 0.0f);
    EXPECT_FLOAT_EQ(offset(), -0.00001f);
    // Slope and clamp: the constant here, Metal's clamp re-based.
    draw_with(100, 0.01f, 2.5f);
    EXPECT_FLOAT_EQ(offset(), 100.0f * r);
    EXPECT_FLOAT_EQ(backend.state.last_draw_call.depth_bias_slope_scale, 2.5f);
    EXPECT_FLOAT_EQ(backend.state.last_draw_call.depth_bias_clamp,
                    0.01f - 100.0f * r);
    // Lines take no bias (15.10).
    context_call(24, {2});
    draw_with(100, 0.0f, 0.0f);
    EXPECT_EQ(offset(), 0.0f);
    // D16_UNORM: r = 1/65535 (Windows `R4 D16 bias=-100`: 0x8000 ->
    // 0x7f9c, exactly 100 units).
    bind_target(28, depth_view(55));
    context_call(24, {4});
    draw_with(static_cast<u32>(-100), 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(offset(), -100.0f / 65535.0f);
    EXPECT_EQ(backend.state.last_draw_call.depth_bias, 0.0f);
    // D32_FLOAT: Metal's own float rule is D3D's, nothing moves.
    bind_target(28, depth_view(40));
    context_call(24, {4});
    draw_with(100, 0.0f, 0.0f);
    EXPECT_EQ(offset(), 0.0f);
    EXPECT_EQ(backend.state.last_draw_call.depth_bias, 100.0f);
    EXPECT_EQ(backend.state.last_draw_pipeline_desc.depth_format,
              graphics::PixelFormat::D32Float);
}

TEST_F(D3D11FixedFunctionTest, SampleMaskSelectsACoverageWritingPixelShader) {
    // D3D11.3 17.17: "SampleMask is always applied", bit 0 gating a 1x write
    // (d3d11_a07_om_probe `O7 1x SampleMask=0xfffffffe: 00 00 00 00`).
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {1});
    context_call(35, {0, 0, 0xFFFFFFFFu});
    context_call(13, {1, 0});
    EXPECT_EQ(backend.state.last_draw_pipeline_desc.fragment_shader_msl.find(
                  "[[sample_mask]]"),
              std::string::npos)
        << "an all-samples mask needs no variant";
    context_call(35, {0, 0, 0xFFFFFFFEu});
    context_call(13, {1, 0});
    EXPECT_NE(backend.state.last_draw_pipeline_desc.fragment_shader_msl.find(
                  "[[sample_mask]]"),
              std::string::npos);
    EXPECT_EQ(push_word<u32>(backend.state.last_draw_fragment_constants,
                             graphics::kFixedFunctionOffset + 4),
              0xFFFFFFFEu);
    // With no pixel shader the mask still limits depth writes: a mask-only
    // fragment function carries it.
    context_call(9, {0, 0, 0});
    context_call(13, {1, 0});
    EXPECT_NE(backend.state.last_draw_pipeline_desc.fragment_shader_msl.find(
                  "[[sample_mask]]"),
              std::string::npos);
}

TEST_F(D3D11FixedFunctionTest, AlphaToCoverageReachesThePipeline) {
    // D3D11.3 17.18 (d3d11_a07_om_probe `O8`: coverage follows o0.a).
    bind_target();
    bind_attr_inst_streams(1);
    use_shaders(g_vs_pt, sizeof(g_vs_pt), g_ps_alpha, sizeof(g_ps_alpha));
    context_call(24, {1});
    write_words(BIG, {1, 0});
    write_words(BIG + 8, {0, 2, 1, 1, 2, 1, 1, 0xF});
    ASSERT_EQ(device_call(20, {BIG, OUT}), d3d11::S_OK);
    context_call(35, {memory.read_u64(OUT).value(), 0, 0xFFFFFFFFu});
    context_call(13, {1, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_TRUE(backend.state.last_draw_pipeline_desc.alpha_to_coverage);
    write_words(BIG, {0, 0});
    ASSERT_EQ(device_call(20, {BIG, OUT}), d3d11::S_OK);
    context_call(35, {memory.read_u64(OUT).value(), 0, 0xFFFFFFFFu});
    context_call(13, {1, 0});
    EXPECT_FALSE(backend.state.last_draw_pipeline_desc.alpha_to_coverage);
}

TEST_F(D3D11FixedFunctionTest, OutputDepthIsClampedToTheViewportRange) {
    // D3D11.3 15.6: the viewport clamp applies to SV_Depth too (Windows,
    // clip on, viewport 0.2..0.8: oDepth 1.5 -> 0.8, -0.5 -> 0.2,
    // d3d11_a07_rs_probe `R5 D32 clip=1 vp z0.2..0.8`).
    bind_target(28, depth_view(40));
    bind_attr_inst_streams(1);
    use_shaders(g_vs_pt, sizeof(g_vs_pt), g_ps_depth, sizeof(g_ps_depth));
    context_call(24, {1});
    write_words(DESC, {0, 0, 0x41800000u, 0x41800000u, 0x3E4CCCCDu,
                       0x3F4CCCCDu});
    context_call(44, {1, DESC});
    context_call(13, {1, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    const auto& push = backend.state.last_draw_fragment_constants;
    EXPECT_FLOAT_EQ(push_word<float>(push, graphics::kFixedFunctionOffset + 8),
                    0.2f);
    EXPECT_FLOAT_EQ(push_word<float>(push, graphics::kFixedFunctionOffset + 12),
                    0.8f);
    const auto& msl = backend.state.last_draw_pipeline_desc.fragment_shader_msl;
    EXPECT_NE(msl.find("[[depth(any)]]"), std::string::npos);
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases._m9"), std::string::npos)
        << "reads MinDepth";
    EXPECT_NE(msl.find("vitreo_sampler_lod_biases._m10"), std::string::npos)
        << "reads MaxDepth";
}

TEST_F(D3D11FixedFunctionTest, BorderColoursReachEverySlotAndOpaqueBlackTwins) {
    // D3D11.3 7.18.9.1 takes any border colour on any s#. Winoracle:
    // s9 border (0.25,0.5,0.75,0.125) samples as that colour, and A8_UNORM
    // with (0,0,0,1) samples alpha 1 (d3d11_a07_sampler_probe `M5`).
    // Metal's opaque black gives A8 alpha 0, so it goes through the twin too.
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {1});
    const auto sampler = [&](std::array<u32, 4> border) {
        write_words(DESC, {0x15, 4, 4, 4, 0, 1, 0, border[0], border[1],
                           border[2], border[3], 0, 0x7F7FFFFFu});
        EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    };
    const GuestAddr black = sampler({0, 0, 0, 0x3F800000u});
    const GuestAddr colour =
        sampler({0x3E800000u, 0x3F000000u, 0x3F400000u, 0x3E000000u});
    ASSERT_TRUE(memory.write_u64(PTRS, black).is_ok());
    context_call(10, {0, 1, PTRS});  // PSSetSamplers s0
    ASSERT_TRUE(memory.write_u64(PTRS, colour).is_ok());
    context_call(10, {9, 1, PTRS});  // s9
    context_call(13, {1, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_EQ(backend.state.last_draw_sampler_border_color, 0u)
        << "s0 binds transparent black; its twin carries the colour";
    const auto& push = backend.state.last_draw_fragment_constants;
    EXPECT_FLOAT_EQ(push_word<float>(push, 64 + 12), 1.0f) << "s0 alpha";
    const usize s9 = graphics::kBorderHighOffset + 16;
    EXPECT_FLOAT_EQ(push_word<float>(push, s9 + 0), 0.25f);
    EXPECT_FLOAT_EQ(push_word<float>(push, s9 + 4), 0.5f);
    EXPECT_FLOAT_EQ(push_word<float>(push, s9 + 8), 0.75f);
    EXPECT_FLOAT_EQ(push_word<float>(push, s9 + 12), 0.125f);
}

TEST_F(D3D11FixedFunctionTest, MinMaxFilteringIsReportedAndEveryFilterServed) {
    // D3D11_FEATURE_DATA_D3D11_OPTIONS1::MinMaxFiltering (WARP: TRUE,
    // d3d11_minmax_probe [caps]) with all 18 MINIMUM_/MAXIMUM_ filters
    // accepted, and the bits the D3D11_FILTER enum does not define refused.
    write_words(OUT2, {0xCD, 0xCD, 0xCD, 0xCD});
    EXPECT_EQ(device_call(33, {10, OUT2, 16}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT2 + 4).value(), 1u);
    const auto create = [&](u32 filter) {
        write_words(DESC, {filter, 3, 3, 3, 0, 16, 0, 0, 0, 0, 0, 0,
                           0x7F7FFFFFu});
        return device_call(23, {DESC, OUT});
    };
    constexpr std::array<u32, 18> minmax{
        0x100, 0x101, 0x104, 0x105, 0x110, 0x111, 0x114, 0x115, 0x155,
        0x180, 0x181, 0x184, 0x185, 0x190, 0x191, 0x194, 0x195, 0x1D5};
    for (const u32 filter : minmax) {
        EXPECT_EQ(create(filter), d3d11::S_OK) << std::hex << filter;
    }
    for (const u32 filter : {0x102u, 0x108u, 0x120u, 0x140u, 0x156u, 0x1D4u,
                             0x1D7u}) {
        EXPECT_EQ(create(filter), d3d11::E_INVALIDARG) << std::hex << filter;
    }
}

TEST_F(D3D11FixedFunctionTest, PointMipSamplersRoundTiesUpAsD3DDoes) {
    // D3D11.3 7.18.10: a point mip filter takes floor(LOD + 0.5); Windows
    // SampleBias(+1.5) at LOD 2 reads mip 4 (d3d11_a07_sampler_probe `M8`).
    // Apple rounds up only from .515625, so the LOD is shifted by 1/64.
    bind_target();
    bind_attr_inst_streams(1);
    context_call(24, {1});
    const auto draw_with = [&](u32 filter) {
        write_words(DESC, {filter, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0,
                           0x7F7FFFFFu});
        ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
        ASSERT_TRUE(memory.write_u64(PTRS, memory.read_u64(OUT).value())
                        .is_ok());
        context_call(10, {0, 1, PTRS});
        context_call(13, {1, 0});
    };
    draw_with(0x0);  // MIN_MAG_MIP_POINT
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_bias, 1.0f / 64);
    EXPECT_FLOAT_EQ(backend.state.last_draw_shader_lod_bias, 1.0f / 64);
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_min_clamp, 1.0f / 64);
    // Min and mag differ: shifting the LOD would move the min/mag choice.
    draw_with(0x4);  // MIN_POINT_MAG_LINEAR_MIP_POINT
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_bias, 0.0f);
    // A linear mip filter has no ties.
    draw_with(0x15);
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_bias, 0.0f);
}

TEST_F(D3D11FixedFunctionTest, R10G10B10A2UintIsAVertexFormat) {
    // IA-required at FL11 (Windows: ia_support=1, all-ones fetch as
    // 1023 1023 1023 3, d3d11_a07_ia_probe `C fmt 25`). Fetched normalized,
    // restored to integers by the vertex shader's prologue.
    ASSERT_EQ(input_layout({{"ATTR", 25, 0, 0, 0, 0}}, g_vs_fmt_u,
                           sizeof(g_vs_fmt_u)),
              d3d11::S_OK);
    context_call(17, {memory.read_u64(OUT).value()});
    bind_target();
    vertex_buffers({{buffer(0x1, {0xFFFFFFFFu}), 4, 0}});
    use_shaders(g_vs_fmt_u, sizeof(g_vs_fmt_u), g_ps_fmt_u,
                sizeof(g_ps_fmt_u));
    context_call(24, {1});
    context_call(13, {1, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    const auto& desc = backend.state.last_draw_pipeline_desc;
    ASSERT_EQ(desc.vertex_attributes.size(), 1u);
    EXPECT_EQ(desc.vertex_attributes[0].format,
              graphics::BackendVertexFormat::UInt1010102);
    EXPECT_NE(desc.vertex_shader_msl.find("1023.0"), std::string::npos)
        << desc.vertex_shader_msl;
}

TEST_F(D3D11FixedFunctionTest, GeometryCaptureCoversEveryDrawnVertex) {
    // LINELIST_ADJ n=9 through a lineadj GS: two primitives use vertices
    // 0..7, but the capture runs all nine, so its buffer must hold nine
    // records -- sized by the primitives, vertex 8 overwrote the next
    // allocation and the GS read garbage (d3d11_a07_ia_probe `A draw
    // LINELIST_ADJ n=9`: Windows [0 1 2 3] [4 5 6 7]).
    bind_target();
    bind_attr_inst_streams(1);
    use_shaders(g_vs_vid, sizeof(g_vs_vid), g_ps_ab, sizeof(g_ps_ab));
    context_call(23, {shader(13, g_gs_lineadj, sizeof(g_gs_lineadj)), 0, 0});
    context_call(24, {10});  // LINELIST_ADJ
    context_call(13, {9, 0});
    ASSERT_GE(backend.state.capture_draws, 1u);
    // vs_vid writes SV_Position and V: two 16-byte registers a record.
    EXPECT_GE(backend.state.last_capture_buffer_bytes, 9u * 32u);
}

// Track R round 3, R3c (graphics/contracts/delta_record.hpp): a draw names
// the sampler and depth-stencil states built from exactly the descs it
// carries -- after read-only DSV masking and after a capture draw turns depth
// off -- so the backend may bind them without looking the descs up.
TEST_F(D3D11FixedFunctionTest, NativeStatesAreTheOnesBuiltFromTheDrawnFields) {
    graphics::delta_record::force_for_test(graphics::delta_record::Part::NativeState,
                                 graphics::delta_record::Force::On);
    const GuestAddr depth = [&] {
        write_words(DESC, {16, 16, 1, 1, 45, 1, 0, 0, 0x40, 0, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }();
    const auto dsv = [&](u32 flags) {
        write_words(DESC, {45, 3, flags, 0});  // D24S8, TEXTURE2D
        EXPECT_EQ(device_call(10, {depth, DESC, OUT2}), d3d11::S_OK);
        return memory.read_u64(OUT2).value();
    };
    const GuestAddr writable = dsv(0);
    const GuestAddr read_only = dsv(d3d11::DSV_READ_ONLY_DEPTH);
    ASSERT_NE(writable, 0u);
    ASSERT_NE(read_only, 0u);
    // DepthEnable, ALL, LESS_EQUAL; StencilEnable, masks, KEEP/ALWAYS faces.
    write_words(DESC, {1, 1, 4, 1, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8});
    ASSERT_EQ(device_call(21, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr dss = memory.read_u64(OUT).value();
    write_words(DESC, {0x15, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0x7F7FFFFFu});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    ASSERT_TRUE(memory.write_u64(PTRS, memory.read_u64(OUT).value()).is_ok());

    bind_attr_inst_streams(1);
    context_call(24, {4});
    context_call(10, {0, 1, PTRS});  // PSSetSamplers s0
    context_call(36, {dss, 0});      // OMSetDepthStencilState
    bind_target(28, writable);
    context_call(13, {3, 0});
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_EQ(backend.state.draws_naming_depth_stencil_state, 1u);
    EXPECT_EQ(backend.state.draws_naming_sampler_state, 1u);
    EXPECT_TRUE(backend.state.last_draw_call.depth_write_enabled);
    const auto* written = backend.state.last_depth_stencil_state;
    // The same object through a read-only view draws with writes masked:
    // a state built from those fields, not the first one.
    bind_target(28, read_only);
    context_call(13, {3, 0});
    EXPECT_FALSE(backend.state.last_draw_call.depth_write_enabled);
    ASSERT_NE(backend.state.last_depth_stencil_state, nullptr);
    EXPECT_NE(backend.state.last_depth_stencil_state, written);
    // And back: the first state again, nothing new built.
    const unsigned built = backend.state.depth_stencil_states_created;
    bind_target(28, writable);
    context_call(13, {3, 0});
    EXPECT_EQ(backend.state.last_depth_stencil_state, written);
    EXPECT_EQ(backend.state.depth_stencil_states_created, built);
    // The NULL state (Learn: DepthEnable TRUE, LESS) names its own.
    context_call(36, {0, 0});
    context_call(13, {3, 0});
    ASSERT_NE(backend.state.last_depth_stencil_state, nullptr);
    EXPECT_EQ(backend.state.last_draw_call.depth_compare,
              graphics::BackendCompare::Less);
    // A GS draw's capture pass turns depth off after the state was
    // resolved; the state it names must follow.
    context_call(36, {dss, 0});
    use_shaders(g_vs_vid, sizeof(g_vs_vid), g_ps_ab, sizeof(g_ps_ab));
    context_call(23, {shader(13, g_gs_point, sizeof(g_gs_point)), 0, 0});
    context_call(24, {1});
    context_call(13, {1, 0});
    EXPECT_GE(backend.state.capture_draws, 1u);
    EXPECT_EQ(backend.state.native_state_mismatches, 0u);
    graphics::delta_record::force_for_test(graphics::delta_record::Part::NativeState,
                                 graphics::delta_record::Force::None);
}

TEST_F(D3D11FixedFunctionTest, CapturedInstancesFetchByTheirStepRate) {
    // A GS draw captures all its instances in one draw. Instance k's
    // per-instance element is StartInstanceLocation + k / step (D3D11.3
    // 8.3.1): the capture is an ordinary instanced draw at the draw's own
    // StartInstanceLocation with the element's step rate in its pipeline, so
    // the backend steps the data exactly as for a draw without a GS -- the
    // binding is not shifted on the CPU.
    bind_target();
    bind_attr_inst_streams(2);
    use_shaders(g_vs_vid, sizeof(g_vs_vid), g_ps_ab, sizeof(g_ps_ab));
    context_call(23, {shader(13, g_gs_point, sizeof(g_gs_point)), 0, 0});
    context_call(24, {1});
    context_call(21, {1, 3, 0, 5});  // DrawInstanced(1, 3, 0, 5)
    ASSERT_EQ(backend.state.capture_draws, 1u);
    EXPECT_EQ(backend.state.last_capture_call.instance_count, 3u);
    EXPECT_EQ(backend.state.last_capture_first_instance, 5u);
    EXPECT_EQ(backend.state.last_capture_vertex_offsets[1], 0u);
    EXPECT_TRUE(
        backend.state.last_capture_pipeline_desc.vertex_buffer_per_instance[1]);
    EXPECT_EQ(backend.state.last_capture_pipeline_desc.vertex_buffer_step_rates[1],
              2u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
