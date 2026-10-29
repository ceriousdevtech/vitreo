// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The performance window's helper-memory counters.
//
// `perf_add_helper_mem()` is on the hot path of every JIT helper memory
// access, so its disabled form was made an inlinable one-load gate
// (`perf_add_helper_mem_fast()` in core/base/perf_window.hpp). Making a
// measurement free when it is off is only a win if it still measures when it
// is on, and nothing else in the suite drives these counters through the real
// `jit_mem_*` helpers. This test pins both halves: exact counts while the
// window is collecting, and no counting at all once it stops.

#include <gtest/gtest.h>

#include "core/base/perf_window.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_helpers.hpp"

#include <vitreo/diagnostics.hpp>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr GuestAddr kBase = 0x20000;

class PerfWindowHelperMemTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kBase, 0x1000,
                                         MemoryProtection::ReadWrite));
        m_cpu = std::make_unique<CpuState>();
        m_ctx = std::make_unique<JitContext>();
        m_ctx->memory = m_memory.get();
        m_ctx->cpu = m_cpu.get();
    }

    void TearDown() override {
        diagnostics::perf_window_stop();
        diagnostics::perf_window_reset_for_tests();
    }

    /// One access of every helper width, both directions. 8 reads, 8 writes.
    void drive_helpers() {
        jit_mem_write8(m_ctx.get(), kBase + 0x00, 0x11);
        jit_mem_write16(m_ctx.get(), kBase + 0x08, 0x2222);
        jit_mem_write32(m_ctx.get(), kBase + 0x10, 0x33333333u);
        jit_mem_write64(m_ctx.get(), kBase + 0x18, 0x4444444444444444ull);
        jit_mem_write8(m_ctx.get(), kBase + 0x20, 0x55);
        jit_mem_write16(m_ctx.get(), kBase + 0x28, 0x6666);
        jit_mem_write32(m_ctx.get(), kBase + 0x30, 0x77777777u);
        jit_mem_write64(m_ctx.get(), kBase + 0x38, 0x8888888888888888ull);

        EXPECT_EQ(jit_mem_read8(m_ctx.get(), kBase + 0x00), 0x11u);
        EXPECT_EQ(jit_mem_read16(m_ctx.get(), kBase + 0x08), 0x2222u);
        EXPECT_EQ(jit_mem_read32(m_ctx.get(), kBase + 0x10), 0x33333333u);
        EXPECT_EQ(jit_mem_read64(m_ctx.get(), kBase + 0x18),
                  0x4444444444444444ull);
        EXPECT_EQ(jit_mem_read8(m_ctx.get(), kBase + 0x20), 0x55u);
        EXPECT_EQ(jit_mem_read16(m_ctx.get(), kBase + 0x28), 0x6666u);
        EXPECT_EQ(jit_mem_read32(m_ctx.get(), kBase + 0x30), 0x77777777u);
        EXPECT_EQ(jit_mem_read64(m_ctx.get(), kBase + 0x38),
                  0x8888888888888888ull);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
    std::unique_ptr<JitContext> m_ctx;
};

TEST_F(PerfWindowHelperMemTest, ActiveWindowCountsEveryHelperAccess) {
    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_start();
    ASSERT_TRUE(diagnostics::perf_window_active());
    ASSERT_TRUE(diagnostics::perf_window_active_fast());

    drive_helpers();
    // The fault leg of the same gate; raising a real access violation needs a
    // dispatcher this fixture does not have.
    diagnostics::perf_add_helper_mem_fast(/*is_write=*/true, /*fault=*/true);

    const auto snap = diagnostics::perf_window_snapshot();
    EXPECT_EQ(snap.helper_reads, 8u);
    EXPECT_EQ(snap.helper_writes, 9u);
    EXPECT_EQ(snap.mem_faults, 1u);
}

TEST_F(PerfWindowHelperMemTest, StoppedWindowCountsNothing) {
    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_start();
    drive_helpers();
    diagnostics::perf_window_stop();
    ASSERT_FALSE(diagnostics::perf_window_active_fast());

    const auto during = diagnostics::perf_window_snapshot();
    ASSERT_EQ(during.helper_reads, 8u);

    drive_helpers();
    const auto after = diagnostics::perf_window_snapshot();
    EXPECT_EQ(after.helper_reads, during.helper_reads);
    EXPECT_EQ(after.helper_writes, during.helper_writes);
    EXPECT_EQ(after.mem_faults, during.mem_faults);
}

// The inlinable gate and the out-of-line entry point must agree; the hot
// callers use the former and every other translation unit still uses the
// latter.
TEST_F(PerfWindowHelperMemTest, InlineAndOutOfLineGatesAgree) {
    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_stop();
    EXPECT_EQ(diagnostics::perf_window_active(),
              diagnostics::perf_window_active_fast());
    diagnostics::perf_window_start();
    EXPECT_EQ(diagnostics::perf_window_active(),
              diagnostics::perf_window_active_fast());

    // Both entry points feed the same counters.
    const u64 before = diagnostics::perf_window_snapshot().helper_reads;
    diagnostics::perf_add_helper_mem(/*is_write=*/false, /*fault=*/false);
    diagnostics::perf_add_helper_mem_fast(/*is_write=*/false, /*fault=*/false);
    EXPECT_EQ(diagnostics::perf_window_snapshot().helper_reads, before + 2u);
}

}  // namespace
