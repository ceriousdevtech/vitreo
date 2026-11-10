// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_frame_bench.cpp
/// @brief The D3D12 frame benchmark's geometry passes through D3D11, for the
/// per-draw cost of the two front ends on the same scene.
///
/// tests/winapi/d3d12/test_d3d12_frame_bench.cpp records a deferred SM5
/// frame through d3d12.dll. Its depth prepass and four-target base pass map
/// directly onto D3D11 with the same shaders (tests/oracle_data/
/// d3d12_frame_bench, fxc 5.0): per draw, the engine's D3D11 pattern -- a
/// Map(WRITE_DISCARD) of the primitive constants, the material's twelve SRVs
/// and its constant buffer when the material changes, a rasterizer state
/// change every few draws (the D3D12 run's pipeline change), the vertex and
/// index buffers, DrawIndexed. The context runs on the calling thread and the
/// Metal worker encodes (frame_profile Encode), as in D3D12.
///
/// VITREO_D3D11_FRAME_BENCH=<frames> runs it; the D3D12 benchmark with
/// VITREO_D3D12_FRAME_BENCH_SUBSET=geometry records the same passes alone.

#include "d3d11_test_metal_fixture.hpp"

#include "graphics/common/frame_profile.hpp"
#include "graphics/common/gpu_busy.hpp"

#include "../../oracle_data/d3d12_frame_bench/d3d12_frame_bench_shaders.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>

namespace vitreo::winapi::d3d11_test {
namespace {

namespace fp = graphics::frame_profile;

class D3D11FrameBench : public D3D11MetalTest {
protected:
    static constexpr u32 kWidth = 1600;
    static constexpr u32 kHeight = 900;
    static constexpr u32 kPrepassDraws = 1500;
    static constexpr u32 kBaseDraws = 1500;
    static constexpr u32 kMaterials = 64;
    static constexpr u32 kSrvs = 12;
    static constexpr u32 kTextures = 48;
    static constexpr u32 kIndices = 360;
    static constexpr u32 kVertices = 65536;
    static constexpr u32 kPsoRun = 4;

    GuestAddr make_buffer(u32 bytes, u32 usage, u32 bind, u32 cpu, const void* data) {
        const std::array<u32, 6> bd{bytes, usage, bind, cpu, 0, 0};
        // Initial data goes through a staging region of its own: the
        // fixture's bump area is too small for the vertex buffers.
        GuestAddr init = 0;
        if (data != nullptr) {
            constexpr GuestAddr kStaging = 0x20000000;
            constexpr usize kStagingSize = 4u << 20;
            if (!staging_mapped) {
                EXPECT_TRUE(memory.map_region(kStaging, kStagingSize,
                                              MemoryProtection::ReadWrite).is_ok());
                staging_mapped = true;
            }
            EXPECT_LE(bytes, kStagingSize);
            (void)memory.write_bytes(kStaging, static_cast<const Byte*>(data), bytes);
            init = put(std::array<u64, 2>{kStaging, 0});
        }
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(3, {put(bd), init, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    GuestAddr texture(u32 w, u32 h, u32 format, u32 bind) {
        const std::array<u32, 11> desc{w, h, 1, 1, format, 1, 0, 0, bind, 0, 0};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(5, {put(desc), 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    GuestAddr view(usize slot, GuestAddr resource) {
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(slot, {resource, 0, out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }

    void build() {
        vs_base = shader(12, g_bench_vs_base, sizeof(g_bench_vs_base));
        vs_depth = shader(12, g_bench_vs_depth, sizeof(g_bench_vs_depth));
        ps_base = shader(15, g_bench_ps_base, sizeof(g_bench_ps_base));
        // Input layouts: position in slot 0; attributes in slot 1.
        {
            const GuestAddr name = put("ATTRIBUTE", 10);
            const auto layout = [&](bool attributes, const unsigned char* code,
                                    usize size) {
                std::vector<u8> descs;
                const auto add = [&](u32 index, u32 format, u32 slot, u32 offset) {
                    std::array<u8, 32> d{};
                    std::memcpy(d.data(), &name, 8);
                    const std::array<u32, 6> f{index, format, slot, offset, 0, 0};
                    std::memcpy(d.data() + 8, f.data(), sizeof(f));
                    descs.insert(descs.end(), d.begin(), d.end());
                };
                add(0, 6 /* R32G32B32_FLOAT */, 0, 0);
                if (attributes) {
                    add(1, 6, 1, 0);
                    add(2, 16 /* R32G32_FLOAT */, 1, 12);
                    add(3, 2 /* R32G32B32A32_FLOAT */, 1, 20);
                }
                const GuestAddr array = put(descs.data(), descs.size());
                const GuestAddr bytecode = put(code, size);
                const GuestAddr out = out_slot();
                EXPECT_EQ(device_call(11, {array, descs.size() / 32, bytecode, size, out}),
                          d3d11::S_OK);
                return memory.read_u64(out).value();
            };
            layout_base = layout(true, g_bench_vs_base, sizeof(g_bench_vs_base));
            layout_depth = layout(false, g_bench_vs_depth, sizeof(g_bench_vs_depth));
        }
        // Geometry: as the D3D12 benchmark's fill_geometry.
        std::vector<float> pos(kVertices * 3);
        std::vector<float> attr(kVertices * 9);
        u32 seed = 12345;
        const auto rnd = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        for (u32 v = 0; v + 2 < kVertices; v += 3) {
            const float cx = v == 0 ? -1.0f : rnd() * 1.9f - 0.95f;
            const float cy = v == 0 ? 1.0f : rnd() * 1.9f - 0.95f;
            const float tri[3][2] = {{cx, cy}, {cx + 0.06f, cy}, {cx, cy - 0.06f}};
            for (u32 k = 0; k < 3; ++k) {
                pos[(v + k) * 3 + 0] = tri[k][0];
                pos[(v + k) * 3 + 1] = tri[k][1];
                pos[(v + k) * 3 + 2] = 0.5f;
                float* a = &attr[(v + k) * 9];
                const float values[9] = {0, 0, 1, 0.5f, 0.5f, 1, 0, 0, 1};
                std::memcpy(a, values, sizeof(values));
            }
        }
        std::vector<u16> idx(97 * 512, 0);
        for (u32 m = 0; m < 97; ++m) {
            for (u32 i = 0; i < kIndices; ++i) idx[m * 512 + i] = static_cast<u16>(i);
        }
        positions = make_buffer(kVertices * 12, 0, 1, 0, pos.data());
        attributes = make_buffer(kVertices * 36, 0, 1, 0, attr.data());
        indices = make_buffer(static_cast<u32>(idx.size() * 2), 0, 2, 0, idx.data());
        std::vector<float> block(64, 0.0f);
        for (u32 k = 0; k < 4; ++k) block[k * 4 + k] = 1.0f;
        for (u32 k = 16; k < 20; ++k) block[k] = 0.5f;
        block[20] = block[21] = 1.0f;
        view_cb = make_buffer(256, 0, 4, 0, block.data());
        for (u32 m = 0; m < kMaterials; ++m) {
            std::vector<float> material(64, 0.25f);
            material_cbs[m] = make_buffer(256, 0, 4, 0, material.data());
        }
        pass_cb = make_buffer(256, 0, 4, 0, block.data());
        // DYNAMIC, CPU write: the per-draw primitive constants.
        primitive_cb = make_buffer(256, 2, 4, 0x10000, nullptr);
        for (u32 t = 0; t < kTextures; ++t) {
            textures[t] = texture(64, 64, 28 /* R8G8B8A8_UNORM */, 8);
            srvs[t] = view(7, textures[t]);
        }
        for (u32 k = 0; k < 4; ++k) {
            gbuffer[k] = texture(kWidth, kHeight, 10 /* R16G16B16A16_FLOAT */, 0x20 | 8);
            rtvs[k] = view(9, gbuffer[k]);
        }
        depth = texture(kWidth, kHeight, 40 /* D32_FLOAT */, 0x40);
        dsv = view(10, depth);
        // Samplers: three linear, one point, wrap.
        for (u32 k = 0; k < 4; ++k) {
            std::array<u32, 13> sd{k == 3 ? 0u : 0x15u, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0};
            const float max_lod = 1000.0f;
            std::memcpy(&sd[12], &max_lod, 4);
            const GuestAddr out = out_slot();
            EXPECT_EQ(device_call(23, {put(sd), out}), d3d11::S_OK);
            samplers[k] = memory.read_u64(out).value();
        }
        // Rasterizer states: the D3D12 run's depth-bias / cull variants.
        for (u32 v = 0; v < 24; ++v) {
            const std::array<u32, 10> rs{3, v % 3 == 0 ? 1u : 3u, 0, v, 0, 0, 1, 0, 0, 0};
            const GuestAddr out = out_slot();
            EXPECT_EQ(device_call(22, {put(rs), out}), d3d11::S_OK);
            raster[v] = memory.read_u64(out).value();
        }
        // Depth: prepass test-and-write, base pass test only (GREATER_EQUAL).
        for (u32 w = 0; w < 2; ++w) {
            std::array<u32, 13> ds{1, w, 7, 0, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8};
            ds[4] = 0xFFFF;  // StencilReadMask, StencilWriteMask (UINT8 x2)
            const GuestAddr out = out_slot();
            EXPECT_EQ(device_call(21, {put(ds), out}), d3d11::S_OK);
            depth_state[w] = memory.read_u64(out).value();
        }
        {
            std::array<u32, 66> bd{};
            for (u32 t = 0; t < 8; ++t) {
                bd[2 + t * 8 + 1] = 2;  // SrcBlend ONE
                bd[2 + t * 8 + 2] = 1;  // DestBlend ZERO
                bd[2 + t * 8 + 3] = 1;  // ADD
                bd[2 + t * 8 + 4] = 2;
                bd[2 + t * 8 + 5] = 1;
                bd[2 + t * 8 + 6] = 1;
                bd[2 + t * 8 + 7] = 0xF;  // RenderTargetWriteMask
            }
            const GuestAddr out = out_slot();
            EXPECT_EQ(device_call(20, {put(bd), out}), d3d11::S_OK);
            blend = memory.read_u64(out).value();
        }
        scratch = put(std::array<u8, 1024>{});
        mapped = put(std::array<u64, 2>{0, 0});
    }

    void w64(GuestAddr at, u64 v) { (void)memory.write_u64(at, v); }
    void w32(GuestAddr at, u32 v) { (void)memory.write_u32(at, v); }

    void bind_mesh(u32 draw, bool attributes_too) {
        const u32 mesh = draw % 97;
        w64(scratch + 0x00, positions);
        w64(scratch + 0x08, attributes);
        w32(scratch + 0x10, 12);
        w32(scratch + 0x14, 36);
        w32(scratch + 0x18, mesh * 600 * 12);
        w32(scratch + 0x1C, mesh * 600 * 36);
        context_call(18, {0, attributes_too ? 2u : 1u, scratch, scratch + 0x10,
                          scratch + 0x18});
        context_call(19, {indices, 57 /* R16_UINT */, mesh * 1024u});
    }
    void primitive_constants(u32 draw) {
        // Map(WRITE_DISCARD), the block, Unmap; bound at b1 of both stages.
        EXPECT_EQ(context_call(14, {primitive_cb, 0, 4, 0, mapped}), d3d11::S_OK);
        const GuestAddr data = memory.read_u64(mapped).value();
        std::array<float, 32> block{};
        for (u32 k = 0; k < 4; ++k) block[k * 4 + k] = 1.0f;
        block[16] = static_cast<float>(draw & 7) * 0.1f;
        (void)memory.write_bytes(data, reinterpret_cast<const Byte*>(block.data()),
                                 sizeof(block));
        context_call(15, {primitive_cb, 0});
    }

    void frame() {
        const std::array<float, 6> viewport{0, 0, kWidth, kHeight, 0, 1};
        const GuestAddr vp = scratch + 0x100;
        (void)memory.write_bytes(vp, reinterpret_cast<const Byte*>(viewport.data()),
                                 sizeof(viewport));
        w64(scratch + 0x140, view_cb);
        w64(scratch + 0x148, primitive_cb);
        w64(scratch + 0x150, 0);  // b2: the material, below
        w64(scratch + 0x158, pass_cb);
        // Prepass.
        context_call(53, {dsv, 1 /* DEPTH */, 0, 0});
        context_call(33, {0, 0, dsv});
        context_call(44, {1, vp});
        context_call(24, {4 /* TRIANGLELIST */});
        context_call(17, {layout_depth});
        context_call(11, {vs_depth, 0, 0});
        context_call(9, {0, 0, 0});
        context_call(36, {depth_state[1], 0});
        context_call(7, {0, 2, scratch + 0x140});  // VSSetConstantBuffers b0, b1
        for (u32 d = 0; d < kPrepassDraws; ++d) {
            if (d % kPsoRun == 0) context_call(43, {raster[(d / kPsoRun) % 4]});
            primitive_constants(d);
            bind_mesh(d, false);
            context_call(12, {kIndices, 0, 0});
        }
        // Base pass: four targets, depth test only.
        for (u32 k = 0; k < 4; ++k) w64(scratch + 0x180 + k * 8, rtvs[k]);
        const std::array<float, 4> black{0, 0, 0, 0};
        (void)memory.write_bytes(scratch + 0x1C0, reinterpret_cast<const Byte*>(black.data()),
                                 sizeof(black));
        for (u32 k = 0; k < 4; ++k) context_call(50, {rtvs[k], scratch + 0x1C0});
        context_call(33, {4, scratch + 0x180, dsv});
        context_call(17, {layout_base});
        context_call(11, {vs_base, 0, 0});
        context_call(9, {ps_base, 0, 0});
        context_call(36, {depth_state[0], 0});
        context_call(35, {blend, 0, 0xFFFFFFFFu});
        for (u32 k = 0; k < 4; ++k) w64(scratch + 0x200 + k * 8, samplers[k]);
        context_call(10, {0, 4, scratch + 0x200});   // PSSetSamplers
        context_call(26, {0, 1, scratch + 0x200});   // VSSetSamplers
        w64(scratch + 0x220, srvs[0]);
        context_call(25, {0, 1, scratch + 0x220});   // VSSetShaderResources
        u32 material = ~0u;
        for (u32 d = 0; d < kBaseDraws; ++d) {
            if (d % kPsoRun == 0) context_call(43, {raster[(d / kPsoRun) % 24]});
            const u32 m = (d / 2) % kMaterials;
            if (m != material) {
                for (u32 k = 0; k < kSrvs; ++k) {
                    w64(scratch + 0x240 + k * 8, srvs[(m * 7 + k) % kTextures]);
                }
                context_call(8, {0, kSrvs, scratch + 0x240});  // PSSetShaderResources
                w64(scratch + 0x150, material_cbs[m]);
                context_call(16, {0, 4, scratch + 0x140});     // PSSetConstantBuffers
                material = m;
            }
            primitive_constants(d);
            bind_mesh(d, true);
            context_call(12, {kIndices, 0, 0});
        }
        // Unbind the targets (as the lighting pass would) and submit.
        context_call(33, {0, 0, 0});
        context_call(111, {});  // Flush
    }

    GuestAddr vs_base{0}, vs_depth{0}, ps_base{0}, layout_base{0}, layout_depth{0};
    GuestAddr positions{0}, attributes{0}, indices{0}, view_cb{0}, pass_cb{0},
        primitive_cb{0};
    std::array<GuestAddr, kMaterials> material_cbs{};
    std::array<GuestAddr, kTextures> textures{}, srvs{};
    std::array<GuestAddr, 4> gbuffer{}, rtvs{}, samplers{};
    GuestAddr depth{0}, dsv{0}, blend{0};
    std::array<GuestAddr, 24> raster{};
    std::array<GuestAddr, 2> depth_state{};
    GuestAddr scratch{0}, mapped{0};
    bool staging_mapped{false};
};

TEST_F(D3D11FrameBench, GeometryPasses) {
    const char* frames_env = std::getenv("VITREO_D3D11_FRAME_BENCH");
    const u32 frames = frames_env != nullptr ? static_cast<u32>(std::atoi(frames_env)) : 0u;
    if (frames == 0) GTEST_SKIP() << "VITREO_D3D11_FRAME_BENCH=<frames> runs it";
    fp::set_enabled_for_testing(true);
    build();
    if (HasFatalFailure()) return;
    auto& backend = graphics::MetalDevice::instance();
    std::deque<std::shared_ptr<graphics::GpuCompletion>> in_flight;
    const auto one = [&]() {
        const u64 start = fp::now_ns();
        frame();
        fp::add(fp::Time::ApiRecord, fp::now_ns() - start);
        in_flight.push_back(backend.flush_completion());
        while (in_flight.size() > 2) {
            const u64 wait = fp::now_ns();
            if (in_flight.front()) (void)in_flight.front()->wait();
            fp::note_wait("frame n-2", fp::now_ns() - wait);
            in_flight.pop_front();
        }
    };
    for (u32 i = 0; i < 30; ++i) one();
    for (auto& c : in_flight) if (c) (void)c->wait();
    in_flight.clear();
    (void)fp::take();
    char reasons[4096];
    fp::format_reasons(reasons, sizeof(reasons));
    graphics::reset_gpu_busy_for_testing();
    const u64 start = fp::now_ns();
    for (u32 i = 0; i < frames; ++i) one();
    for (auto& c : in_flight) if (c) (void)c->wait();
    const u64 wall = fp::now_ns() - start;
    const fp::Snapshot snap = fp::take();
    fp::format_reasons(reasons, sizeof(reasons));
    const graphics::RecentGpuBusy busy =
        graphics::recent_gpu_busy(graphics::gpu_busy_clock_s());
    const double per = static_cast<double>(frames);
    const double seconds = static_cast<double>(wall) / 1e9;
    const double frames_in_window = std::min(1.0, seconds) * per / seconds;
    const auto ms = [&](fp::Time t) {
        return static_cast<double>(snap.times[static_cast<usize>(t)]) / 1e6 / per;
    };
    const auto count = [&](fp::Counter c) {
        return static_cast<double>(snap.counters[static_cast<usize>(c)]) / per;
    };
    std::printf(
        "[d3d11-frame-bench] frames=%u wall=%.2fms/frame context=%.2f encode=%.2f "
        "cpu-wait=%.2f gpu=%.2f draws=%.0f encoded=%.0f render-enc=%.1f "
        "residency=%.0f\n  %s\n",
        frames, static_cast<double>(wall) / 1e6 / per, ms(fp::Time::ApiRecord),
        ms(fp::Time::Encode), ms(fp::Time::CpuWait),
        frames_in_window > 0 ? busy.busy_ms / frames_in_window : 0.0,
        static_cast<double>(kPrepassDraws + kBaseDraws),
        count(fp::Counter::EncodedDraws), count(fp::Counter::RenderEncoders),
        count(fp::Counter::ResidencyDecls), reasons);
    EXPECT_GE(snap.counters[static_cast<usize>(fp::Counter::EncodedDraws)],
              static_cast<u64>(kPrepassDraws + kBaseDraws) * frames);
    fp::set_enabled_for_testing(false);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
