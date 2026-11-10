// SPDX-License-Identifier: LGPL-2.1-only
// WS2: bulk primitives, accessible-prefix rule, and the helper called directly.

#include "test_jit_loop_idiom_fixture.hpp"

#include "core/cpu/jit/loop_idiom_runtime.hpp"
#include "core/cpu/jit/loop_idiom_thresholds.hpp"

#include "core/cpu/jit/loop_idiom_mem.hpp"
#include "core/cpu/jit/loop_idiom_runtime.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::loop_idiom;
using namespace vitreo::cpu::jit::loop_idiom::test;
using namespace vitreo::cpu::jit::loop_idiom::test::catalogue;

TEST(LoopIdiomMem, HazardRuleMatchesSequentialLoop) {
    for (u32 w : {1u, 2u, 4u, 8u}) {
        for (bool down : {false, true}) {
            for (int off = -20; off <= 20; ++off) {
                for (u64 iters : {1u, 2u, 3u, 7u}) {
                    std::array<u8, 256> ref{}, got{};
                    for (usize i = 0; i < ref.size(); ++i) ref[i] = got[i] = static_cast<u8>(i * 13 + 1);
                    const i64 base = 128;
                    const u64 src1 = static_cast<u64>(base);
                    const u64 dst1 = static_cast<u64>(base + off);
                    const i64 st = down ? -static_cast<i64>(w) : static_cast<i64>(w);
                    for (u64 k = 0; k < iters; ++k) {
                        u8 tmp[8];
                        std::memcpy(tmp, ref.data() + src1 + k * st, w);
                        std::memcpy(ref.data() + dst1 + k * st, tmp, w);
                    }
                    if (!copy_hazard_free(dst1, src1, down, w, iters)) continue;
                    for (bool ordered : {false, true}) {
                        auto buf = got;
                        if (ordered && ((dst1 | src1) % w) != 0) continue;
                        bulk_copy(buf.data() + dst1, buf.data() + src1, down, w, iters, ordered);
                        ASSERT_EQ(0, std::memcmp(buf.data(), ref.data(), ref.size()))
                            << "w=" << w << " down=" << down << " off=" << off
                            << " iters=" << iters << " ordered=" << ordered;
                    }
                }
            }
        }
    }
    // The rule refuses the forward-overlap case that replicates bytes.
    EXPECT_FALSE(copy_hazard_free(0x101, 0x100, false, 1, 4));
    EXPECT_TRUE(copy_hazard_free(0x100, 0x101, false, 1, 4));
    EXPECT_FALSE(copy_hazard_free(0x100, 0x101, true, 1, 4));
    EXPECT_TRUE(copy_hazard_free(0x200, 0x100, false, 1, 4));
}

TEST(LoopIdiomMem, OrderedAndPlainAgree) {
    alignas(64) u8 a[256], b[256];
    const u8 value[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                          0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x01};
    for (u32 w : {1u, 2u, 4u, 8u, 16u}) {
        for (bool down : {false, true}) {
            for (u32 head = 0; head < 8; head += 1) {
                if (head % std::min(w, 8u) != 0) continue;
                for (u64 n = 0; n <= 9; ++n) {
                    const usize start = down ? 160 + head : 16 + head;
                    std::memset(a, 0x5A, sizeof a);
                    std::memset(b, 0x5A, sizeof b);
                    bulk_fill(a + start, down, w, n, value, false);
                    bulk_fill(b + start, down, w, n, value, true);
                    ASSERT_EQ(0, std::memcmp(a, b, sizeof a)) << "fill w=" << w << " n=" << n;
                    // Scan / compare: plant a terminator at every element.
                    for (u64 hit = 0; hit <= n; ++hit) {
                        u8 buf[256];
                        for (usize i = 0; i < sizeof buf; ++i) buf[i] = static_cast<u8>(0x30 + i % 7);
                        const i64 st = down ? -static_cast<i64>(w) : static_cast<i64>(w);
                        if (hit < n) std::memset(buf + start + hit * st, 0, w);
                        ElemPred p;
                        p.op = DataOp::ValueEq;
                        p.exit_if_eq = true;
                        p.cmp_bytes = std::min(w, 8u);
                        const u64 s0 = bulk_scan(buf + start, down, w, n, p, false);
                        const u64 s1 = bulk_scan(buf + start, down, w, n, p, true);
                        ASSERT_EQ(s0, hit) << "scan w=" << w;
                        ASSERT_EQ(s1, hit) << "ordered scan w=" << w;
                        u8 buf2[256];
                        std::memcpy(buf2, buf, sizeof buf);
                        if (hit < n) buf2[start + hit * st] = 0x77;
                        const u64 c0 = bulk_compare(buf + start, buf2 + start, down, w, n,
                                                    false, nullptr, 0, false);
                        const u64 c1 = bulk_compare(buf + start, buf2 + start, down, w, n,
                                                    false, nullptr, 0, true);
                        ASSERT_EQ(c0, hit);
                        ASSERT_EQ(c1, hit);
                    }
                }
            }
        }
    }
}

TEST(LoopIdiomMem, CopyUntilStopsTwoBeforeTheTerminator) {
    u8 src[64], dst[64];
    for (u32 i = 0; i < 64; ++i) src[i] = static_cast<u8>(i + 1);
    src[10] = 0;
    std::memset(dst, 0xCC, sizeof dst);
    ElemPred term;
    term.cmp_bytes = 1;
    term.exit_if_eq = true;
    const u64 m = bulk_copy_until(dst, src, false, 1, 60, term);
    EXPECT_EQ(m, 9u);  // terminator in iteration 11: bulk 1..9, body 10..11
    for (u32 i = 0; i < 9; ++i) EXPECT_EQ(dst[i], src[i]);
    EXPECT_EQ(dst[9], 0xCC);
    // No terminator: exactly max_stored elements.
    src[10] = 5;
    std::memset(dst, 0xCC, sizeof dst);
    EXPECT_EQ(bulk_copy_until(dst, src, false, 1, 20, term), 20u);
    EXPECT_EQ(dst[19], src[19]);
    EXPECT_EQ(dst[20], 0xCC);
}

class LoopIdiomSpan : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(mem.map_region(0x20000, 0x2000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.map_region(0x22000, 0x1000, MemoryProtection::Read));
        ASSERT_TRUE(mem.map_region(0x23000, 0x1000, MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(mem.map_region(0x24000, 0x1000, MemoryProtection::ReadWrite));
    }
    GuestMemory mem;
};

TEST_F(LoopIdiomSpan, StopsAtProtectionAndMapping) {
    // RW up to 0x22000, then read-only.
    EXPECT_EQ(safe_span(mem, 0x21FF0, false, 1, 4095, true, false).elems, 16u);
    EXPECT_EQ(safe_span(mem, 0x21FF0, false, 4, 4095, true, false).elems, 4u);
    // Loads continue through the read-only page while it is one region.
    EXPECT_EQ(safe_span(mem, 0x22000, false, 1, 4095, false, false).elems, 4095u);
    // A store into read-only or executable memory gets nothing.
    EXPECT_EQ(safe_span(mem, 0x22000, false, 1, 4095, true, false).elems, 0u);
    EXPECT_EQ(safe_span(mem, 0x23000, false, 1, 16, true, false).elems, 0u);
    // Unmapped.
    EXPECT_EQ(safe_span(mem, 0x30000, false, 1, 16, false, false).elems, 0u);
    // Backward: from 0x20010 down to the region base.
    EXPECT_EQ(safe_span(mem, 0x20010, true, 1, 4095, true, false).elems, 17u);
    // An element straddling the end is not counted.
    EXPECT_EQ(safe_span(mem, 0x24FFE, false, 4, 16, true, false).elems, 0u);
}

TEST_F(LoopIdiomSpan, StopsAtCompiledCodeAndWraps) {
    mem.retain_jit_code_page(0x20000);
    EXPECT_EQ(safe_span(mem, 0x20000, false, 1, 4095, true, false).elems, 0u)
        << "a store window touching a page with compiled code must decline";
    EXPECT_EQ(safe_span(mem, 0x20000, false, 1, 4095, false, false).elems, 4095u)
        << "loads are unaffected";
    mem.release_jit_code_page(0x20000);
    EXPECT_EQ(safe_span(mem, 0x20000, false, 1, 4095, true, false).elems, 4095u);
    // 32-bit: never wraps past 2^32.
    EXPECT_EQ(safe_span(mem, 0xFFFFFFF0ull, false, 1, 4095, false, false).elems, 0u);
}

// ---------------------------------------------------------------------------
// The helper called directly.

namespace {

Words words_of(std::span<const u8> code, bool is64, MatchConfig cfg = {true, false}) {
    const auto r = match_bytes(code, is64, cfg);
    EXPECT_TRUE(r.match) << reject_reason_name(r.reason);
    return *pack(r.match->desc);
}

u32 call(CpuState& cpu, GuestMemory& mem, const Words& w) {
    return jit_loop_idiom(&cpu, &mem, w[0], w[1], w[2], w[3], w[4], w[5]);
}

}  // namespace

TEST(LoopIdiomRuntime, FillCommitsInductionsOnly) {
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
    std::vector<u8> init(0x200, 0xA5);
    ASSERT_TRUE(mem.write_bytes(0x20000, init.data(), init.size()));
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = 0x1000;
    cpu.gpr.set32(0, 0x3C);       // AL
    cpu.gpr.set32(7, 0x20000);    // EDI
    cpu.gpr.set32(1, 100);        // ECX
    cpu.set_flag_op(CpuState::FlagOp::Sub32);
    cpu.flag_src = 7;
    cpu.flag_dst = 9;
    const auto op = cpu.flag_op;
    ScopedForce f(Force::OnNoThreshold);
    // The exit (ECX reaching 0 in iteration 100) follows the bulk directly.
    ASSERT_EQ(call(cpu, mem, words_of(kF1, false)), kRanTail);
    EXPECT_EQ(cpu.gpr.get32(7), 0x20000u + 98);
    EXPECT_EQ(cpu.gpr.get32(1), 2u);
    EXPECT_EQ(cpu.gpr.get32(0), 0x3Cu);
    EXPECT_EQ(cpu.jit_iters, 98u);
    EXPECT_EQ(cpu.rip, 0x1000u);
    EXPECT_EQ(cpu.flag_op, op);
    EXPECT_EQ(cpu.flag_src, 7u);
    EXPECT_EQ(cpu.flag_dst, 9u);
    std::vector<u8> got(0x200);
    ASSERT_TRUE(mem.read_bytes(0x20000, got.data(), got.size()));
    for (u32 i = 0; i < 98; ++i) ASSERT_EQ(got[i], 0x3C) << i;
    EXPECT_EQ(got[98], 0xA5);
}

TEST(LoopIdiomRuntime, CountZeroIsTwoToThe32) {
    // LOOP/DEC+JNZ with ECX = 0 runs 2^32 iterations (Intel SDM Vol. 2
    // LOOP; DEC wraps to 0xFFFFFFFF): one bulk window of the per-call budget
    // less one, ECX wraps, and the block must re-dispatch for more.
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x20000, 0x20000, MemoryProtection::ReadWrite));
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = 0x1000;
    cpu.gpr.set32(7, 0x20000);
    cpu.gpr.set32(1, 0);
    ScopedForce f(Force::OnNoThreshold);
    const u32 m = static_cast<u32>(max_iters_per_call(1) - 1);
    ASSERT_EQ(call(cpu, mem, words_of(kF1, false)), kRanRedispatch);
    EXPECT_EQ(cpu.jit_iters, m);
    EXPECT_EQ(cpu.gpr.get32(1), 0u - m);
    EXPECT_EQ(cpu.gpr.get32(7), 0x20000u + m);
}

TEST(LoopIdiomRuntime, BudgetCapsEveryKindPerCall) {
    // Never below the REP chunk of 4095 iterations; a byte element moves
    // 64 KiB.
    EXPECT_EQ(max_iters_per_call(1), kMaxBulkBytesPerCall);
    EXPECT_EQ(max_iters_per_call(8), kMaxBulkBytesPerCall / 8);
    EXPECT_EQ(max_iters_per_call(16), kMaxBulkBytesPerCall / 16);  // 4096 >= 4095
    EXPECT_EQ(max_iters_per_call(32), kMinItersPerCall);
}

TEST(LoopIdiomRuntime, DeclineTouchesNothing) {
    ScopedForce f(Force::OnNoThreshold);
    GuestMemory mem;
    ASSERT_TRUE(mem.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
    std::vector<u8> init(0x10000);
    for (usize i = 0; i < init.size(); ++i) init[i] = static_cast<u8>(i * 3 + 1);
    ASSERT_TRUE(mem.write_bytes(0x20000, init.data(), init.size()));
    auto check = [&](const char* what, std::span<const u8> code, bool is64,
                     const std::function<void(CpuState&)>& setup) {
        SCOPED_TRACE(what);
        CpuState cpu;
        if (is64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.rip = 0x1000;
        setup(cpu);
        const CpuState before = cpu;
        EXPECT_EQ(call(cpu, mem, words_of(code, is64)), 0u);
        EXPECT_EQ(0, std::memcmp(&before.gpr, &cpu.gpr, sizeof cpu.gpr));
        EXPECT_EQ(before.rip, cpu.rip);
        EXPECT_EQ(before.jit_iters, cpu.jit_iters);
        std::vector<u8> got(init.size());
        ASSERT_TRUE(mem.read_bytes(0x20000, got.data(), got.size()));
        EXPECT_EQ(got, init);
    };
    check("unmapped first element", kF1, false, [](CpuState& c) {
        c.gpr.set32(7, 0x50000);
        c.gpr.set32(1, 100);
    });
    check("copy dst = src + 1", kC1, false, [](CpuState& c) {
        c.gpr.set32(6, 0x20000);
        c.gpr.set32(7, 0x20001);
        c.gpr.set32(1, 100);
    });
    check("count 2: nothing to bulk", kF1, false, [](CpuState& c) {
        c.gpr.set32(7, 0x20000);
        c.gpr.set32(1, 2);
    });
    check("movdqa misaligned", kC10, true, [](CpuState& c) {
        c.gpr.set64(6, 0x20008);
        c.gpr.set64(7, 0x24000);
        c.gpr.set64(0, 0);
        c.gpr.set64(2, 0x800);
    });
    check("terminator in iteration 2", kC5, false, [](CpuState& c) {
        // init[i] = (3i + 1) mod 256 is zero at i = 85: element 2 from 84.
        c.gpr.set32(6, 0x20000 + 84);
        c.gpr.set32(7, 0x28000);
    });
}
