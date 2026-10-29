// SPDX-License-Identifier: LGPL-2.1-only
//
// Draw memoisation, view reads and line/wireframe rasterisation.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// Track R round 4 (winapi/graphics/d3d11/d3d11_draw_memo.hpp). A binding
// array re-stamps on every change and only on a change, and a copy carries
// its stamp: equal stamps, equal contents.
TEST(D3D11DrawMemo, StampedSlotsStampEveryChangeAndOnlyAChange) {
    d3d11::SamplerSlots a;
    EXPECT_EQ(a.stamp(), 0u) << "the all-zero array";
    a.set(3, 0x1000);
    const u64 first = a.stamp();
    EXPECT_NE(first, 0u);
    a.set(3, 0x1000);
    EXPECT_EQ(a.stamp(), first) << "an unchanged slot keeps the stamp";
    a.set(3, 0x2000);
    EXPECT_NE(a.stamp(), first);
    const d3d11::SamplerSlots copy = a;
    EXPECT_EQ(copy.stamp(), a.stamp());
    EXPECT_EQ(copy[3], 0x2000u);
    d3d11::SamplerSlots b;
    b.set(3, 0x2000);
    EXPECT_NE(b.stamp(), a.stamp()) << "stamps are never reused";
    const u64 before = b.stamp();
    b.fill(0);
    EXPECT_NE(b.stamp(), before);
    std::array<GuestAddr, d3d11::SAMPLER_SLOTS> values{};
    values[1] = 0x3000;
    b.assign(values);
    EXPECT_EQ(b[1], 0x3000u);
    EXPECT_NE(b.stamp(), before);
}

// The memos follow every rebind: a draw after PSSetSamplers / an input
// layout meeting another vertex shader sees the new binding, never the
// memoised one. RED arm: with the memo keys ignored the second draw keeps
// the first draw's sampler and component types.
class D3D11DrawMemoTest : public D3D11FixedFunctionTest {
protected:
    struct Seen {
        std::vector<graphics::BackendFilter> filters;
        std::vector<graphics::BackendVertexComponentType> components;
        std::vector<const graphics::BackendTexture2D*> textures;
        std::vector<const graphics::BackendTexture2D*> targets;
        std::vector<float> shader_lod_bias;
        std::vector<const graphics::BackendBuffer*> vertex_buffers;
    };
    Seen run_sequence() {
        bind_target();
        const auto sampler = [&](u32 filter) {
            write_words(DESC, {filter, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0,
                               0x7F7FFFFFu});
            EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
            return memory.read_u64(OUT).value();
        };
        const GuestAddr point = sampler(0x0);    // MIN_MAG_MIP_POINT
        const GuestAddr linear = sampler(0x15);  // MIN_MAG_MIP_LINEAR
        EXPECT_EQ(input_layout({{"ATTR", 42, 0, 0, 0, 0}}, g_vs_fmt_u,
                               sizeof(g_vs_fmt_u)),
                  d3d11::S_OK);
        context_call(17, {memory.read_u64(OUT).value()});
        const std::array<GuestAddr, 2> vbs{buffer(0x1, {1u, 2u, 3u}),
                                           buffer(0x1, {4u, 5u, 6u})};
        context_call(24, {1});
        // Each shader made once, so a repeated draw meets the same objects.
        const GuestAddr vs_u = shader(12, g_vs_fmt_u, sizeof(g_vs_fmt_u));
        const GuestAddr vs_i = shader(12, g_vs_fmt_i, sizeof(g_vs_fmt_i));
        const GuestAddr vs_f = shader(12, g_vs_fmt_f, sizeof(g_vs_fmt_f));
        context_call(9, {shader(15, g_ps_fmt_u, sizeof(g_ps_fmt_u)), 0, 0});
        // Two views of two textures for t0.
        const auto view_of = [&](GuestAddr tex) {
            EXPECT_EQ(device_call(7, {tex, 0, OUT}), d3d11::S_OK);
            return memory.read_u64(OUT).value();
        };
        const std::array<GuestAddr, 2> views{view_of(texture()),
                                             view_of(texture())};
        // Two render targets, alternating like t0.
        const auto target_view = [&] {
            write_words(DESC, {16, 16, 1, 1, 28, 1, 0, 0, 0x20, 0, 0});
            EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
            EXPECT_EQ(device_call(9, {memory.read_u64(OUT).value(), 0, OUT2}),
                      d3d11::S_OK);
            return memory.read_u64(OUT2).value();
        };
        const std::array<GuestAddr, 2> rtvs{target_view(), target_view()};
        Seen seen;
        u32 n = 0;
        const auto draw = [&](GuestAddr s, GuestAddr vs) {
            ASSERT_TRUE(memory.write_u64(PTRS, s).is_ok());
            context_call(10, {0, 1, PTRS});  // PSSetSamplers s0
            context_call(11, {vs, 0, 0});    // VSSetShader
            // t0 alternates every other draw (draws 0,1 share one).
            ASSERT_TRUE(memory.write_u64(PTRS, rtvs[(n / 2) & 1]).is_ok());
            context_call(33, {1, PTRS, 0});  // OMSetRenderTargets
            vertex_buffers({{vbs[(n / 2) & 1], 4, 0}});
            ASSERT_TRUE(memory.write_u64(PTRS, views[(n++ / 2) & 1]).is_ok());
            context_call(8, {0, 1, PTRS});   // PSSetShaderResources t0
            context_call(13, {1, 0});
            seen.textures.push_back(backend.state.last_draw_call.textures[0]);
            seen.targets.push_back(backend.state.last_pass_color0);
            seen.vertex_buffers.push_back(
                backend.state.last_draw_call.vertex_buffers[0]);
            seen.shader_lod_bias.push_back(
                backend.state.last_draw_shader_lod_bias);
            seen.filters.push_back(backend.state.last_draw_min_filter);
            const auto& attrs =
                backend.state.last_draw_pipeline_desc.vertex_attributes;
            seen.components.push_back(
                attrs.empty() ? graphics::BackendVertexComponentType::Float
                              : attrs[0].shader_component_type);
        };
        draw(point, vs_u);
        draw(point, vs_u);   // every memo hits
        draw(linear, vs_i);
        draw(point, vs_f);
        draw(0, vs_u);       // the default sampler
        return seen;
    }
    void TearDown() override {
        d3d11::draw_memo::mutation_for_test().store(0);
        d3d11::draw_memo::force_all_for_test(d3d11::draw_memo::Force::None);
        D3D11FixedFunctionTest::TearDown();
    }
};

TEST_F(D3D11DrawMemoTest, MemoisedDrawsMatchTheColdPathAndHit) {
    d3d11::draw_memo::force_all_for_test(d3d11::draw_memo::Force::Off);
    const Seen cold = run_sequence();
    d3d11::draw_memo::force_all_for_test(d3d11::draw_memo::Force::On);
    const auto before = d3d11.draw_memo_counters();
    const Seen memo = run_sequence();
    const auto after = d3d11.draw_memo_counters();
    ASSERT_EQ(cold.filters.size(), 5u);
    EXPECT_EQ(memo.filters, cold.filters);
    EXPECT_EQ(memo.components, cold.components);
    ASSERT_EQ(memo.textures.size(), cold.textures.size());
    for (usize i = 0; i < cold.textures.size(); ++i) {
        // Each run makes its own textures: compare the pattern.
        EXPECT_EQ(memo.textures[i] == memo.textures[0],
                  cold.textures[i] == cold.textures[0]) << i;
        EXPECT_NE(cold.textures[i], nullptr);
    }
    EXPECT_NE(cold.textures[2], cold.textures[0]);
    EXPECT_EQ(cold.filters[0], graphics::BackendFilter::Nearest);
    EXPECT_EQ(cold.filters[2], graphics::BackendFilter::Linear);
    EXPECT_EQ(cold.components[0], graphics::BackendVertexComponentType::UInt);
    EXPECT_EQ(cold.components[2], graphics::BackendVertexComponentType::SInt);
    EXPECT_EQ(cold.components[3], graphics::BackendVertexComponentType::Float);
    if (!d3d11::draw_memo::verify()) {  // verify mode recomputes the IA
        EXPECT_GT(after.sampler_hits, before.sampler_hits) << "the repeat hit";
        EXPECT_GT(after.stage_hits, before.stage_hits);
        EXPECT_GT(after.ia_hits, before.ia_hits);
        EXPECT_GT(after.srv_hits, before.srv_hits);
        EXPECT_GT(after.om_hits, before.om_hits);
        EXPECT_GT(after.key_hits, before.key_hits);
        EXPECT_GT(after.vb_hits, before.vb_hits);
    }
    ASSERT_EQ(memo.vertex_buffers.size(), cold.vertex_buffers.size());
    for (usize i = 0; i < cold.vertex_buffers.size(); ++i) {
        EXPECT_EQ(memo.vertex_buffers[i] == memo.vertex_buffers[0],
                  cold.vertex_buffers[i] == cold.vertex_buffers[0]) << i;
        EXPECT_NE(cold.vertex_buffers[i], nullptr);
    }
    EXPECT_EQ(memo.shader_lod_bias, cold.shader_lod_bias);
    ASSERT_EQ(memo.targets.size(), cold.targets.size());
    for (usize i = 0; i < cold.targets.size(); ++i) {
        EXPECT_EQ(memo.targets[i] == memo.targets[0],
                  cold.targets[i] == cold.targets[0]) << i;
    }
}

// The same for a deferred context: FinishCommandList(TRUE) carries the
// context's bindings into the next list as its initial state, holding every
// object in it. After the application unbinds and releases a sampler, a
// draw recorded under that initial state still draws with it -- the object
// stays alive, so its address cannot be recycled under a memo -- with the
// memos on (and recomputed under VITREO_D3D11_DRAW_MEMO_VERIFY=1) or off.
TEST_F(D3D11DrawMemoTest, DeferredInitialStateHoldsItsSamplers) {
    for (const auto force :
         {d3d11::draw_memo::Force::On, d3d11::draw_memo::Force::Off}) {
        SCOPED_TRACE(force == d3d11::draw_memo::Force::On ? "memo on"
                                                          : "memo off");
        d3d11::draw_memo::force_all_for_test(force);
        ASSERT_EQ(device_call(27, {0, OUT}), d3d11::S_OK);  // deferred
        const GuestAddr deferred = memory.read_u64(OUT).value();
        const GuestAddr immediate = context;
        const auto sampler = [&](u32 filter) {
            write_words(DESC, {filter, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0,
                               0x7F7FFFFFu});
            EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
            return memory.read_u64(OUT).value();
        };
        // A sampler of its own per arm: the state pool hands an identical
        // desc the same object.
        const GuestAddr point =
            sampler(force == d3d11::draw_memo::Force::On ? 0x0u : 0x1u);
        // Everything below is recorded on the deferred context.
        context = deferred;
        bind_target();
        ASSERT_EQ(input_layout({{"ATTR", 42, 0, 0, 0, 0}}, g_vs_fmt_u,
                               sizeof(g_vs_fmt_u)),
                  d3d11::S_OK);
        context_call(17, {memory.read_u64(OUT).value()});
        vertex_buffers({{buffer(0x1, {1u, 2u, 3u}), 4, 0}});
        context_call(24, {1});
        use_shaders(g_vs_fmt_u, sizeof(g_vs_fmt_u), g_ps_fmt_u,
                    sizeof(g_ps_fmt_u));
        ASSERT_TRUE(memory.write_u64(PTRS, point).is_ok());
        context_call(10, {0, 1, PTRS});
        context_call(114, {1, OUT2});  // FinishCommandList(TRUE)
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
        const GuestAddr first = memory.read_u64(OUT2).value();
        context_call(13, {1, 0});  // a draw under the carried state
        ASSERT_TRUE(memory.write_u64(PTRS, 0).is_ok());
        context_call(10, {0, 1, PTRS});
        // The application lets go: only the next list's initial state
        // holds it now.
        cpu.gpr.rsp = STACK_BASE + 0x800;
        set_arg(0, point);
        d3d11.impl_SamplerStateMethod(marshaler, 2);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_TRUE(kept_alive_by_runtime(
            point, &D3D11Manager::impl_SamplerStateMethod));
        context_call(114, {0, OUT2});
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
        const GuestAddr second = memory.read_u64(OUT2).value();
        context = immediate;
        // A new sampler of another filter cannot take the held address.
        const GuestAddr linear = sampler(0x15);
        EXPECT_NE(linear, point);
        const u32 draws = backend.state.draws;
        context_call(58, {second, 0});  // ExecuteCommandList(FALSE)
        ASSERT_EQ(backend.state.draws, draws + 1);
        EXPECT_EQ(backend.state.last_draw_min_filter,
                  graphics::BackendFilter::Nearest);
        (void)first;
    }
}

// R4 bench (not a test; run with --gtest_also_run_disabled_tests): a
// UE-like draw stream through the frontend into the test backend. Per draw a
// constant buffer and a vertex buffer offset change, t0 changes every other
// draw, the pixel shader every 16th and the render target every 64th.
// Prints ns/draw for the memo arm the environment selects; profile it with
// `sample`.
TEST_F(D3D11DrawMemoTest, DISABLED_DrawLoopBench) {
    bind_target();
    const auto sampler = [&](u32 filter) {
        write_words(DESC, {filter, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0,
                           0x7F7FFFFFu});
        EXPECT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    };
    const GuestAddr linear = sampler(0x15);
    ASSERT_EQ(input_layout({{"ATTR", 42, 0, 0, 0, 0}}, g_vs_fmt_u,
                           sizeof(g_vs_fmt_u)),
              d3d11::S_OK);
    context_call(17, {memory.read_u64(OUT).value()});
    const GuestAddr vb = buffer(0x1, std::vector<u32>(256, 7u));
    context_call(24, {1});
    const GuestAddr vs = shader(12, g_vs_fmt_u, sizeof(g_vs_fmt_u));
    const std::array<GuestAddr, 2> ps{
        shader(15, g_ps_fmt_u, sizeof(g_ps_fmt_u)),
        shader(15, g_ps_fmt_u, sizeof(g_ps_fmt_u))};
    context_call(11, {vs, 0, 0});
    const auto view_of = [&](GuestAddr tex) {
        EXPECT_EQ(device_call(7, {tex, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    };
    std::array<GuestAddr, 4> views{};
    for (auto& v : views) v = view_of(texture());
    std::array<GuestAddr, 8> cbs{};
    for (auto& cb : cbs) cb = buffer(0x4, std::vector<u32>(64, 1u));
    const auto target_view = [&] {
        write_words(DESC, {16, 16, 1, 1, 28, 1, 0, 0, 0x20, 0, 0});
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        EXPECT_EQ(device_call(9, {memory.read_u64(OUT).value(), 0, OUT2}),
                  d3d11::S_OK);
        return memory.read_u64(OUT2).value();
    };
    const std::array<GuestAddr, 2> rtvs{target_view(), target_view()};
    ASSERT_TRUE(memory.write_u64(PTRS + 0x20, linear).is_ok());
    context_call(10, {0, 1, PTRS + 0x20});
    const char* n_text = std::getenv("VITREO_R4_BENCH_DRAWS");
    const u32 draws = n_text != nullptr ? static_cast<u32>(std::atoi(n_text))
                                        : 200000u;
    const auto t0 = std::chrono::steady_clock::now();
    for (u32 n = 0; n < draws; ++n) {
        if ((n & 63) == 0) {
            ASSERT_TRUE(memory.write_u64(PTRS, rtvs[(n >> 6) & 1]).is_ok());
            context_call(33, {1, PTRS, 0});
        }
        if ((n & 15) == 0) context_call(9, {ps[(n >> 4) & 1], 0, 0});
        vertex_buffers({{vb, 4, (n & 7) * 4}});
        ASSERT_TRUE(memory.write_u64(PTRS + 0x40, cbs[n & 7]).is_ok());
        context_call(16, {0, 1, PTRS + 0x40});  // PSSetConstantBuffers b0
        if ((n & 1) == 0) {
            ASSERT_TRUE(
                memory.write_u64(PTRS + 0x60, views[(n >> 1) & 3]).is_ok());
            context_call(8, {0, 1, PTRS + 0x60});
        }
        context_call(13, {3, 0});
    }
    const double ns =
        std::chrono::duration<double, std::nano>(
            std::chrono::steady_clock::now() - t0).count() / draws;
    std::fprintf(stderr, "[r4-bench] draws=%u ns/draw=%.1f (incl. setters)\n",
                 draws, ns);
}

// RED, one part at a time: that part alone on, its keys ignored. Each must
// then miss a change the cold path sees, in the field it derives.
TEST_F(D3D11DrawMemoTest, IgnoringEachMemosKeysIsCaught) {
    if (d3d11::draw_memo::verify()) {
        GTEST_SKIP() << "verify mode aborts on the deliberate defect itself";
    }
    using d3d11::draw_memo::Part;
    d3d11::draw_memo::force_all_for_test(d3d11::draw_memo::Force::Off);
    const Seen cold = run_sequence();
    // The pattern of a run: which draws met the same object as draw 0.
    const auto pattern = [](const auto& seq) {
        std::vector<bool> same;
        for (const auto& v : seq) same.push_back(v == seq[0]);
        return same;
    };
    for (const Part part : {Part::StageMemo, Part::SamplerMemo, Part::IaMemo,
                            Part::SrvMemo, Part::OmMemo, Part::KeyMemo,
                            Part::VbMemo}) {
        SCOPED_TRACE(d3d11::draw_memo::name(part));
        d3d11::draw_memo::force_all_for_test(d3d11::draw_memo::Force::Off);
        d3d11::draw_memo::force_for_test(part, d3d11::draw_memo::Force::On);
        // Honest first: the part alone changes nothing.
        const Seen honest = run_sequence();
        EXPECT_EQ(honest.filters, cold.filters);
        EXPECT_EQ(honest.components, cold.components);
        EXPECT_EQ(pattern(honest.textures), pattern(cold.textures));
        EXPECT_EQ(pattern(honest.targets), pattern(cold.targets));
        EXPECT_EQ(honest.shader_lod_bias, cold.shader_lod_bias);
        EXPECT_EQ(pattern(honest.vertex_buffers), pattern(cold.vertex_buffers));
        d3d11::draw_memo::mutation_for_test().store(
            static_cast<int>(d3d11::draw_memo::Mutation::IgnoreKeys));
        const Seen broken = run_sequence();
        d3d11::draw_memo::mutation_for_test().store(0);
        switch (part) {
        case Part::StageMemo:
            EXPECT_NE(broken.shader_lod_bias, cold.shader_lod_bias);
            break;
        case Part::SamplerMemo:
            EXPECT_NE(broken.filters, cold.filters);
            break;
        case Part::IaMemo:
        case Part::KeyMemo:
            EXPECT_NE(broken.components, cold.components);
            break;
        case Part::SrvMemo:
            EXPECT_NE(pattern(broken.textures), pattern(cold.textures));
            break;
        case Part::OmMemo:
            EXPECT_NE(pattern(broken.targets), pattern(cold.targets));
            break;
        case Part::VbMemo:
            EXPECT_NE(pattern(broken.vertex_buffers),
                      pattern(cold.vertex_buffers));
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Views whose use had no oracle, and the line modes (d3d11_view_line_probe,
// measured on Windows; its shaders, fxc output, below).
// ---------------------------------------------------------------------------
#include "../../oracle_data/d3d11_view_line_shaders.h"

class D3D11ViewLineTest : public D3D11FixedFunctionTest {
protected:
    static constexpr GuestAddr VIEWDESC = PAGE + 0x1E00;
    /// Pointer arrays of the binding calls below.
    static constexpr GuestAddr BINDS = PAGE + 0x2000;

    void SetUp() override {
        D3D11FixedFunctionTest::SetUp();
        ASSERT_TRUE(memory.map_region(BINDS, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
    }

    GuestAddr texture_2d(std::initializer_list<u32> desc) {
        write_words(DESC, desc);
        EXPECT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
        return memory.read_u64(OUT).value();
    }
    /// Create* view @p slot of @p resource from @p desc words.
    GuestAddr view_of(usize slot, GuestAddr resource,
                      std::initializer_list<u32> desc) {
        write_words(VIEWDESC, desc);
        EXPECT_EQ(device_call(slot, {resource, VIEWDESC, OUT2}), d3d11::S_OK)
            << "view slot " << slot;
        return memory.read_u64(OUT2).value();
    }
    /// A structured UAV of @p records 16-byte records, for a CS's u0.
    GuestAddr structured_uav(u32 records) {
        write_words(DESC, {records * 16u, 0, 0x80, 0, 0x40, 16});
        EXPECT_EQ(device_call(3, {DESC, 0, OUT}), d3d11::S_OK);
        return view_of(8, memory.read_u64(OUT).value(),
                       {0, 1, 0, records, 0});
    }
    void dispatch(const unsigned char* blob, usize size,
                  std::initializer_list<GuestAddr> srvs,
                  std::initializer_list<GuestAddr> uavs,
                  GuestAddr sampler = 0) {
        context_call(69, {shader(18, blob, size), 0, 0});  // CSSetShader
        u32 i = 0;
        for (const GuestAddr v : srvs) {
            EXPECT_TRUE(memory.write_u64(BINDS + 8 * i++, v).is_ok());
        }
        context_call(67, {0, srvs.size(), BINDS});  // CSSetShaderResources
        i = 0;
        for (const GuestAddr v : uavs) {
            EXPECT_TRUE(memory.write_u64(BINDS + 0x80 + 8 * i++, v).is_ok());
        }
        context_call(68, {0, uavs.size(), BINDS + 0x80, 0});
        if (sampler != 0) {
            EXPECT_TRUE(memory.write_u64(BINDS + 0x100, sampler).is_ok());
            context_call(70, {0, 1, BINDS + 0x100});  // CSSetSamplers
        }
        context_call(41, {1, 1, 1});  // Dispatch
    }

    /// vs_pos_col's POSITION/COLOR layout over @p vertices (x, y, z, w,
    /// colour tag), with ps_col (or @p ps).
    void draw_pos_col(const std::vector<std::array<float, 5>>& vertices,
                      u32 topology,
                      const unsigned char* ps = g_ps_col,
                      usize ps_size = sizeof(g_ps_col)) {
        ASSERT_EQ(input_layout({{"POSITION", 2, 0, 0, 0, 0},
                                {"COLOR", 2, 0, 16, 0, 0}},
                               g_vs_pos_col, sizeof(g_vs_pos_col)),
                  d3d11::S_OK);
        context_call(17, {memory.read_u64(OUT).value()});
        std::vector<u32> words;
        for (const auto& v : vertices) {
            for (u32 k = 0; k < 4; ++k) words.push_back(std::bit_cast<u32>(v[k]));
            for (u32 k = 0; k < 3; ++k) words.push_back(std::bit_cast<u32>(v[4]));
            words.push_back(std::bit_cast<u32>(1.0f));
        }
        vertex_buffers({{buffer(0x1, words), 32, 0}});
        use_shaders(g_vs_pos_col, sizeof(g_vs_pos_col), ps, ps_size);
        context_call(24, {topology});
        context_call(13, {static_cast<u64>(vertices.size()), 0});  // Draw
    }
    /// RSSetState with {Fill, Cull, FrontCCW, DepthBias, Clamp, Slope,
    /// DepthClip, Scissor, Multisample, AntialiasedLine}.
    void rs(std::initializer_list<u32> words) {
        context_call(43, {rasterizer(words)});
    }
    graphics::LineQuadParams line_params() const {
        graphics::LineQuadParams params{};
        const auto& bytes = backend.state.last_draw_vertex_srv2_bytes;
        EXPECT_EQ(bytes.size(), sizeof(params));
        if (bytes.size() == sizeof(params)) {
            std::memcpy(&params, bytes.data(), sizeof(params));
        }
        return params;
    }
};

// W19: a TEXTURE2DMS(ARRAY) view of a one-sample texture binds as its 2D mip
// and the shader reads it through a 2D declaration (Windows, [ms1]: the
// view reads its one sample for every sample index; the MSL side is
// ViewLineGpu.SingleSampleMultisampleLoads).
TEST_F(D3D11ViewLineTest, SingleSampleMultisampleViewsReadThroughA2DVariant) {
    const GuestAddr single = texture_2d({4, 4, 1, 1, 28, 1, 0, 0, 0x8, 0, 0});
    const GuestAddr slices = texture_2d({4, 4, 1, 2, 28, 1, 0, 0, 0x8, 0, 0});
    const GuestAddr ms = view_of(7, single, {28, 6, 0, 0});
    const GuestAddr msa = view_of(7, slices, {28, 7, 0, 2});
    dispatch(g_cs_ms1, sizeof(g_cs_ms1), {ms, msa}, {structured_uav(27)});
    ASSERT_GE(backend.state.dispatches, 1u);
    const std::string& msl = backend.state.last_compute_msl;
    EXPECT_EQ(msl.find("texture2d_ms"), std::string::npos) << msl;
    EXPECT_NE(msl.find("texture2d<float> t0"), std::string::npos) << msl;
    EXPECT_NE(msl.find("texture2d_array<float> t1"), std::string::npos)
        << msl;
    // A multisampled resource's view keeps texture2d_ms.
    const GuestAddr four = texture_2d({4, 4, 1, 1, 28, 4, 0, 0, 0x8, 0, 0});
    dispatch(g_cs_ms1, sizeof(g_cs_ms1),
             {view_of(7, four, {28, 6, 0, 0}), msa}, {structured_uav(27)});
    EXPECT_NE(backend.state.last_compute_msl.find("texture2d_ms<float> t0"),
              std::string::npos)
        << backend.state.last_compute_msl;
}

// D3D11.3 7.18.9.1: a comparison filter compares against the border's red,
// any float -- Windows [cmp] "R32_FLOAT linear LESS border .5". Corrected:
// CreateSamplerState refused it (E_NOTIMPL). The shader compares through
// the border twin; the push slot holds (red, D3D11_COMPARISON_FUNC).
TEST_F(D3D11ViewLineTest, ComparisonBorderRedIsComparedInTheShader) {
    // Filter COMPARISON_MIN_MAG_MIP_LINEAR (0x95), BORDER x3, LESS (2),
    // border (0.5, 0, 0, 1).
    write_words(DESC, {0x95, 4, 4, 4, 0, 1, 2, 0x3F000000u, 0, 0,
                       0x3F800000u, 0, 0x7F7FFFFFu});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    const GuestAddr sampler = memory.read_u64(OUT).value();
    const GuestAddr depth = texture_2d({2, 2, 1, 1, 41, 1, 0, 0, 0x8, 0, 0});
    dispatch(g_cs_cmp, sizeof(g_cs_cmp), {view_of(7, depth, {41, 4, 0, 1})},
             {structured_uav(8)}, sampler);
    ASSERT_GE(backend.state.dispatches, 1u);
    EXPECT_NE(backend.state.last_compute_msl.find("s0_border_twin"),
              std::string::npos);
    const auto& push = backend.state.last_dispatch_constants;
    ASSERT_GE(push.size(), 20u);
    EXPECT_EQ(std::bit_cast<float>(push[16]), 0.5f) << "the border's red";
    EXPECT_EQ(std::bit_cast<float>(push[17]), 2.0f) << "LESS";
    // Red 0 and 1 stay Metal borders: no twin.
    write_words(DESC, {0x95, 4, 4, 4, 0, 1, 2, 0x3F800000u, 0, 0,
                       0x3F800000u, 0, 0x7F7FFFFFu});
    ASSERT_EQ(device_call(23, {DESC, OUT}), d3d11::S_OK);
    dispatch(g_cs_cmp, sizeof(g_cs_cmp), {view_of(7, depth, {41, 4, 0, 1})},
             {structured_uav(8)}, memory.read_u64(OUT).value());
    EXPECT_EQ(backend.state.last_compute_msl.find("s0_border_twin"),
              std::string::npos);
}

// W22 / B17: a draw into a buffer RTV renders a NumElements x 1 target whose
// texels are the window's elements -- filled from the buffer before the
// pass, copied back after it, at FirstElement * element size. Winoracle
// [bufrt] "R32_FLOAT {2,4} vp 4x1 draw": elements 2-5 written, the rest
// unchanged.
TEST_F(D3D11ViewLineTest, BufferRenderTargetViewDrawsIntoItsWindow) {
    write_words(DESC, {64, 0, 0x28, 0, 0, 0});  // SR|RT
    ASSERT_EQ(device_call(3, {DESC, 0, OUT}), d3d11::S_OK);
    const GuestAddr rtv =
        view_of(9, memory.read_u64(OUT).value(), {41, 1, 2, 4});
    ASSERT_TRUE(memory.write_u64(BINDS + 0x180, rtv).is_ok());
    context_call(33, {1, BINDS + 0x180, 0});  // OMSetRenderTargets
    write_words(DESC, {0, 0, 0x40800000u, 0x3F800000u, 0, 0x3F800000u});
    context_call(44, {1, DESC});  // a 4x1 viewport
    context_call(43, {rasterizer({3, 1, 0, 0, 0, 0, 1, 0, 0, 0})});
    const unsigned before = backend.state.draws;
    draw_pos_col({{-1, -1, 0.5f, 1, 1}, {-1, 3, 0.5f, 1, 1},
                  {3, -1, 0.5f, 1, 1}},
                 4, g_ps_bufrt, sizeof(g_ps_bufrt));
    ASSERT_EQ(backend.state.draws, before + 1);
    EXPECT_EQ(backend.state.last_pass_color0_desc.width, 4u);
    EXPECT_EQ(backend.state.last_pass_color0_desc.height, 1u);
    EXPECT_EQ(backend.state.last_pass_color0_desc.pixel_format,
              graphics::PixelFormat::R32Float);
    const auto& copies = backend.state.buffer_texture_row_copies;
    ASSERT_EQ(copies.size(), 2u);
    EXPECT_TRUE(copies[0].to_texture) << "the window, before the pass";
    EXPECT_EQ(copies[0].draws_before, before);
    EXPECT_FALSE(copies[1].to_texture) << "the result, after it";
    EXPECT_EQ(copies[1].draws_before, before + 1);
    for (const auto& copy : copies) {
        EXPECT_EQ(copy.offset, 8u) << "FirstElement 2 of 4-byte elements";
        EXPECT_EQ(copy.width, 4u);
    }
    // ClearRenderTargetView writes the window only ([bufrt] "clear").
    write_words(DESC, {0x3FC00000u, 0, 0, 0});
    context_call(50, {rtv, DESC});
    ASSERT_EQ(copies.size(), 3u);
    EXPECT_FALSE(copies[2].to_texture);
    EXPECT_EQ(copies[2].offset, 8u);
}

// D3D11.3 3.4.5 / Learn D3D11_RASTERIZER_DESC: MultisampleEnable draws a
// line as a 1.4-wide rectangle -- pass C of the vertex capture, six vertices
// a line, never culled, no depth bias (Windows d3d11_a07_rs_probe
// `R7 lines quad(MSAA)`, `R4 ... line`). Corrected: drawn aliased before.
TEST_F(D3D11ViewLineTest, QuadrilateralLinesDrawAsRectangles) {
    bind_target();
    rs({3, 3, 0, 100, 0, 0, 1, 0, 1, 0});  // cull BACK, bias 100, MS
    draw_pos_col({{-0.5f, 0, 0.5f, 1, 1}, {0.5f, 0.25f, 0.5f, 1, 1},
                  {0, -0.5f, 0.5f, 1, 1}, {0, 0.5f, 0.5f, 1, 1}},
                 2);  // LINELIST, two lines
    ASSERT_EQ(backend.state.capture_draws, 1u);
    EXPECT_EQ(backend.state.last_draw_topology,
              graphics::BackendPrimitiveTopology::TriangleList);
    EXPECT_EQ(backend.state.last_draw_vertex_count, 12u);
    EXPECT_NE(backend.state.last_pipeline_vertex_msl.find(
                  "vitreo_line_quad_params"),
              std::string::npos);
    EXPECT_EQ(backend.state.last_draw_cull_mode,
              graphics::BackendCullMode::None);
    EXPECT_EQ(backend.state.last_draw_depth_bias, 0.0f);
    const auto params = line_params();
    EXPECT_EQ(params.half_width, 0.7f);
    EXPECT_EQ(params.primitives, 2u);
    EXPECT_EQ(params.viewports[0][0], 16.0f);
    EXPECT_EQ(params.viewports[0][1], 16.0f);
    // The record ids of the two lines, in order.
    const auto& ids = backend.state.last_draw_vertex_srv1_bytes;
    ASSERT_EQ(ids.size(), 16u);
    std::array<u32, 4> words{};
    std::memcpy(words.data(), ids.data(), 16);
    EXPECT_EQ(words, (std::array<u32, 4>{0, 1, 2, 3}));
    // SV_PrimitiveID counts lines: Metal's triangles over two.
    EXPECT_FALSE(backend.state.last_pipeline_fragment_msl.empty());
}

// D3D11.3 3.4.4: AntialiasedLineEnable alone draws a 4-wide rectangle and
// the pixel shader scales o0.a by clamp(0.675 - 0.45 d) -- every cell of
// Windows `R7 lines alpha-AA` -- rasterized once per pixel on MSAA
// targets ([line] "4x alpha-AA" equals the 1x grid).
TEST_F(D3D11ViewLineTest, AlphaAntialiasedLinesScaleAlphaByCoverage) {
    backend.state.programmable_sample_positions = true;  // as on Apple GPUs
    write_words(DESC, {16, 16, 1, 1, 28, 4, 0, 0, 0x20, 0, 0});  // 4x
    ASSERT_EQ(device_call(5, {DESC, 0, OUT}), d3d11::S_OK);
    ASSERT_EQ(device_call(9, {memory.read_u64(OUT).value(), 0, OUT2}),
              d3d11::S_OK);
    ASSERT_TRUE(memory.write_u64(BINDS + 0x180,
                                 memory.read_u64(OUT2).value()).is_ok());
    context_call(33, {1, BINDS + 0x180, 0});
    write_words(DESC, {0, 0, 0x41800000u, 0x41800000u, 0, 0x3F800000u});
    context_call(44, {1, DESC});
    rs({3, 1, 0, 0, 0, 0, 1, 0, 0, 1});  // AA alone
    draw_pos_col({{-0.5f, 0, 0.5f, 1, 1}, {0.5f, 0.25f, 0.5f, 1, 1}}, 3);
    ASSERT_EQ(backend.state.capture_draws, 1u);
    EXPECT_EQ(backend.state.last_draw_vertex_count, 6u);
    EXPECT_EQ(line_params().half_width, 2.0f);
    const std::string& fragment = backend.state.last_pipeline_fragment_msl;
    EXPECT_NE(fragment.find("vitreo_line_equation"), std::string::npos)
        << fragment;
    EXPECT_NE(fragment.find("0.675"), std::string::npos) << fragment;
    EXPECT_TRUE(backend.state.last_pass_center_sample_positions);
    // Quadrilateral lines on the same target keep the sample pattern.
    rs({3, 1, 0, 0, 0, 0, 1, 0, 1, 1});
    draw_pos_col({{-0.5f, 0, 0.5f, 1, 1}, {0.5f, 0.25f, 0.5f, 1, 1}}, 3);
    EXPECT_FALSE(backend.state.last_pass_center_sample_positions);
    EXPECT_EQ(line_params().half_width, 0.7f);
    // Aliased lines on it are rasterized per pixel too ([line] "4x aliased").
    rs({3, 1, 0, 0, 0, 0, 1, 0, 0, 0});
    draw_pos_col({{-0.5f, 0, 0.5f, 1, 1}, {0.5f, 0.25f, 0.5f, 1, 1}}, 2);
    EXPECT_EQ(backend.state.last_draw_topology,
              graphics::BackendPrimitiveTopology::LineList);
    EXPECT_TRUE(backend.state.last_pass_center_sample_positions);
}

// D3D11.3 15.13: WIREFRAME edges are lines; in a line mode each triangle
// draws its three edge rectangles (18 vertices) after its own facing test,
// which the vertex shader applies (cull BACK -> LineQuadParams.cull 2), and
// keeps its depth bias (d3d11_a07_rs_probe `R4 ... wireframe`; [line]
// "quad wireframe triangle").
TEST_F(D3D11ViewLineTest, WireframeTrianglesDrawTheirEdgesAsRectangles) {
    bind_target();
    rs({2, 3, 0, 0, 0, 0, 1, 0, 1, 0});  // WIREFRAME, cull BACK, MS
    draw_pos_col({{-0.5f, -0.5f, 0.5f, 1, 1}, {0, 0.5f, 0.5f, 1, 1},
                  {0.5f, -0.5f, 0.5f, 1, 1}},
                 4);
    ASSERT_EQ(backend.state.capture_draws, 1u);
    EXPECT_EQ(backend.state.last_draw_vertex_count, 18u);
    EXPECT_EQ(backend.state.last_draw_fill_mode,
              graphics::BackendFillMode::Fill);
    EXPECT_EQ(backend.state.last_draw_cull_mode,
              graphics::BackendCullMode::None);
    EXPECT_EQ(line_params().cull, 2u);
    EXPECT_NE(backend.state.last_pipeline_vertex_msl.find("determinant"),
              std::string::npos);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
