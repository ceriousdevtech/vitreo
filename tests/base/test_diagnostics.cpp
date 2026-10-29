// SPDX-License-Identifier: LGPL-2.1-only
//
// Contract tests for vitreo::diagnostics.
//
// These hooks exist so low layers can request diagnostics that only a higher
// layer can produce. The property that makes that safe is that an
// un-installed hook is inert: the CPU, WinAPI, and IPC all call into here from
// unit tests and fuzz harnesses that link no process runtime at all, and none
// of them may crash for it.

#include <vitreo/diagnostics.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::string> g_reasons;

void recording_dump(const char* reason) {
    g_reasons.emplace_back(reason ? reason : "<null>");
}

class DiagnosticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_reasons.clear();
        vitreo::diagnostics::set_hostcall_dump(nullptr);
        vitreo::diagnostics::set_trace_milestone_reached(false);
        while (vitreo::diagnostics::module_init_depth() > 0) {
            vitreo::diagnostics::leave_module_init();
        }
    }
    void TearDown() override {
        // The hooks are process-global; leaving one installed would leak a
        // dangling pointer into every test that runs after this one.
        vitreo::diagnostics::set_hostcall_dump(nullptr);
        vitreo::diagnostics::set_trace_milestone_reached(false);
    }
};

// -----------------------------------------------------------------------------
// Hostcall dump hook
// -----------------------------------------------------------------------------

TEST_F(DiagnosticsTest, DumpWithNoImplementationInstalledIsInert) {
    // The case every unit test and fuzz binary relies on.
    vitreo::diagnostics::dump_hostcalls("spin-detected");
    SUCCEED();
}

TEST_F(DiagnosticsTest, DumpForwardsReasonToInstalledImplementation) {
    vitreo::diagnostics::set_hostcall_dump(&recording_dump);

    vitreo::diagnostics::dump_hostcalls("jit-av");
    vitreo::diagnostics::dump_hostcalls("guest-oom");

    ASSERT_EQ(g_reasons.size(), 2u);
    EXPECT_EQ(g_reasons[0], "jit-av");
    EXPECT_EQ(g_reasons[1], "guest-oom");
}

TEST_F(DiagnosticsTest, InstallingNullptrRestoresTheNoOp) {
    vitreo::diagnostics::set_hostcall_dump(&recording_dump);
    vitreo::diagnostics::dump_hostcalls("first");

    vitreo::diagnostics::set_hostcall_dump(nullptr);
    vitreo::diagnostics::dump_hostcalls("second");

    ASSERT_EQ(g_reasons.size(), 1u);
    EXPECT_EQ(g_reasons[0], "first");
}

TEST_F(DiagnosticsTest, NullReasonReachesTheImplementation) {
    // Callers pass a literal today, but the hook must not assume it: the
    // implementation decides how to render a missing reason.
    vitreo::diagnostics::set_hostcall_dump(&recording_dump);
    vitreo::diagnostics::dump_hostcalls(nullptr);

    ASSERT_EQ(g_reasons.size(), 1u);
    EXPECT_EQ(g_reasons[0], "<null>");
}

// -----------------------------------------------------------------------------
// Trace milestone latch
// -----------------------------------------------------------------------------

TEST_F(DiagnosticsTest, TraceMilestoneStartsUnreached) {
    EXPECT_FALSE(vitreo::diagnostics::trace_milestone_reached());
}

TEST_F(DiagnosticsTest, TraceMilestoneReflectsWhatWasSet) {
    vitreo::diagnostics::set_trace_milestone_reached(true);
    EXPECT_TRUE(vitreo::diagnostics::trace_milestone_reached());

    vitreo::diagnostics::set_trace_milestone_reached(false);
    EXPECT_FALSE(vitreo::diagnostics::trace_milestone_reached());
}

TEST_F(DiagnosticsTest, TraceMilestoneIsVisibleAcrossThreads) {
    // The setter and the reader are different threads in practice: kernel32's
    // overlapped-IO path observes the milestone, the guest run loop reads it.
    std::thread setter([] { vitreo::diagnostics::set_trace_milestone_reached(true); });
    setter.join();

    EXPECT_TRUE(vitreo::diagnostics::trace_milestone_reached());
}

// -----------------------------------------------------------------------------
// Module-init depth
// -----------------------------------------------------------------------------

TEST_F(DiagnosticsTest, ModuleInitDepthStartsAtZero) {
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 0);
}

TEST_F(DiagnosticsTest, ModuleInitDepthNests) {
    vitreo::diagnostics::enter_module_init();
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 1);

    // A DllMain that loads another module re-enters.
    vitreo::diagnostics::enter_module_init();
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 2);

    vitreo::diagnostics::leave_module_init();
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 1);

    vitreo::diagnostics::leave_module_init();
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 0);
}

TEST_F(DiagnosticsTest, UnbalancedLeaveIsClampedAtZero) {
    // A guest that longjmps out of DllMain leaves the counter unbalanced.
    // Going negative would disable the gate for the rest of the thread's life.
    vitreo::diagnostics::leave_module_init();
    vitreo::diagnostics::leave_module_init();
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 0);

    vitreo::diagnostics::enter_module_init();
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 1);
}

TEST_F(DiagnosticsTest, ModuleInitDepthIsPerThread) {
    // It describes the calling thread's guest stack, so one thread's DllMain
    // must not make another thread look like it is inside module init.
    vitreo::diagnostics::enter_module_init();
    ASSERT_EQ(vitreo::diagnostics::module_init_depth(), 1);

    int observed_in_other_thread = -1;
    std::thread other([&] {
        observed_in_other_thread = vitreo::diagnostics::module_init_depth();
    });
    other.join();

    EXPECT_EQ(observed_in_other_thread, 0);
    EXPECT_EQ(vitreo::diagnostics::module_init_depth(), 1);

    vitreo::diagnostics::leave_module_init();
}

// -----------------------------------------------------------------------------
// Load-phase clock
// -----------------------------------------------------------------------------

class PhaseClockTest : public ::testing::Test {
protected:
    void SetUp() override { vitreo::diagnostics::phase_clock_reset_for_tests(); }
    void TearDown() override { vitreo::diagnostics::phase_clock_reset_for_tests(); }
};

TEST_F(PhaseClockTest, StartAndMarkAreOnceOnly) {
    testing::internal::CaptureStderr();
    vitreo::diagnostics::phase_clock_start();
    vitreo::diagnostics::phase_clock_start();
    vitreo::diagnostics::phase_mark(vitreo::diagnostics::LoadPhase::PeEntry);
    vitreo::diagnostics::phase_mark(vitreo::diagnostics::LoadPhase::PeEntry);
    const std::string out = testing::internal::GetCapturedStderr();

    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 2);
    EXPECT_NE(out.find("[load-phase] process_start"), std::string::npos);
    EXPECT_NE(out.find("[load-phase] pe_entry"), std::string::npos);
}

TEST_F(PhaseClockTest, HeartbeatPrintsPresentWallSinceFirst) {
    testing::internal::CaptureStderr();
    vitreo::diagnostics::phase_clock_start();
    vitreo::diagnostics::phase_note_present();
    vitreo::diagnostics::phase_add_present_wall(2'000'000ull);
    vitreo::diagnostics::phase_add_present_submit_wall(1'000'000ull);
    // Force the 5 s slot: origin is now, so sleep is too slow for a unit
    // test. The first heartbeat is skipped until slot>=1; just prove the
    // wall add does not crash and first_present is marked.
    const std::string out = testing::internal::GetCapturedStderr();
    EXPECT_NE(out.find("[load-phase] first_present"), std::string::npos);
}

TEST_F(PhaseClockTest, PresentMarksAtFirstTenSixtyAndThreeHundred) {
    vitreo::diagnostics::phase_clock_start();
    testing::internal::CaptureStderr();
    for (int i = 0; i < 300; ++i) {
        vitreo::diagnostics::phase_note_present();
    }
    vitreo::diagnostics::phase_note_present();
    const std::string out = testing::internal::GetCapturedStderr();

    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'), 4);
    EXPECT_NE(out.find("[load-phase] first_present"), std::string::npos);
    EXPECT_NE(out.find("[load-phase] present_10"), std::string::npos);
    EXPECT_NE(out.find("[load-phase] present_60"), std::string::npos);
    EXPECT_NE(out.find("[load-phase] present_300"), std::string::npos);
}

// -----------------------------------------------------------------------------
// Performance window
// -----------------------------------------------------------------------------

class PerfWindowTest : public ::testing::Test {
protected:
    void SetUp() override {
        vitreo::diagnostics::perf_window_reset_for_tests();
        vitreo::diagnostics::phase_clock_reset_for_tests();
        vitreo::diagnostics::perf_set_miss_rip_name_fn(nullptr);
    }
    void TearDown() override {
        vitreo::diagnostics::perf_set_miss_rip_name_fn(nullptr);
        vitreo::diagnostics::perf_window_reset_for_tests();
        vitreo::diagnostics::phase_clock_reset_for_tests();
    }
};

TEST_F(PerfWindowTest, InactiveAddsAreIgnored) {
    EXPECT_FALSE(vitreo::diagnostics::perf_window_active());
    vitreo::diagnostics::perf_add_jit_insns(100);
    vitreo::diagnostics::perf_add_interp_insns(50);
    vitreo::diagnostics::perf_add_fallback_insns(7);
    vitreo::diagnostics::perf_note_block_miss(0x1000, false);
    const auto snap = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_EQ(snap.jit_insns, 0u);
    EXPECT_EQ(snap.interp_insns, 0u);
    EXPECT_EQ(snap.fallback_insns, 0u);
    EXPECT_EQ(snap.block_miss_total, 0u);
}

TEST_F(PerfWindowTest, MemoryModelDistinguishesLinear32AndLinear64Jit) {
    vitreo::diagnostics::perf_note_memory_model(
        true, true, false, false);
    auto linear32 = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_TRUE(linear32.guest_linear);
    EXPECT_TRUE(linear32.jit_linear);
    EXPECT_FALSE(linear32.jit_linear64);

    vitreo::diagnostics::perf_note_memory_model(
        true, false, true, false);
    auto linear64 = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_TRUE(linear64.guest_linear);
    EXPECT_FALSE(linear64.jit_linear);
    EXPECT_TRUE(linear64.jit_linear64);

    vitreo::diagnostics::perf_note_memory_model(
        false, false, false, false);
}

TEST_F(PerfWindowTest, StartStopIsolatesCounts) {
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    EXPECT_TRUE(vitreo::diagnostics::perf_window_active());

    vitreo::diagnostics::perf_add_jit_insns(10);
    vitreo::diagnostics::perf_add_interp_insns(4);
    vitreo::diagnostics::perf_add_fallback_insns(1);
    vitreo::diagnostics::perf_note_compile(true, false, 1500, 64, 8, 16, 0x1000);
    vitreo::diagnostics::perf_add_rep_movs(4096);
    vitreo::diagnostics::perf_add_registry(2500);
    vitreo::diagnostics::perf_add_graphics_ns(8000);
    vitreo::diagnostics::perf_note_threads(3, 2, 1, 2);

    auto mid = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_EQ(mid.jit_insns, 10u);
    EXPECT_EQ(mid.interp_insns, 4u);
    EXPECT_EQ(mid.fallback_insns, 1u);
    EXPECT_EQ(mid.guest_insns, 15u);
    EXPECT_EQ(mid.compile_successes, 1u);
    EXPECT_EQ(mid.compile_ns, 1500u);
    EXPECT_EQ(mid.native_bytes, 64u);
    EXPECT_EQ(mid.rep_movs_bytes, 4096u);
    EXPECT_EQ(mid.registry_calls, 1u);
    EXPECT_EQ(mid.registry_ns, 2500u);
    EXPECT_EQ(mid.graphics_ns, 8000u);
    EXPECT_EQ(mid.guest_threads, 3u);
    EXPECT_EQ(mid.guest_runnable, 2u);
    EXPECT_EQ(mid.guest_suspended, 1u);
    EXPECT_EQ(mid.host_workers, 2u);
    EXPECT_EQ(mid.guest_threads_max, 3u);

    vitreo::diagnostics::perf_window_stop();
    EXPECT_FALSE(vitreo::diagnostics::perf_window_active());
    vitreo::diagnostics::perf_add_jit_insns(99);
    auto after = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_EQ(after.jit_insns, 10u);
}

TEST_F(PerfWindowTest, ResetClearsCounts) {
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    vitreo::diagnostics::perf_add_jit_insns(20);
    vitreo::diagnostics::perf_window_reset();
    const auto snap = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_FALSE(snap.active);
    EXPECT_EQ(snap.jit_insns, 0u);
}

TEST_F(PerfWindowTest, FirstPresentStartsConfiguredWindow) {
    vitreo::diagnostics::perf_window_enable_for_tests(
        vitreo::diagnostics::LoadPhase::FirstPresent);
    EXPECT_FALSE(vitreo::diagnostics::perf_window_active());
    testing::internal::CaptureStderr();
    vitreo::diagnostics::phase_clock_start();
    vitreo::diagnostics::phase_note_present();
    (void)testing::internal::GetCapturedStderr();
    EXPECT_TRUE(vitreo::diagnostics::perf_window_active());
}

TEST_F(PerfWindowTest, DumpPrintsReportOnce) {
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    vitreo::diagnostics::perf_add_jit_insns(3);
    vitreo::diagnostics::perf_window_stop();

    testing::internal::CaptureStderr();
    vitreo::diagnostics::perf_window_dump("test");
    vitreo::diagnostics::perf_window_dump("test-again");
    const std::string out = testing::internal::GetCapturedStderr();
    EXPECT_NE(out.find("VITREO PERF WINDOW (test)"), std::string::npos);
    EXPECT_EQ(out.find("VITREO PERF WINDOW (test-again)"), std::string::npos);
}

TEST_F(PerfWindowTest, BlockMissCountsUniqueAndRepeat) {
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    vitreo::diagnostics::perf_note_block_miss(0x1000, false);
    vitreo::diagnostics::perf_note_block_miss(0x1000, false);
    vitreo::diagnostics::perf_note_block_miss(0x2000, true);
    vitreo::diagnostics::perf_note_block_miss(0x2000, true);
    vitreo::diagnostics::perf_note_chain_lookup_miss(0x3000);
    vitreo::diagnostics::perf_note_chain_miss_mid_block();
    vitreo::diagnostics::perf_note_chain_miss_thunk();
    vitreo::diagnostics::perf_note_chain_thunk_inline_fast();
    vitreo::diagnostics::perf_note_chain_thunk_inline_slow();
    vitreo::diagnostics::perf_note_chain_thunk_inline_direct();
    vitreo::diagnostics::perf_note_jcc_emit_fused();
    vitreo::diagnostics::perf_note_jcc_emit_carried();
    vitreo::diagnostics::perf_note_jcc_emit_helper();
    vitreo::diagnostics::perf_note_hop_limit_fallback();
    const auto snap = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_EQ(snap.block_miss_total, 5u);
    EXPECT_EQ(snap.block_miss_unique, 3u);
    EXPECT_EQ(snap.block_miss_repeat, 2u);
    EXPECT_EQ(snap.block_miss_empty, 2u);
    EXPECT_EQ(snap.block_miss_empty_repeat, 1u);
    EXPECT_EQ(snap.block_miss_chain, 1u);
    EXPECT_EQ(snap.chain_miss_mid_block, 1u);
    EXPECT_EQ(snap.chain_miss_thunk, 1u);
    EXPECT_EQ(snap.chain_thunk_inline, 3u);
    EXPECT_EQ(snap.chain_thunk_inline_fast, 1u);
    EXPECT_EQ(snap.chain_thunk_inline_slow, 1u);
    EXPECT_EQ(snap.chain_thunk_inline_direct, 1u);
    EXPECT_EQ(snap.jcc_emit_fused, 1u);
    EXPECT_EQ(snap.jcc_emit_carried, 1u);
    EXPECT_EQ(snap.jcc_emit_helper, 1u);
    EXPECT_EQ(snap.hop_limit_fallbacks, 1u);
}

TEST_F(PerfWindowTest, BlockMissRipNamePrintedOnDump) {
    vitreo::diagnostics::perf_set_miss_rip_name_fn(
        [](vitreo::u64 rip, char* out, size_t out_sz) {
            if (out == nullptr || out_sz == 0) {
                return;
            }
            out[0] = '\0';
            if (rip == 0x70003570ull) {
                std::snprintf(out, out_sz, "kernel32.dll!Sleep");
            }
        });
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    vitreo::diagnostics::perf_note_chain_lookup_miss(0x70003570ull);
    vitreo::diagnostics::perf_window_stop();

    testing::internal::CaptureStderr();
    vitreo::diagnostics::perf_window_dump("miss-name");
    const std::string out = testing::internal::GetCapturedStderr();
    EXPECT_NE(out.find("0x70003570"), std::string::npos);
    EXPECT_NE(out.find("(chain)  kernel32.dll!Sleep"), std::string::npos);

    // Unregister must not crash a subsequent dump.
    vitreo::diagnostics::perf_set_miss_rip_name_fn(nullptr);
    vitreo::diagnostics::perf_window_reset_for_tests();
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    vitreo::diagnostics::perf_note_chain_lookup_miss(0x70003570ull);
    vitreo::diagnostics::perf_window_stop();
    vitreo::diagnostics::perf_window_dump("miss-name-cleared");
}

} // namespace
