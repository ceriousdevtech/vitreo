// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 regions above the reservation (scattered fallback) and the
// high-region slot cache. Split from test_jit_linear.cpp; shared fixtures and
// matchers live in jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Windows' x64 user space is 128 TiB and high system DLLs load just under
// 0x7FFF'FFFEFFFF — far above the 2^46 linear reservation. Capping the guest
// VA at the reservation (measured on Steam) relocated every high DLL to
// 0x3ffa... and wedged CEF, so regions above the window now fall back to the
// scattered model. These prove the fallback end to end: the mapping works, a
// JIT access reaches it with the right value (through the checked helper,
// because the bounds check rejects the address before any direct access), and
// protection and unmap behave like any scattered region.
// Run alone to additionally verify first-fault backpatches in fresh process
// helper tables. The ordinary suite may have already patched the same helpers.
TEST_F(JitLinear64Test, SharedScalarHelperReturnsAcrossCrossingFaults) {
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    constexpr u64 value = 0x1122334455667788ull;
    const u64 patches_before = tso_backpatch_count();
    for (bool store : {false, true}) {
        for (int width : {1, 2, 4, 8}) {
            std::vector<u8> code{0x39, 0xD1}; // cmp ecx,edx: live ZF
            if (width == 2) code.push_back(0x66);
            if (width == 8) code.push_back(0x48);
            code.push_back(static_cast<u8>(store ? (width == 1 ? 0x88 : 0x89)
                                                 : (width == 1 ? 0x8A : 0x8B)));
            code.push_back(0x03); // [rbx], accumulator
            code.insert(code.end(), {0x0F, 0x94, 0xC1}); // sete cl after return
            ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
            auto* block = m_jit->compile(build64(code));
            ASSERT_NE(block, nullptr);
            // Aligned miss populates the region; the crossing access then
            // hits that region and exercises the shared helper's return path.
            for (GuestAddr address : {high, high + 15, high + 31}) {
                ASSERT_TRUE(m_memory->write_u64(address, store ? ~value : value));
                auto cpu = cpu64(address);
                cpu.gpr.rax = store ? value : 0;
                cpu.gpr.rcx = 7;
                cpu.gpr.rdx = 7;
                ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *block, cpu), JitExecResult::Continue);
                ASSERT_FALSE(cpu.pending_raise.active);
                EXPECT_EQ(cpu.gpr.rcx & 0xffu, 1u);
                const u64 mask = width == 8 ? ~u64{0} : (u64{1} << (width * 8)) - 1;
                EXPECT_EQ((store ? m_memory->read_u64(address).value_or(0)
                                 : cpu.gpr.rax) & mask, value & mask);
            }
        }
    }
    // The focused fresh-process invocation must actually traverse all six
    // width-2/4/8 load/store retry paths, rather than testing patched code only.
    if (std::getenv("VITREO_TEST_FRESH_SHARED_HELPERS"))
        EXPECT_EQ(tso_backpatch_count(), patches_before + 6);
}

TEST_F(JitLinear64Test, DISABLED_SharedScalarLrBenchmark) {
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(high, 0x1122334455667788ull));
    constexpr int accesses = 128;
    constexpr int iterations = 1000000;
    for (bool store : {false, true}) {
        for (int width : {4, 8}) {
            std::vector<u8> code;
            for (int i = 0; i < accesses; ++i) {
                if (width == 8) code.push_back(0x48);
                code.push_back(static_cast<u8>(store ? 0x89 : 0x8B));
                code.push_back(0x03);
            }
            ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
            auto body = build64(code);
            ASSERT_EQ(body.instruction_count, accesses);
            auto* block = m_jit->compile(body);
            ASSERT_NE(block, nullptr);
            auto cpu = cpu64(high);
            cpu.gpr.rax = 0x1122334455667788ull;
            for (int i = 0; i < 1000; ++i) {
                cpu.rip = kCode64;
                ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *block, cpu), JitExecResult::Continue);
            }
            const auto begin = std::chrono::steady_clock::now();
            bool valid = true;
            for (int i = 0; i < iterations; ++i) {
                cpu.rip = kCode64;
                valid &= test::execute_like_dispatcher(*m_jit, *block, cpu) == JitExecResult::Continue;
            }
            const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - begin).count();
            EXPECT_TRUE(valid);
            EXPECT_FALSE(cpu.pending_raise.active);
            std::printf("SCALAR_LR_BENCH op=%s width=%d accesses=%d seconds=%.9f\n",
                        store ? "store" : "load", width, accesses * iterations, seconds);
        }
    }
}

TEST_F(JitLinear64Test, DISABLED_Cmp8BranchBenchmark) {
    const std::vector<u8> code{0x38, 0x0b, 0x75, 0x02}; // cmp [rbx],cl; jne
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    ASSERT_TRUE(m_memory->write_u8(kData64, 0x7f));
    auto body = build64(code);
    auto* block = m_jit->compile(body);
    ASSERT_NE(block, nullptr);
    for (u64 rhs : {0x7full, 0x80ull}) {
        auto cpu = cpu64(kData64);
        cpu.gpr.rcx = rhs;
        for (int i = 0; i < 1000; ++i) {
            cpu.rip = kCode64;
            (void)test::execute_like_dispatcher(*m_jit, *block, cpu);
        }
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 10000000; ++i) {
            cpu.rip = kCode64;
            (void)test::execute_like_dispatcher(*m_jit, *block, cpu);
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        EXPECT_EQ(cpu.rip, kCode64 + (rhs == 0x7f ? 4 : 6));
        std::printf("CMP8_BRANCH_BENCH equal=%d iterations=10000000 seconds=%.9f\n",
                    rhs == 0x7f, seconds);
    }
}

TEST_F(JitLinear64Test, HighRegionRepeatedLoadsUsePublishedCache) {
    constexpr u8 cx = static_cast<u8>(RegIndex::RCX);
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(high, 0x1122334455667788ull));
    const GuestAddr mixed = kData64 + 0x1000;
    ASSERT_TRUE(m_memory->write_u64(mixed, 0x1122334455667788ull));
    ASSERT_TRUE(m_memory->protect_region(kData64, 0x1000, MemoryProtection::None));
    // CMP; MOV; SETE: the cold cache hit must preserve live guest flags.
    const std::vector<u8> code{0x39, 0xD1, 0x48, 0x8B, 0x03, 0x0F, 0x94, 0xC1};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    auto* block = m_jit->compile(build64(code));
    ASSERT_NE(block, nullptr);
    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_start();
    for (GuestAddr address : {high, mixed}) {
        for (int i = 0; i < 1000; ++i) {
            auto cpu = cpu64(address);
            cpu.gpr.set32(cx, i & 1);
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *block, cpu), JitExecResult::Continue);
            EXPECT_EQ(cpu.gpr.get64(EAX), 0x1122334455667788ull);
            EXPECT_EQ(cpu.gpr.get32(cx) & 0xFFu, (i & 1) == 0 ? 1u : 0u);
        }
    }
    const auto snapshot = diagnostics::perf_window_snapshot();
    diagnostics::perf_window_stop();
    diagnostics::perf_window_reset_for_tests();
    EXPECT_LE(snapshot.helper_reads, 2u);
    ASSERT_TRUE(m_memory->protect_region(high, 0x4000, MemoryProtection::None));
    auto denied = cpu64(high);
    (void)test::execute_like_dispatcher(*m_jit, *block, denied);
    EXPECT_TRUE(denied.pending_raise.active);
    ASSERT_TRUE(m_memory->unmap_region(high, 0x4000));
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(high, 0x8877665544332211ull));
    auto remapped = cpu64(high);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *block, remapped), JitExecResult::Continue);
    EXPECT_FALSE(remapped.pending_raise.active);
    EXPECT_EQ(remapped.gpr.get64(EAX), 0x8877665544332211ull);
}

TEST_F(JitLinear64Test, HighRegionRepeatedStoresUsePublishedCache) {
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    const std::vector<u8> code{0x48, 0x89, 0x03}; // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    auto* block = m_jit->compile(build64(code));
    ASSERT_NE(block, nullptr);
    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_start();
    for (u64 i = 0; i < 1000; ++i) {
        auto cpu = cpu64(high);
        cpu.gpr.set64(EAX, i);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *block, cpu), JitExecResult::Continue);
        EXPECT_EQ(m_memory->read_u64(high).value_or(~0ull), i);
    }
    const auto snapshot = diagnostics::perf_window_snapshot();
    diagnostics::perf_window_stop();
    diagnostics::perf_window_reset_for_tests();
    EXPECT_LE(snapshot.helper_writes, 1u);
    std::array<u8, 1> dirty{};
    std::array<u8, 1> armed{};
    write_watch::register_region(high, 0x4000, dirty.data(), armed.data(), false);
    auto watched = cpu64(high + 0x1000);
    watched.gpr.set64(EAX, 0xABCDEFu);
    const auto watched_result = test::execute_like_dispatcher(*m_jit, *block, watched);
    const bool recorded = write_watch::test_bit(dirty.data(), 1);
    write_watch::unregister_region(high);
    EXPECT_EQ(watched_result, JitExecResult::Continue);
    EXPECT_TRUE(recorded) << "cached store bypassed newly enabled write watch";

    const GuestAddr victim_address = high + 0x2000;
    const std::vector<u8> victim_code{0xB8, 1, 0, 0, 0};
    ASSERT_TRUE(m_memory->write_bytes(victim_address, victim_code.data(), victim_code.size()));
    auto victim = build64(victim_code);
    victim.start_addr = victim_address;
    ASSERT_NE(m_jit->compile(victim), nullptr);
    m_cache->insert(victim);
    ASSERT_EQ(m_cache->size(), 1u);
    auto smc = cpu64(victim_address);
    smc.gpr.set64(EAX, 0x9090909090909090ull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *block, smc), JitExecResult::Continue);
    EXPECT_EQ(m_cache->size(), 0u) << "cached store bypassed code invalidation";
    ASSERT_TRUE(m_memory->protect_region(high, 0x4000, MemoryProtection::Read));
    auto denied = cpu64(high);
    (void)test::execute_like_dispatcher(*m_jit, *block, denied);
    EXPECT_TRUE(denied.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(high).value_or(~0ull), 999u);
}

// A high-region slot is trusted only while the thread's protect_sequence_seen
// matches the live sequence, and only execute() used to advance it. So after
// any map/commit/protect ANYWHERE in the process, every scattered access of a
// running chain missed its slot and took the C++ helper until the chain went
// back to the dispatcher -- in an x64 game, every read of a DLL loaded above
// the window, while its allocator commits and decommits on other threads. The
// helper now drains the invalidations itself, so each one costs one helper
// call and the next access hits the republished slot again.
TEST_F(JitLinear64Test, HighRegionSlotRecoversFromProtectChangesInsideAChain) {
    constexpr GuestAddr high = 0x00007FFA10000000ull;
    ASSERT_GT(high, kReservation64);
    ASSERT_TRUE(m_memory->map_region(high, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(high, 0x1122334455667788ull));
    const GuestAddr unrelated = kData64 + 0x2000;
    // top: mov rax,[rbx] ; dec rcx ; jnz top -- a loop the block runs natively.
    const std::vector<u8> code{0x48, 0x8B, 0x03, 0x48, 0xFF, 0xC9,
                               0x0F, 0x85, 0xF4, 0xFF, 0xFF, 0xFF};
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* block = m_jit->compile(build64(code));
    ASSERT_NE(block, nullptr);

    constexpr int kProtectChanges = 20;
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::atomic<int> changes_made{0};
    std::thread changer([&] {
        while (!started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kProtectChanges &&
                        !done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            (void)m_memory->protect_region(
                unrelated, 0x1000,
                (i & 1) ? MemoryProtection::ReadWrite : MemoryProtection::Read);
            changes_made.fetch_add(1, std::memory_order_release);
        }
    });

    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_start();
    auto cpu = cpu64(high);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), 100'000'000ull);
    cpu.jit_iters = 0xFFFFFFFFu;  // the in-block loop budget
    started.store(true, std::memory_order_release);
    // The loop leaves on its hop budget with RIP at the JNZ; play the
    // dispatcher's part (take the branch while RCX is non-zero) and re-enter.
    // Each entry drains the invalidations too, which bounds the stale window
    // without the fix -- the assertion allows one helper read per entry.
    const GuestAddr jnz = kCode64 + 6;
    const GuestAddr fallthrough = kCode64 + code.size();
    u64 entries = 0;
    while (cpu.rip != fallthrough && entries < 100'000'000ull) {
        if (cpu.rip == jnz) {
            cpu.rip = cpu.gpr.rcx != 0 ? kCode64 : fallthrough;
            continue;
        }
        ASSERT_EQ(cpu.rip, kCode64);
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *block, cpu), JitExecResult::Continue);
        ASSERT_FALSE(cpu.pending_raise.active);
        cpu.jit_iters = 0xFFFFFFFFu;
        ++entries;
    }
    done.store(true, std::memory_order_release);
    changer.join();
    const auto snapshot = diagnostics::perf_window_snapshot();
    diagnostics::perf_window_stop();
    diagnostics::perf_window_reset_for_tests();

    EXPECT_EQ(cpu.rip, kCode64 + code.size());
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.get64(EAX), 0x1122334455667788ull);
    ASSERT_GT(changes_made.load(), 0)
        << "the loop finished before any protection change: not a test";
    // At most one helper read per invalidation (plus the first publish, and
    // one per dispatcher entry), not one per iteration after the first.
    EXPECT_LE(snapshot.helper_reads,
              static_cast<u64>(changes_made.load()) + entries + 2u)
        << "entries=" << entries << " changes=" << changes_made.load();
}

TEST_F(JitLinear64Test, RegionAboveReservationFallsBackToScattered) {
    constexpr GuestAddr kHigh = 0x00007FFA00000000ull;  // real high-DLL shape
    ASSERT_GT(kHigh, kReservation64);
    ASSERT_TRUE(m_memory->map_region(kHigh, 0x4000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kHigh + 0x40,
                                    0x1122334455667788ull).is_ok());
    EXPECT_EQ(m_memory->read_u64(kHigh + 0x40).value_or(0),
              0x1122334455667788ull);

    // JIT load from the high region: the linear64 bounds check must reject
    // the EA (it is outside the power-of-two window) and the cold helper must
    // complete the access against the scattered region.
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax,[rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
    CompiledBlock* compiled = m_jit->compile(build64(code));
    ASSERT_NE(compiled, nullptr);
    CpuState jcpu = cpu64(kHigh + 0x40);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
    EXPECT_FALSE(jcpu.pending_raise.active);
    EXPECT_EQ(jcpu.gpr.get64(EAX), 0x1122334455667788ull);

    // A JIT store the same way, verified by a host read.
    const std::vector<u8> store = {0x48, 0x89, 0x03};  // mov [rbx],rax
    ASSERT_TRUE(m_memory->write_bytes(kCode64, store.data(), store.size()));
    CompiledBlock* scb = m_jit->compile(build64(store));
    ASSERT_NE(scb, nullptr);
    CpuState scpu = cpu64(kHigh + 0x80);
    scpu.gpr.set64(EAX, 0xA5A5A5A5A5A5A5A5ull);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *scb, scpu), JitExecResult::Continue);
    EXPECT_FALSE(scpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kHigh + 0x80).value_or(0),
              0xA5A5A5A5A5A5A5A5ull);

    // Scattered-region protection is real host protection, not the linear
    // keep-RW-and-record model: NOACCESS must actually deny the guest.
    ASSERT_TRUE(m_memory->protect_region(kHigh, 0x1000,
                                         MemoryProtection::None));
    EXPECT_FALSE(m_memory->read_u64(kHigh + 0x40).is_ok());
    ASSERT_TRUE(m_memory->protect_region(kHigh, 0x1000,
                                         MemoryProtection::ReadWrite));

    ASSERT_TRUE(m_memory->unmap_region(kHigh, 0x4000));
    EXPECT_FALSE(m_memory->read_u64(kHigh + 0x40).is_ok());
}

TEST_F(JitLinear64Test, HighRegionDoesNotDisturbWindowBookkeeping) {
    // Mapping, protecting and unmapping a high region must leave the
    // reservation's check-page map alone: reconcile clamps to the window, so
    // a high address can never index (or worse, mprotect) through it.
    constexpr GuestAddr kHigh = 0x00007FFB00000000ull;
    const u8* check = m_memory->linear_check_page_map();
    ASSERT_NE(check, nullptr);
    std::vector<u8> before(check, check + (kReservation64 >> 14));
    ASSERT_TRUE(m_memory->map_region(kHigh, 0x2000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->protect_region(kHigh, 0x1000,
                                         MemoryProtection::Read));
    ASSERT_TRUE(m_memory->unmap_region(kHigh, 0x2000));
    EXPECT_EQ(std::memcmp(before.data(), check, before.size()), 0)
        << "a high region's lifecycle touched the reservation's check map";
}

}  // namespace
