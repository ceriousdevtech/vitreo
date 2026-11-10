// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_map_zero_copy_bench.cpp
/// @brief WS6 gate: CPU cost of Map(WRITE_DISCARD) + Unmap of a 64 KiB
///        DYNAMIC constant buffer, shadow path (arm A, the VITREO_WS6=0 kill
///        switch) against the zero-copy ring (arm B).
///
/// One frame is 1,000 x (Map DISCARD, write W bytes, Unmap, bind, use) then
/// Flush; the use reads cb0, so every DISCARD finds its slice in use and the
/// ring renames each time. Two variants: the plan's draw (PSSetConstantBuffers
/// + Draw of vtfb_ps_uint into a 1x1 target) and a dispatch
/// (CSSetConstantBuffers + Dispatch of vtfb_cs_fill). Map+Unmap and
/// bind+use are timed separately, so a cost the ring adds on the bind side
/// (slab-mates sharing one MTLBuffer, say) shows too. W in {256, 4096,
/// 65536}; per W, 5 reps with the arms interleaved (A, B, A, B, ...), each
/// rep 2 warm-up frames and 20 timed ones; the gate compares the medians of
/// the reps: Map+Unmap of B at least 2x cheaper at 64 KiB and no slower
/// than A (5% noise allowance) at 256 B; bind+use of B no slower than A
/// (10% allowance: it is mostly encoding). Runs only with
/// VITREO_WS6_BENCH=1, on an otherwise idle machine.

#include <gtest/gtest.h>

#include "graphics/metal/metal_submit_queue.hpp"
#include "winapi/graphics/d3d11/d3d11_buffer_ring.hpp"

#include "../winapi/d3d11/d3d11_test_metal_vt_feedback.hpp"
#include "../winapi/d3d11/vt_feedback_shaders.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vitreo::winapi::d3d11_test {

class D3D11MapZeroCopyBench : public VtFeedbackTest {
protected:
    void TearDown() override {
        d3d11_ws6_override_for_test(std::nullopt);
        graphics::MetalSubmitQueue::instance().drain();
        VtFeedbackTest::TearDown();
    }

    struct Cost {
        double map_ns{0};   // per Map+Unmap pair
        double bind_ns{0};  // per bind + draw / dispatch
    };
    /// One arm: @p ring on or off, W = @p write_bytes, a draw or a dispatch.
    Cost run_arm(bool ring, u32 write_bytes, bool draw) {
        d3d11_ws6_override_for_test(ring);
        const GuestAddr cb =
            buffer(65536, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                   D3D11_CPU_ACCESS_WRITE, 0, 0, nullptr);
        if (m_scratch == 0) m_scratch = put(std::array<u64, 4>{0, 0, 0, 0});
        EXPECT_TRUE(memory.write_u64(m_scratch, cb).is_ok());
        const GuestAddr mapped = m_scratch + 16;
        GuestAddr out = 0, uav = 0, target = 0, rtv = 0;
        if (draw) {
            if (m_ps == 0) m_ps = shader(15, vtfb_ps_uint, sizeof(vtfb_ps_uint));
            pipeline(m_ps);
            target = texture(1, 1, 1, 1, kR32Uint, D3D11_USAGE_DEFAULT,
                             D3D11_BIND_RENDER_TARGET, 0);
            rtv = render_target_view(target, kR32Uint, 0);
            viewport(0, 0, 1, 1);
            EXPECT_TRUE(memory.write_u64(m_scratch + 8, rtv).is_ok());
            context_call(33, {1, m_scratch + 8, 0});  // OMSetRenderTargets
        } else {
            out = buffer(256, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS,
                         0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, nullptr);
            uav = buffer_uav(out, 0, 64);
            if (m_cs == 0) m_cs = shader(18, vtfb_cs_fill, sizeof(vtfb_cs_fill));
            EXPECT_TRUE(memory.write_u64(m_scratch + 8, uav).is_ok());
            context_call(69, {m_cs, 0, 0});              // CSSetShader
            context_call(68, {0, 1, m_scratch + 8, 0});  // CSSetUnorderedAccessViews
        }
        std::vector<u8> payload(write_bytes, 0x5A);
        constexpr int kWarmup = 2;
        constexpr int kFrames = 20;
        constexpr int kPerFrame = 1000;
        u64 map_ns = 0;
        u64 bind_ns = 0;
        const auto ns = [](auto d) {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
        };
        for (int frame = 0; frame < kWarmup + kFrames; ++frame) {
            for (int i = 0; i < kPerFrame; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                const u64 hr = context_call(
                    14, {cb, 0, D3D11_MAP_WRITE_DISCARD, 0, mapped});
                const auto t1 = std::chrono::steady_clock::now();
                EXPECT_EQ(hr, d3d11::S_OK);
                const GuestAddr data = memory.read_u64(mapped).value();
                payload[0] = static_cast<u8>(i);
                (void)memory.write_bytes(data, payload.data(), write_bytes);
                const auto t2 = std::chrono::steady_clock::now();
                context_call(15, {cb, 0});  // Unmap
                const auto t3 = std::chrono::steady_clock::now();
                if (draw) {
                    context_call(16, {0, 1, m_scratch});  // PSSetConstantBuffers
                    context_call(13, {3, 0});             // Draw(3, 0)
                } else {
                    context_call(71, {0, 1, m_scratch});  // CSSetConstantBuffers
                    context_call(41, {1, 1, 1});          // Dispatch
                }
                const auto t4 = std::chrono::steady_clock::now();
                if (frame >= kWarmup) {
                    map_ns += ns((t1 - t0) + (t3 - t2));
                    bind_ns += ns(t4 - t3);
                }
            }
            context_call(111, {});  // Flush
        }
        graphics::MetalSubmitQueue::instance().drain();
        if (draw) {
            context_call(33, {0, 0, 0});
            set_arg(0, rtv);
            d3d11.impl_RenderTargetViewMethod(marshaler, 2);  // Release
            set_arg(0, target);
            d3d11.impl_Texture2DMethod(marshaler, 2);  // Release
        } else {
            set_arg(0, uav);
            d3d11.impl_UnorderedAccessViewMethod(marshaler, 2);  // Release
            set_arg(0, out);
            d3d11.impl_BufferMethod(marshaler, 2);  // Release
        }
        set_arg(0, cb);
        d3d11.impl_BufferMethod(marshaler, 2);  // Release
        const double calls = static_cast<double>(kFrames) * kPerFrame;
        return {static_cast<double>(map_ns) / calls,
                static_cast<double>(bind_ns) / calls};
    }

    void run_gate(bool draw) {
        constexpr int kReps = 5;
        const auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const char* const use = draw ? "bind+Draw" : "bind+Dispatch";
        std::printf("[ws6-bench] %s variant: 64 KiB DYNAMIC CB, 1000 per "
                    "frame, 20 frames per rep, median of 5 interleaved reps\n",
                    draw ? "draw" : "dispatch");
        std::printf("[ws6-bench] %6s | %-34s | %-34s | %7s %7s %5s | %7s %7s "
                    "%5s\n",
                    "W", "Map+Unmap ns A (VITREO_WS6=0)", "Map+Unmap ns B (ring)",
                    "med A", "med B", "A/B", use, "B", "A/B");
        double a64 = 0, b64 = 0, a256 = 0, b256 = 0;
        for (const u32 w : {256u, 4096u, 65536u}) {
            std::vector<double> a, b, ab, bb;
            for (int rep = 0; rep < kReps; ++rep) {
                const Cost ca = run_arm(false, w, draw);
                const Cost cb = run_arm(true, w, draw);
                a.push_back(ca.map_ns);
                b.push_back(cb.map_ns);
                ab.push_back(ca.bind_ns);
                bb.push_back(cb.bind_ns);
            }
            const double ma = median(a), mb = median(b);
            const double mab = median(ab), mbb = median(bb);
            char ra[64] = {}, rb[64] = {};
            std::snprintf(ra, sizeof(ra), "%.0f %.0f %.0f %.0f %.0f", a[0],
                          a[1], a[2], a[3], a[4]);
            std::snprintf(rb, sizeof(rb), "%.0f %.0f %.0f %.0f %.0f", b[0],
                          b[1], b[2], b[3], b[4]);
            std::printf("[ws6-bench] %6u | %-34s | %-34s | %7.0f %7.0f %5.2f | "
                        "%7.0f %7.0f %5.2f\n",
                        w, ra, rb, ma, mb, ma / mb, mab, mbb, mab / mbb);
            EXPECT_LE(mbb, mab * 1.10)
                << "WS6 gate: no regression on the " << use << " side, W=" << w;
            if (w == 65536u) { a64 = ma; b64 = mb; }
            if (w == 256u) { a256 = ma; b256 = mb; }
        }
        EXPECT_LE(b64 * 2.0, a64) << "WS6 gate: >= 2x CPU reduction at 64 KiB";
        EXPECT_LE(b256, a256 * 1.05) << "WS6 gate: no regression at 256 B";
    }

    static bool enabled() {
        const char* value = std::getenv("VITREO_WS6_BENCH");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }

    GuestAddr m_ps{0};
    GuestAddr m_cs{0};
    GuestAddr m_scratch{0};
};

// The plan's form: Map DISCARD, write, Unmap, bind, draw.
TEST_F(D3D11MapZeroCopyBench, DiscardConstantBufferPerDraw) {
    if (!enabled()) GTEST_SKIP() << "set VITREO_WS6_BENCH=1 on an idle machine";
    run_gate(/*draw=*/true);
}

TEST_F(D3D11MapZeroCopyBench, DiscardConstantBufferPerDispatch) {
    if (!enabled()) GTEST_SKIP() << "set VITREO_WS6_BENCH=1 on an idle machine";
    run_gate(/*draw=*/false);
}

}  // namespace vitreo::winapi::d3d11_test
