// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_vt_pixel_atomics.cpp
/// @brief VtFeedbackTest, pixel-shader UAV atomics: the page-feedback report,
///        depth test, discard, helper lanes, raw buffers.
///
/// Split from the former test_d3d11_metal.cpp; the fixture is
/// d3d11_test_metal_vt_feedback.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_metal_vt_feedback.hpp"
#include "vt_feedback_shaders.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

// -- 2. Pixel-shader UAV atomics ---------------------------------------------

// The landscape's page-feedback shape at u4: one pixel per 16x16 tile, then
// one per 2x2 quad, adds 1 to element 0 and writes its key into the slot it
// got. On a single-sample target every covered pixel runs once (D3D11.3
// 16.12.2: "a deterministic set of Pixel Shader invocations"), so the counter
// is exactly the reporting pixel count and every entry is written once.
TEST_F(VtFeedbackTest, PixelAtomicsReportEveryTileAndQuadExactlyOnce) {
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    for (const u32 mask : {15u, 1u}) {
        SCOPED_TRACE(mask == 15u ? "one pixel per 16x16 tile"
                                 : "one pixel per 2x2 quad");
        const GuestAddr report = feedback_buffer();
        bind_feedback(ps, buffer_uav(report, 0, kFeedbackWords));
        if (HasFatalFailure()) return;
        constants({mask, 0, 0, 0});
        draw();
        context_call(33, {0, 0, 0});  // unbind
        const auto words = read_buffer(report, kFeedbackWords * 4);
        ASSERT_EQ(words.size(), kFeedbackWords);
        const auto keys = expected_keys(mask, 0, 0, kWidth, kHeight);
        const u32 n = mask == 15u ? 7u * 4u : 50u * 31u;
        ASSERT_EQ(keys.size(), n);
        EXPECT_EQ(words[0], n) << "the counter: one add per reporting pixel";
        expect_entries(words, keys, std::min<u32>(words[0], kFeedbackWords - 2));
    }
}

// A 4x multisampled target: a shader that does not run per sample runs
// once per pixel (D3D11.3 16.12.2's deterministic set of invocations; the
// samples share the one invocation), so the report is the single-sample
// one -- not one per covered sample.
TEST_F(VtFeedbackTest, PixelAtomicsOnAMultisampledTargetRunOncePerPixel) {
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    const GuestAddr report = feedback_buffer();
    bind_feedback(ps, buffer_uav(report, 0, kFeedbackWords), false, 1, 0, 4);
    if (HasFatalFailure()) return;
    constants({15, 0, 0, 0});
    draw();
    context_call(33, {0, 0, 0});
    const auto words = read_buffer(report, kFeedbackWords * 4);
    ASSERT_EQ(words.size(), kFeedbackWords);
    EXPECT_EQ(words[0], 28u) << "one report per pixel, not per sample";
    expect_entries(words, expected_keys(15, 0, 0, kWidth, kHeight),
                   std::min<u32>(words[0], 64));
}

// UAVStartSlot 4 behind four bound render targets (the deferred base pass's
// GBuffer): the report is the same.
TEST_F(VtFeedbackTest, PixelAtomicsBehindFourRenderTargets) {
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    const GuestAddr report = feedback_buffer();
    bind_feedback(ps, buffer_uav(report, 0, kFeedbackWords), false, 4);
    if (HasFatalFailure()) return;
    constants({15, 0, 0, 0});
    draw();
    context_call(33, {0, 0, 0});
    const auto words = read_buffer(report, kFeedbackWords * 4);
    ASSERT_EQ(words.size(), kFeedbackWords);
    EXPECT_EQ(words[0], 28u);
    expect_entries(words, expected_keys(15, 0, 0, kWidth, kHeight),
                   std::min<u32>(words[0], 64));
}

// Depth testing and UAV side effects (D3D11.3 16.12). Depth is cleared to 1
// and an occluder writes 0.25 over columns [0, 48); the reporting pass draws
// at 0.5 with LESS, so columns [0, 48) fail. Under forceEarlyDepthStencil
// (16.12.1) "if the tests do not pass, the Pixel Shader is not invoked": 16
// tiles report. Without it (16.12.2) a shader that accesses a UAV runs for
// every rasterized pixel -- early culling "must be disabled ... so
// applications can rely on a deterministic set of Pixel Shader invocations"
// -- so all 28 report, though the failing pixels' colours are discarded.
TEST_F(VtFeedbackTest, DepthTestAndUavSideEffects) {
    const GuestAddr early =
        shader(15, vtfb_ps_feedback_early, sizeof(vtfb_ps_feedback_early));
    const GuestAddr late = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    const GuestAddr occluder = shader(15, vtfb_ps_colour, sizeof(vtfb_ps_colour));
    constexpr u32 kD32Float = 40;  // DXGI_FORMAT_D32_FLOAT
    // D3D11_DEPTH_STENCIL_DESC: depth on, write ALL, LESS; stencil off.
    const std::array<u32, 13> dss_desc{1, 1, 2, 0, 0x0000FFFFu,
                                       1, 1, 1, 8, 1, 1, 1, 8};
    GuestAddr out = out_slot();
    ASSERT_EQ(device_call(21, {put(dss_desc), out}), d3d11::S_OK);
    const GuestAddr dss = memory.read_u64(out).value();
    for (const bool force_early : {true, false}) {
        SCOPED_TRACE(force_early ? "[earlydepthstencil]" : "default mode");
        const GuestAddr depth = texture(kWidth, kHeight, 1, 1, kD32Float,
                                        D3D11_USAGE_DEFAULT,
                                        D3D11_BIND_DEPTH_STENCIL, 0);
        out = out_slot();
        ASSERT_EQ(device_call(10, {depth, 0, out}), d3d11::S_OK);
        const GuestAddr dsv = memory.read_u64(out).value();
        // ClearDepthStencilView(dsv, DEPTH, 1.0f, 0): the float in XMM3.
        cpu.sse.xmm[3].u64_[0] = 0;
        cpu.sse.xmm[3].u64_[1] = 0;
        const float one = 1.0f;
        std::memcpy(&cpu.sse.xmm[3].u32_[0], &one, sizeof(one));
        context_call(53, {dsv, 1, 0, 0});
        const GuestAddr report = feedback_buffer();
        bind_feedback(force_early ? early : late,
                      buffer_uav(report, 0, kFeedbackWords), false, 1, dsv);
        if (HasFatalFailure()) return;
        context_call(36, {dss, 0});  // OMSetDepthStencilState
        constants({15, 0, 0, 0});
        // The occluder: z 0.5 through a viewport depth range [0, 0.5].
        context_call(9, {occluder, 0, 0});
        const std::array<float, 6> left{0, 0, 48, kHeight, 0, 0.5f};
        context_call(44, {1, put(left)});
        draw();
        context_call(9, {force_early ? early : late, 0, 0});
        viewport(0, 0, kWidth, kHeight);
        draw();
        context_call(33, {0, 0, 0});
        context_call(36, {0, 0});
        const auto words = read_buffer(report, kFeedbackWords * 4);
        ASSERT_EQ(words.size(), kFeedbackWords);
        const auto keys = force_early
            ? expected_keys(15, 48, 0, kWidth, kHeight)
            : expected_keys(15, 0, 0, kWidth, kHeight);
        EXPECT_EQ(words[0], keys.size());
        expect_entries(words, keys, std::min<u32>(words[0], 64));
    }
}

// The whole loop as an engine runs it, frame after frame: reset the report,
// draw, CopySubresourceRegion the report's used range (a box) into one of a
// ring of STAGING buffers, End an EVENT query behind it; a frame later poll
// that query's GetData until S_OK ("the GPU has finished the commands
// before it", Learn D3D11_QUERY_EVENT) and Map READ the staging buffer.
// Each frame's report is exactly that frame's -- never the previous one's
// left in the staging buffer.
TEST_F(VtFeedbackTest, FeedbackLoopFrameAfterFrame) {
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    const GuestAddr report = feedback_buffer();
    bind_feedback(ps, buffer_uav(report, 0, kFeedbackWords));
    if (HasFatalFailure()) return;
    std::vector<u32> reset(kFeedbackWords, kSentinel);
    reset[0] = 0;
    const GuestAddr reset_data = put(reset.data(), reset.size() * 4);
    constexpr u32 kRing = 2, kFrames = 6;
    std::array<GuestAddr, kRing> staging{};
    std::array<GuestAddr, kRing> query{};
    for (u32 i = 0; i < kRing; ++i) {
        staging[i] = buffer(kFeedbackWords * 4, D3D11_USAGE_STAGING, 0,
                            D3D11_CPU_ACCESS_READ, 0, 0, nullptr);
        const std::array<u32, 2> qd{D3D11_QUERY_EVENT, 0};
        const GuestAddr out = out_slot();
        ASSERT_EQ(device_call(24, {put(qd), out}), d3d11::S_OK);
        query[i] = memory.read_u64(out).value();
    }
    const GuestAddr done = out_slot();
    const auto mask_of = [](u32 frame) { return frame % 2 == 0 ? 15u : 1u; };
    const auto check = [&](u32 frame) {
        const u32 slot = frame % kRing;
        u64 hr = 1;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (hr != d3d11::S_OK &&
               std::chrono::steady_clock::now() < deadline) {
            // GetData(query, &done, sizeof(BOOL), 0)
            hr = context_call(29, {query[slot], done, 4, 0});
            if (hr != d3d11::S_OK) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        ASSERT_EQ(hr, d3d11::S_OK) << "frame " << frame << ": EVENT query";
        const Mapped m = map(staging[slot], 0, D3D11_MAP_READ);
        ASSERT_EQ(m.hr, d3d11::S_OK);
        const auto keys =
            expected_keys(mask_of(frame), 0, 0, kWidth, kHeight);
        // The copied range: the counter, the entries, one sentinel.
        const auto words = words_at(m.data, keys.size() + 2);
        unmap(staging[slot], 0);
        EXPECT_EQ(words[0], keys.size()) << "frame " << frame;
        expect_entries(words, keys,
                       std::min<u32>(words[0],
                                     static_cast<u32>(keys.size())));
    };
    for (u32 frame = 0; frame < kFrames; ++frame) {
        context_call(48, {report, 0, 0, reset_data, 0, 0});  // reset
        constants({mask_of(frame), 0, 0, 0});
        draw();
        const u32 used = static_cast<u32>(
            (2 + expected_keys(mask_of(frame), 0, 0, kWidth, kHeight).size()) *
            4);
        const std::array<u32, 6> box{0, 0, 0, used, 1, 1};
        // CopySubresourceRegion(staging, 0, 0, 0, 0, report, 0, &box).
        context_call(46, {staging[frame % kRing], 0, 0, 0, 0, report, 0,
                          put(box)});
        context_call(28, {query[frame % kRing]});  // End
        context_call(111, {});  // Flush
        if (frame >= 1) {
            check(frame - 1);
            if (HasFatalFailure()) return;
        }
    }
    check(kFrames - 1);
}

// The engine's readback shape, end to end (Architect's focused case). A
// RWStructuredBuffer bound with OMSetRenderTargetsAndUnorderedAccessViews(4,
// rtvs, dsv, UAVStartSlot 4, 1, &uav, &initialCount): Learn -- u# is
// absolute and UAVStartSlot >= NumRTVs -- so the shader's u4 is the buffer.
// An 8x8 draw adds 1 to buf[0] per pixel: CopyResource to STAGING, Map READ
// reads 64. Map READ | DO_NOT_WAIT right after the copy answers
// WAS_STILL_DRAWING or S_OK with 64, never older data. The UAV stays bound
// across frames (initialCount -1 keeps a counter; buf[0] is plain memory and
// accumulates); ClearUnorderedAccessViewUint on the structured view sets
// every 32-bit word to Values[0] (D3D11.3 5.2.3.2) and restarts the count.
// The fence: End(EVENT) after the copy, GetData(NULL, 0, DONOTFLUSH) polled
// across a Present and nothing else that flushes -- Learn
// (D3D11_ASYNC_GETDATA_FLAG) DONOTFLUSH sends nothing, so the Present is
// what submits -- until S_OK, then Map READ without DO_NOT_WAIT reads 64.
TEST_F(VtFeedbackTest, EngineReadbackShapeFourTargetsUavAtSlotFour) {
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    constexpr u32 kSide8 = 8, kWords = 128;
    constexpr u32 kD32Float = 40;  // DXGI_FORMAT_D32_FLOAT
    std::array<u64, 4> rtvs{};
    for (u32 i = 0; i < 4; ++i) {
        const GuestAddr target =
            texture(kSide8, kSide8, 1, 1, kRgba8Unorm, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET, 0);
        rtvs[i] = render_target_view(target, kRgba8Unorm, 0);
    }
    const GuestAddr depth = texture(kSide8, kSide8, 1, 1, kD32Float,
                                    D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_DEPTH_STENCIL, 0);
    GuestAddr out = out_slot();
    ASSERT_EQ(device_call(10, {depth, 0, out}), d3d11::S_OK);
    const GuestAddr dsv = memory.read_u64(out).value();
    cpu.sse.xmm[3].u64_[0] = 0;
    cpu.sse.xmm[3].u64_[1] = 0;
    const float one = 1.0f;
    std::memcpy(&cpu.sse.xmm[3].u32_[0], &one, sizeof(one));
    context_call(53, {dsv, 1, 0, 0});  // ClearDepthStencilView(DEPTH, 1)
    const GuestAddr buf =
        buffer(kWords * 4, D3D11_USAGE_DEFAULT,
               D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
               D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, nullptr);
    const GuestAddr uav = buffer_uav(buf, 0, kWords);
    const GuestAddr staging = buffer(kWords * 4, D3D11_USAGE_STAGING, 0,
                                     D3D11_CPU_ACCESS_READ, 0, 0, nullptr);
    pipeline(ps);
    if (HasFatalFailure()) return;
    viewport(0, 0, kSide8, kSide8);
    constants({0, 0, 0, 0});  // mask 0: every pixel adds
    const auto bind = [&] {
        const std::array<u64, 1> uavs{uav};
        const std::array<u32, 1> keep{0xFFFFFFFFu};
        // OMSetRenderTargetsAndUnorderedAccessViews(4, rtvs, dsv, 4, 1,
        // &uav, &initialCount = -1).
        context_call(34, {4, put(rtvs), dsv, 4, 1, put(uavs), put(keep)});
    };
    const auto counter = [&](bool do_not_wait_first) -> u32 {
        context_call(47, {staging, buf});  // CopyResource
        if (do_not_wait_first) {
            const Mapped early = map(staging, 0, D3D11_MAP_READ,
                                     D3D11_MAP_FLAG_DO_NOT_WAIT);
            if (early.hr == d3d11::S_OK) {
                const u32 word = memory.read_u32(early.data).value();
                unmap(staging, 0);
                return word;
            }
            EXPECT_EQ(early.hr, kDxgiErrorWasStillDrawing)
                << "DO_NOT_WAIT right after the copy";
        }
        const Mapped m = map(staging, 0, D3D11_MAP_READ);
        EXPECT_EQ(m.hr, d3d11::S_OK);
        if (m.hr != d3d11::S_OK) return ~0u;
        const u32 word = memory.read_u32(m.data).value();
        unmap(staging, 0);
        return word;
    };
    // ClearUnorderedAccessViewUint: every word of the structured view is
    // Values[0].
    context_call(51, {uav, put(std::array<u32, 4>{7, 8, 9, 10})});
    {
        const auto words = read_buffer(buf, kWords * 4);
        ASSERT_EQ(words.size(), kWords);
        EXPECT_EQ(std::count(words.begin(), words.end(), 7u),
                  static_cast<std::ptrdiff_t>(kWords))
            << "ClearUnorderedAccessViewUint on a structured view";
    }
    context_call(51, {uav, put(std::array<u32, 4>{0, 0, 0, 0})});
    bind();
    draw();
    EXPECT_EQ(counter(true), 64u) << "frame 1";
    // Frame 2: still bound, no clear -- buf[0] accumulates.
    draw();
    EXPECT_EQ(counter(true), 128u) << "frame 2, the UAV left bound";
    // Bound again with initialCount -1: nothing is reset.
    bind();
    draw();
    EXPECT_EQ(counter(false), 192u) << "frame 3, bound again with -1";
    // Clear, then the fence pattern across a Present.
    context_call(51, {uav, put(std::array<u32, 4>{0, 0, 0, 0})});
    draw();
    context_call(47, {staging, buf});  // CopyResource
    const std::array<u32, 2> qd{D3D11_QUERY_EVENT, 0};
    out = out_slot();
    ASSERT_EQ(device_call(24, {put(qd), out}), d3d11::S_OK);
    const GuestAddr query = memory.read_u64(out).value();
    context_call(28, {query});  // End
    // A windowless (composition) swap chain to Present: Width 8, Height 8,
    // B8G8R8A8, SampleDesc 1, RENDER_TARGET_OUTPUT, 2 buffers, FLIP_SEQUENTIAL.
    const std::array<u32, 12> scd{8, 8, 87, 0, 1, 0, 0x20, 2, 0, 3, 1, 0};
    out = out_slot();
    set_arg(0, 0xFACADE);
    set_arg(1, device);
    set_arg(2, put(scd));
    set_arg(3, 0);
    set_arg(4, out);
    d3d11.impl_CreateSwapChainForComposition(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr chain = memory.read_u64(out).value();
    ASSERT_NE(chain, 0u);
    u64 hr = 1;
    // Polls before the Present may answer S_FALSE (nothing was sent).
    for (int poll = 0; poll < 3 && hr != d3d11::S_OK; ++poll) {
        hr = context_call(29, {query, 0, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH});
        EXPECT_TRUE(hr == d3d11::S_OK || hr == 1u) << "GetData 0x" << std::hex
                                                   << hr;
    }
    set_arg(0, chain);
    set_arg(1, 0);
    set_arg(2, 0);
    d3d11.impl_SwapChainMethod(marshaler, 8);  // Present(0, 0)
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "Present";
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    u32 polls = 0;
    while (hr != d3d11::S_OK && std::chrono::steady_clock::now() < deadline) {
        hr = context_call(29, {query, 0, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH});
        ++polls;
        if (hr != d3d11::S_OK) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    ASSERT_EQ(hr, d3d11::S_OK)
        << "the EVENT query completes after the Present (" << polls
        << " DONOTFLUSH polls)";
    const Mapped m = map(staging, 0, D3D11_MAP_READ);
    ASSERT_EQ(m.hr, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(m.data).value(), 64u) << "after the fence";
    unmap(staging, 0);
}

// D3D11.3 16.13: a helper invocation -- run only to fill out a 2x2 stamp --
// has its atomic memory updates ignored. The scissor starts at (17, 17), so
// the pixels of column 16 and row 16 are outside it while their quad
// neighbours are inside: they run as helpers, and their tile corners
// (16, y) and (x, 16) must not report.
TEST_F(VtFeedbackTest, HelperLanesDoNotReport) {
    const GuestAddr ps = shader(15, vtfb_ps_feedback, sizeof(vtfb_ps_feedback));
    const GuestAddr report = feedback_buffer();
    bind_feedback(ps, buffer_uav(report, 0, kFeedbackWords), true);
    if (HasFatalFailure()) return;
    scissor(17, 17, kWidth, kHeight);
    constants({15, 0, 0, 0});
    draw();
    context_call(33, {0, 0, 0});
    const auto words = read_buffer(report, kFeedbackWords * 4);
    ASSERT_EQ(words.size(), kFeedbackWords);
    const auto keys = expected_keys(15, 17, 17, kWidth, kHeight);
    ASSERT_EQ(keys.size(), 5u * 2u);
    EXPECT_EQ(words[0], keys.size())
        << "only covered pixels report (helpers at x or y = 16 do not)";
    expect_entries(words, keys, std::min<u32>(words[0], kFeedbackWords - 2));
}

// D3D11.3 16.13: a discard before the atomic -- the pixel's atomic and store
// change nothing. A discard after the atomic -- the atomic is "in the past"
// and completes, the store after the discard changes nothing, so the slot
// it took stays the sentinel. Discarded: x & 16, the tile columns at x0 =
// 16, 48 and 80.
TEST_F(VtFeedbackTest, DiscardBeforeOrAfterTheAtomic) {
    const GuestAddr before = shader(15, vtfb_ps_feedback_discard_before,
                                    sizeof(vtfb_ps_feedback_discard_before));
    const GuestAddr after = shader(15, vtfb_ps_feedback_discard_after,
                                   sizeof(vtfb_ps_feedback_discard_after));
    const auto kept = expected_keys(15, 0, 0, kWidth, kHeight, 16);
    ASSERT_EQ(kept.size(), 4u * 4u);
    {
        SCOPED_TRACE("discard before the atomic");
        const GuestAddr report = feedback_buffer();
        bind_feedback(before, buffer_uav(report, 0, kFeedbackWords));
        if (HasFatalFailure()) return;
        constants({15, 16, 0, 0});
        draw();
        context_call(33, {0, 0, 0});
        const auto words = read_buffer(report, kFeedbackWords * 4);
        ASSERT_EQ(words.size(), kFeedbackWords);
        EXPECT_EQ(words[0], kept.size());
        expect_entries(words, kept, std::min<u32>(words[0], 64));
    }
    {
        SCOPED_TRACE("discard after the atomic");
        const GuestAddr report = feedback_buffer();
        bind_feedback(after, buffer_uav(report, 0, kFeedbackWords));
        if (HasFatalFailure()) return;
        constants({15, 16, 0, 0});
        draw();
        context_call(33, {0, 0, 0});
        const auto words = read_buffer(report, kFeedbackWords * 4);
        ASSERT_EQ(words.size(), kFeedbackWords);
        EXPECT_EQ(words[0], 28u) << "every tile's atomic completes";
        expect_entries(words, kept, std::min<u32>(words[0], 64),
                       28u - static_cast<u32>(kept.size()));
    }
}

// The same report through a RWByteAddressBuffer, with the other atomics
// (22.17): imm_atomic_iadd hands out the slots, atomic_or collects the tile
// columns, atomic_umax the largest key, imm_atomic_cmp_exch and
// atomic_cmp_store each set one word per tile from 0 -- exactly once, so
// no compare-exchange ever finds a non-zero word.
TEST_F(VtFeedbackTest, RawBufferAtomicsInAPixelShader) {
    const GuestAddr ps =
        shader(15, vtfb_ps_feedback_raw, sizeof(vtfb_ps_feedback_raw));
    constexpr u32 kBytes = 8192;
    std::vector<u32> init(kBytes / 4, 0);
    for (u32 i = 4096 / 4; i < kBytes / 4; ++i) init[i] = kSentinel;
    const GuestAddr raw = buffer(kBytes, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_UNORDERED_ACCESS, 0,
                                 D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0,
                                 init.data());
    bind_feedback(ps, buffer_uav(raw, kR32Typeless, kBytes / 4,
                                 1 /* D3D11_BUFFER_UAV_FLAG_RAW */));
    if (HasFatalFailure()) return;
    constants({15, 0, 7 /* tile columns */, 0});
    draw();
    context_call(33, {0, 0, 0});
    const auto words = read_buffer(raw, kBytes);
    ASSERT_EQ(words.size(), kBytes / 4);
    const auto keys = expected_keys(15, 0, 0, kWidth, kHeight);
    EXPECT_EQ(words[0], 28u) << "imm_atomic_iadd";
    EXPECT_EQ(words[1], 0x7Fu) << "atomic_or of the seven tile columns";
    EXPECT_EQ(words[2], (48u << 16) | 96u) << "atomic_umax of the keys";
    EXPECT_EQ(words[3], 0u) << "no compare-exchange found its word set";
    for (u32 t = 0; t < 28; ++t) {
        const u32 key = ((t / 7) * 16u << 16) | ((t % 7) * 16u);
        EXPECT_EQ(words[16 + t], key + 1) << "imm_atomic_cmp_exch tile " << t;
        EXPECT_EQ(words[256 + t], key + 1) << "atomic_cmp_store tile " << t;
    }
    std::vector<u32> entries(words.begin() + 1024 - 1, words.end());
    entries[0] = words[0];
    expect_entries(entries, keys, std::min<u32>(words[0], 64));
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
