// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// IR tier-up (jit_compiler_cold_residency.hpp, jit_ir_tier.cpp): a block the
// Tier-1 gate admits is compiled with stencils and a tier counter first, and
// recompiled with the IR once it has been entered the threshold number of
// times. Every expected guest result is the interpreter's for the same bytes
// and inputs (the interpreter is the CPU semantic reference).

#include "jit_linear_fixture.hpp"

#include <cstdlib>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

constexpr u32 kBlrX30 = 0xD63F03C0u;

struct TierThreshold {
    explicit TierThreshold(int threshold) {
        JitCompiler::set_ir_tier_threshold_for_test(threshold);
        JitCompiler::reset_ir_tier_for_test();
    }
    ~TierThreshold() {
        JitCompiler::reset_ir_tier_for_test();
        JitCompiler::set_ir_tier_threshold_for_test(-1);
    }
};

struct ResidencyOn {
    explicit ResidencyOn(JitCompiler& jit) : m_jit(jit) {
        JitCompiler::set_cold_residency_for_test(1);
        m_jit.reset_cold_residency_for_test();
    }
    ~ResidencyOn() {
        m_jit.reset_cold_residency_for_test();
        JitCompiler::set_cold_residency_for_test(-1);
    }
    JitCompiler& m_jit;
};

/// The gate's policy for a block of this bitness (jit_ir_gate.cpp): unset
/// VITREO_JIT_IR admits every 64-bit and every linear 32-bit block.
bool gate_open(bool is_64bit) {
    const char* e = std::getenv("VITREO_JIT_IR");
    return e == nullptr || e[0] == '\0' || JitCompiler::ir_enabled_for(is_64bit);
}

std::vector<u32> sites_of(const CompiledBlock& cb, u32 site_word) {
    std::vector<u32> out;
    for (const WordRange& r : block_word_ranges(cb)) {
        for (const u32* w = r.begin; w + 2 < r.end; ++w) {
            if (w[0] == site_word && w[1] == kBlrX30) out.push_back(w[2]);
        }
    }
    return out;
}

std::vector<u32> tier_sites(const CompiledBlock& cb) {
    return sites_of(cb, JitCompiler::tier_counter_site_word());
}

void expect_same_state(CpuState& jit, CpuState& ref, const char* what) {
    for (u8 r = 0; r < 16; ++r) {
        EXPECT_EQ(jit.gpr.get64(r), ref.gpr.get64(r)) << what << ": GPR " << unsigned(r);
    }
    EXPECT_EQ(jit.rip, ref.rip) << what << ": RIP";
    jit.materialize_lazy_flags();
    ref.materialize_lazy_flags();
    EXPECT_EQ(jit.flags.rflags, ref.flags.rflags) << what << ": RFLAGS";
}

}  // namespace

class JitIrTierTest : public JitLinear64Test {
protected:
    BasicBlock* publish(const std::vector<u8>& code, CompiledBlock& out) {
        EXPECT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
        BasicBlock* cached = m_cache->insert(build64(code));
        CompiledBlock* cb = m_jit->compile(*cached);
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) return nullptr;
        out = *cb;
        m_jit->publish_compiled_block(*cached, *cb);
        return cached;
    }

    CompiledBlock recompile(const std::vector<u8>& code) {
        BasicBlock fresh = build64(code);
        CompiledBlock* cb = m_jit->compile(fresh);
        EXPECT_NE(cb, nullptr);
        return cb ? *cb : CompiledBlock{};
    }

    CpuState seeded(u64 k) {
        CpuState cpu = cpu64(0);
        cpu.gpr.set64(0, 0x0123456789ABCDEFull ^ (k * 0x9E3779B97F4A7C15ull));
        cpu.gpr.set64(1, 0xFEDCBA9876543210ull + k);
        cpu.gpr.set64(2, 0x8000000000000000ull >> (k & 63));
        cpu.gpr.set64(3, k * 0x10001ull);
        return cpu;
    }

    /// Run `cb` from `in` and compare with the interpreter stepping `steps`
    /// instructions (or until RIP reaches `stop`) from the same state.
    void run_and_compare(CompiledBlock& cb, const CpuState& in, GuestAddr stop,
                         const char* what) {
        CpuState ref = in;
        {
            Interpreter interp(ref, *m_memory);
            for (u32 n = 0; ref.rip != stop && n < 100000; ++n) {
                const ExecResult r = interp.step();
                ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch);
            }
        }
        CpuState jit = in;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, cb, jit), JitExecResult::Continue);
        ASSERT_FALSE(jit.pending_raise.active);
        expect_same_state(jit, ref, what);
    }

    ScopedCompileMemoIsolation m_memo;
};

// add rax, rbx ; xor rcx, rax ; shl rax, 3 ; sub rdx, rcx
// SUB is the last flag writer, so every compared RFLAGS bit is defined
// (SHL leaves AF, and OF for a count above 1, undefined: SDM Vol. 2B SAL/SHL).
const std::vector<u8> kAlu = {0x48, 0x01, 0xD8, 0x48, 0x31, 0xC1,
                              0x48, 0xC1, 0xE0, 0x03, 0x48, 0x29, 0xCA};

TEST_F(JitIrTierTest, BlockBelowThresholdStaysStencilThenTiersUpToTheIr) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u32 kThreshold = 8;
    TierThreshold tier(kThreshold);
    const GuestAddr stop = kCode64 + kAlu.size();

    CompiledBlock first{};
    BasicBlock* cached = publish(kAlu, first);
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    EXPECT_FALSE(m_jit->ir_emit_active()) << "tier 0 must not pay the IR";
    const std::vector<u32> sites = tier_sites(first);
    ASSERT_EQ(sites.size(), 1u);
    ASSERT_NE(sites[0], 0u) << "the tier site was left unbound";

    const auto s0 = JitCompiler::ir_tier_stats();
    for (u32 k = 1; k < kThreshold; ++k) {
        run_and_compare(first, seeded(k), stop, "tier 0");
        m_jit->service_cold_residency();
    }
    EXPECT_EQ(JitCompiler::tier_counter_value_for_test(sites[0]), kThreshold - 1);
    EXPECT_EQ(JitCompiler::ir_tier_stats().requests, s0.requests);
    EXPECT_NE(cached->compiled_code, nullptr) << "withdrawn below the threshold";

    // The threshold entry requests; the dispatcher withdraws this block.
    const u32 link_gen = JitCompiler::link_generation_for_test();
    run_and_compare(first, seeded(kThreshold), stop, "threshold entry");
    m_jit->service_cold_residency();
    const auto s1 = JitCompiler::ir_tier_stats();
    EXPECT_EQ(s1.requests - s0.requests, 1u);
    EXPECT_EQ(s1.promotions - s0.promotions, 1u);
    EXPECT_EQ(s1.withdrawals - s0.withdrawals, 1u);
    EXPECT_EQ(cached->compiled_code, nullptr);
    EXPECT_EQ(JitCompiler::link_generation_for_test(), link_gen)
        << "a tier-up must not invalidate every direct edge";

    // The next compile is tier 1: the IR, no counter, same results.
    CompiledBlock tier1 = recompile(kAlu);
    ASSERT_NE(tier1.code, nullptr);
    EXPECT_FALSE(m_jit->last_compile_was_tier0());
    EXPECT_TRUE(m_jit->ir_emit_active());
    EXPECT_TRUE(tier_sites(tier1).empty());
    EXPECT_EQ(JitCompiler::ir_tier_stats().tier1_compiles - s1.tier1_compiles, 1u);
    for (u32 k = 0; k < 4; ++k) run_and_compare(tier1, seeded(100 + k), stop, "tier 1");

    // The old translation (still valid for any thread running it) asking
    // again changes nothing on this thread.
    run_and_compare(first, seeded(7), stop, "old tier 0");
    m_jit->service_cold_residency();
    EXPECT_EQ(JitCompiler::ir_tier_stats().withdrawals, s1.withdrawals);

    // New bytes at the same address are a new identity: tier 0 again.
    const std::vector<u8> rewritten = {0x48, 0x01, 0xC8};  // add rax, rcx
    ASSERT_TRUE(m_memory->write_bytes(kCode64, rewritten.data(), rewritten.size()));
    CompiledBlock next = recompile(rewritten);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    EXPECT_EQ(tier_sites(next).size(), 1u);
}

TEST_F(JitIrTierTest, HotInBlockLoopTiersUpWithinOneEntry) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u32 kThreshold = 16;
    TierThreshold tier(kThreshold);
    // top: dec rcx ; jnz top -- one block whose back-edge is in-block.
    const std::vector<u8> loop = {0x48, 0xFF, 0xC9, 0x75, 0xFB};
    const GuestAddr stop = kCode64 + loop.size();
    CompiledBlock first{};
    BasicBlock* cached = publish(loop, first);
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    const std::vector<u32> sites = tier_sites(first);
    ASSERT_EQ(sites.size(), 1u);

    CpuState in = cpu64(0);
    in.gpr.set64(1, 200);  // RCX: 200 iterations in one dispatcher entry
    run_and_compare(first, in, stop, "loop at tier 0");
    EXPECT_GE(JitCompiler::tier_counter_value_for_test(sites[0]), kThreshold)
        << "loop iterations must count as entries";
    m_jit->service_cold_residency();
    EXPECT_EQ(cached->compiled_code, nullptr) << "a hot loop must tier up";

    CompiledBlock tier1 = recompile(loop);
    ASSERT_NE(tier1.code, nullptr);
    EXPECT_FALSE(m_jit->last_compile_was_tier0());
    EXPECT_TRUE(tier_sites(tier1).empty());
    in.gpr.set64(1, 1000);
    run_and_compare(tier1, in, stop, "loop at tier 1");
}

TEST_F(JitIrTierTest, ThresholdOneCompilesTheIrOnFirstCompile) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    for (int threshold : {0, 1}) {
        TierThreshold tier(threshold);
        CompiledBlock first{};
        ASSERT_NE(publish(kAlu, first), nullptr);
        EXPECT_FALSE(m_jit->last_compile_was_tier0()) << threshold;
        EXPECT_TRUE(m_jit->ir_emit_active()) << threshold;
        EXPECT_TRUE(tier_sites(first).empty()) << threshold;
        run_and_compare(first, seeded(3), kCode64 + kAlu.size(), "no tiering");
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }
}

TEST_F(JitIrTierTest, GateClosedNeverCountsOrTiers) {
    if (gate_open(true)) GTEST_SKIP() << "run with VITREO_JIT_IR=0 or 32";
    TierThreshold tier(4);
    CompiledBlock first{};
    BasicBlock* cached = publish(kAlu, first);
    ASSERT_NE(cached, nullptr);
    EXPECT_FALSE(m_jit->last_compile_was_tier0());
    EXPECT_FALSE(m_jit->ir_emit_active());
    EXPECT_TRUE(tier_sites(first).empty());
    for (u32 k = 0; k < 16; ++k) {
        run_and_compare(first, seeded(k), kCode64 + kAlu.size(), "IR off");
        m_jit->service_cold_residency();
    }
    EXPECT_NE(cached->compiled_code, nullptr);
}

// A cold piece: a linear64 load from a host page with one guest page denied,
// so every execution takes the access's slow stub (Intel SDM Vol. 3A 4.6).
TEST_F(JitIrTierTest, ColdResidencyWaitsForHotnessAndSharesTheTierOneCompile) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u64 kRo = 0x1111111111111111ull;
    constexpr u64 kRw = 0x2222222222222222ull;
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100, kRo));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x2000, kRw));
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000, MemoryProtection::None));
    ResidencyOn on(*m_jit);
    constexpr u32 kThreshold = 64;
    TierThreshold tier(kThreshold);
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax, [rbx]
    const GuestAddr stop = kCode64 + code.size();
    CompiledBlock first{};
    BasicBlock* cached = publish(code, first);
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    const std::vector<u32> cold =
        sites_of(first, JitCompiler::cold_counter_site_word());
    if (cold.empty()) GTEST_SKIP() << "no counted cold piece (split cold off)";

    // Cold and not hot: no profile, no recompile.
    const auto r0 = JitCompiler::cold_residency_stats();
    const std::vector<u32> tsites = tier_sites(first);
    ASSERT_EQ(tsites.size(), 1u);
    for (u32 k = 1; k < kThreshold; ++k) {
        run_and_compare(first, cpu64(kData64 + 0x2000), stop, "tier 0");
        ASSERT_EQ(JitCompiler::tier_counter_value_for_test(tsites[0]), k);
        ASSERT_EQ(JitCompiler::cold_counter_value_for_test(cold[0]), k);
        m_jit->service_cold_residency();
        ASSERT_EQ(JitCompiler::cold_residency_stats().profiles, r0.profiles) << k;
    }
    EXPECT_EQ(JitCompiler::cold_residency_stats().profiles, r0.profiles);
    EXPECT_EQ(JitCompiler::cold_residency_stats().variant_compiles, r0.variant_compiles);
    EXPECT_NE(cached->compiled_code, nullptr);

    // Hot: one recompile that is both tier 1 and the residency variant.
    run_and_compare(first, cpu64(kData64 + 0x2000), stop, "threshold entry");
    m_jit->service_cold_residency();
    EXPECT_EQ(cached->compiled_code, nullptr);
    EXPECT_EQ(JitCompiler::cold_residency_stats().profiles - r0.profiles, 1u);
    CompiledBlock tier1 = recompile(code);
    ASSERT_NE(tier1.code, nullptr);
    EXPECT_FALSE(m_jit->last_compile_was_tier0());
    const auto r1 = JitCompiler::cold_residency_stats();
    EXPECT_EQ(r1.variant_compiles - r0.variant_compiles, 1u);
    EXPECT_TRUE(sites_of(tier1, JitCompiler::cold_counter_site_word()).empty())
        << "tier 1 must not count toward a second recompile";
    EXPECT_TRUE(tier_sites(tier1).empty());
    run_and_compare(tier1, cpu64(kData64 + 0x2000), stop, "tier 1 rw half");
    run_and_compare(tier1, cpu64(kData64 + 0x100), stop, "tier 1 ro half");
}

/// Whether an A64 instruction writes PSTATE.NZCV, by encoding class (Arm ARM
/// C4.1): ADDS/SUBS (immediate, shifted and extended register), ANDS/BICS
/// (immediate and shifted register), ADCS/SBCS, CCMP/CCMN, FCMP/FCCMP, and
/// MSR NZCV.
bool writes_nzcv(u32 w) {
    const bool s = (w & (1u << 29)) != 0;
    if ((w & 0x1F000000u) == 0x11000000u) return s;              // add/sub imm
    if ((w & 0x1F000000u) == 0x0B000000u) return s;              // add/sub reg
    if ((w & 0x1F800000u) == 0x12000000u) return (w >> 29 & 3) == 3;  // logical imm
    if ((w & 0x1F000000u) == 0x0A000000u) return (w >> 29 & 3) == 3;  // logical reg
    if ((w & 0x1FE00000u) == 0x1A000000u) return s;              // adc/sbc
    if ((w & 0x1FE00000u) == 0x1A400000u) return s;              // ccmp/ccmn
    if ((w & 0x5F203C00u) == 0x1E202000u) return true;           // fcmp
    if ((w & 0x5F200C00u) == 0x1E200400u) return true;           // fccmp
    if ((w & 0xFFFFFFE0u) == 0xD51B4200u) return true;           // msr nzcv
    return false;
}

// A tier site sits right after the body start, where a linked predecessor's
// NZCV may still be live: none of its words may write the flags.
TEST_F(JitIrTierTest, TierSiteNeverWritesTheFlags) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    TierThreshold tier(8);
    CompiledBlock first{};
    ASSERT_NE(publish(kAlu, first), nullptr);
    ASSERT_TRUE(m_jit->last_compile_was_tier0());
    const u32 tail = JitCompiler::tier_counter_site_word();
    u32 found = 0;
    for (const WordRange& r : block_word_ranges(first)) {
        for (const u32* w = r.begin; w + 3 <= r.end; ++w) {
            if (w[0] != tail || w[1] != kBlrX30) continue;
            const u32* site = w - (JitCompiler::kTierSiteWords - 3);
            ASSERT_GE(site, r.begin);
            for (u32 i = 0; i + 1 < JitCompiler::kTierSiteWords; ++i) {
                EXPECT_FALSE(writes_nzcv(site[i]))
                    << "site word " << i << " = 0x" << std::hex << site[i];
            }
            ++found;
        }
    }
    EXPECT_EQ(found, 1u);
}

// top: adc rax, rbx ; dec rcx ; jnz top -- DEC leaves CF (SDM Vol. 2A DEC),
// so each iteration's ADC reads the previous ADC's carry across the in-block
// loop-back and through the tier site. Threshold 16 under 50 iterations: the
// site's request tail runs on every iteration from the 16th.
TEST_F(JitIrTierTest, CarryCrossesTheTierSiteOnEveryLoopBack) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u32 kThreshold = 16;
    TierThreshold tier(kThreshold);
    const std::vector<u8> loop = {0x48, 0x11, 0xD8, 0x48, 0xFF, 0xC9, 0x75, 0xF8};
    const GuestAddr stop = kCode64 + loop.size();
    CompiledBlock first{};
    ASSERT_NE(publish(loop, first), nullptr);
    ASSERT_TRUE(m_jit->last_compile_was_tier0());
    const std::vector<u32> sites = tier_sites(first);
    ASSERT_EQ(sites.size(), 1u);
    for (u64 k = 0; k < 4; ++k) {
        CpuState in = cpu64(0);
        in.gpr.set64(0, 0xFFFFFFFFFFFFFFF0ull - k);
        in.gpr.set64(3, 0x8000000000000007ull + k * 0x1111);
        in.gpr.set64(1, 50);
        in.flags.set_cf((k & 1) != 0);
        run_and_compare(first, in, stop, "adc loop at tier 0");
        m_jit->service_cold_residency();
    }
}

// Blocks that cross the threshold between two dispatcher entries (one linked
// chain) are all serviced by the next one, not only the last to ask.
TEST_F(JitIrTierTest, OneDispatcherEntryServicesEveryBlockThatCrossed) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u32 kThreshold = 4;
    constexpr u32 kBlocks = 3;
    TierThreshold tier(kThreshold);
    // Three distinct one-instruction blocks: add rax, rbx / rcx / rdx.
    const std::vector<u8> codes[kBlocks] = {
        {0x48, 0x01, 0xD8}, {0x48, 0x01, 0xC8}, {0x48, 0x01, 0xD0}};
    const GuestAddr starts[kBlocks] = {kCode64, kCode64 + 0x100, kCode64 + 0x200};
    BasicBlock* cached[kBlocks]{};
    CompiledBlock compiled[kBlocks]{};
    for (u32 b = 0; b < kBlocks; ++b) {
        ASSERT_TRUE(m_memory->write_bytes(starts[b], codes[b].data(), codes[b].size()));
        BasicBlock built = build64(codes[b]);
        built.start_addr = starts[b];  // position-independent encodings
        cached[b] = m_cache->insert(built);
        CompiledBlock* cb = m_jit->compile(*cached[b]);
        ASSERT_NE(cb, nullptr);
        ASSERT_TRUE(m_jit->last_compile_was_tier0());
        compiled[b] = *cb;
        m_jit->publish_compiled_block(*cached[b], *cb);
    }
    const auto s0 = JitCompiler::ir_tier_stats();
    // Interleaved entries past the threshold: a queued block asks once.
    for (u32 k = 0; k < kThreshold + 5; ++k) {
        for (u32 b = 0; b < kBlocks; ++b) {
            CpuState in = seeded(k);
            in.rip = starts[b];
            run_and_compare(compiled[b], in, starts[b] + codes[b].size(), "chain");
        }
    }
    m_jit->service_cold_residency();
    const auto s1 = JitCompiler::ir_tier_stats();
    EXPECT_EQ(s1.requests - s0.requests, kBlocks);
    EXPECT_EQ(s1.withdrawals - s0.withdrawals, kBlocks);
    for (u32 b = 0; b < kBlocks; ++b) {
        EXPECT_EQ(cached[b]->compiled_code, nullptr) << "block " << b;
    }
}

// More blocks cross than one thread's request ring holds: the overflow is
// not lost, it asks again kTierRetryEntries entries later.
TEST_F(JitIrTierTest, FullRequestRingRetriesInsteadOfDropping) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u32 kThreshold = 2;
    constexpr u32 kBlocks = JIT_CTX_TIER_REQUEST_RING + 1;
    constexpr u32 kStride = 8;
    TierThreshold tier(kThreshold);
    const std::vector<u8> code = {0x48, 0x01, 0xD8};  // add rax, rbx
    std::vector<BasicBlock*> cached(kBlocks);
    std::vector<CompiledBlock> compiled(kBlocks);
    for (u32 b = 0; b < kBlocks; ++b) {
        const GuestAddr start = kCode64 + GuestAddr{b} * kStride;
        ASSERT_TRUE(m_memory->write_bytes(start, code.data(), code.size()));
        BasicBlock built = build64(code);
        built.start_addr = start;
        cached[b] = m_cache->insert(built);
        CompiledBlock* cb = m_jit->compile(*cached[b]);
        ASSERT_NE(cb, nullptr);
        ASSERT_TRUE(m_jit->last_compile_was_tier0());
        compiled[b] = *cb;
        m_jit->publish_compiled_block(*cached[b], *cb);
    }
    const auto run = [&](u32 b, u64 k) {
        CpuState in = seeded(k);
        in.rip = kCode64 + GuestAddr{b} * kStride;
        run_and_compare(compiled[b], in, in.rip + code.size(), "block");
    };
    const auto s0 = JitCompiler::ir_tier_stats();
    for (u32 k = 0; k < kThreshold; ++k) {
        for (u32 b = 0; b < kBlocks; ++b) run(b, k);
    }
    m_jit->service_cold_residency();
    const auto s1 = JitCompiler::ir_tier_stats();
    EXPECT_EQ(s1.withdrawals - s0.withdrawals, JIT_CTX_TIER_REQUEST_RING);
    const u32 last = kBlocks - 1;
    ASSERT_NE(cached[last]->compiled_code, nullptr) << "the ring held more than it can";

    for (u32 k = 1; k < JitCompiler::kTierRetryEntries; ++k) run(last, k);
    m_jit->service_cold_residency();
    EXPECT_NE(cached[last]->compiled_code, nullptr) << "asked before the retry window";
    run(last, JitCompiler::kTierRetryEntries);
    m_jit->service_cold_residency();
    EXPECT_EQ(JitCompiler::ir_tier_stats().withdrawals - s1.withdrawals, 1u);
    EXPECT_EQ(cached[last]->compiled_code, nullptr);
}

// Past the counter-slot cap a block stays at tier 0 with no counter: it is
// neither compiled with the IR up front nor ever asked to tier up.
TEST_F(JitIrTierTest, SlotCapLeavesTierZeroUncounted) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    constexpr u32 kThreshold = 4;
    TierThreshold tier(kThreshold);
    struct SlotLimit {
        SlotLimit() {
            JitCompiler::set_tier_slot_limit_for_test(
                JitCompiler::tier_slots_used_for_test());
        }
        ~SlotLimit() { JitCompiler::set_tier_slot_limit_for_test(0); }
    } limit;
    const auto s0 = JitCompiler::ir_tier_stats();
    CompiledBlock first{};
    BasicBlock* cached = publish(kAlu, first);
    ASSERT_NE(cached, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    EXPECT_FALSE(m_jit->ir_emit_active()) << "the cap must not mean IR on first compile";
    EXPECT_TRUE(tier_sites(first).empty()) << "an uncounted site was left callable";
    EXPECT_EQ(JitCompiler::ir_tier_stats().uncounted - s0.uncounted, 1u);
    EXPECT_EQ(JitCompiler::ir_tier_stats().tier0_compiles, s0.tier0_compiles);
    for (u32 k = 0; k < 4 * kThreshold; ++k) {
        run_and_compare(first, seeded(k), kCode64 + kAlu.size(), "uncounted tier 0");
        m_jit->service_cold_residency();
    }
    EXPECT_EQ(JitCompiler::ir_tier_stats().requests, s0.requests);
    EXPECT_NE(cached->compiled_code, nullptr);
}

// A hot block whose IR declines converges on an UNCOUNTED tier-0 translation.
// The tier-up compile is abandoned before emitting anything and its retry
// compiles tier 0 without a counter site (keyed apart in the memo, so it does
// not adopt the counted code). A second thread still running the counted
// translation asks on its next entry -- the counter was zeroed -- and its
// request for the declined identity withdraws that translation instead of
// parking the counter; its next compile adopts the uncounted code. After
// that no entry touches the slot. Parking used to leave the inline
// load/decrement/store of the shared slot on every entry of every thread.
TEST_F(JitIrTierTest, IrDeclineAtTierUpConvergesOnAnUncountedTierZero) {
    if (!gate_open(true)) GTEST_SKIP() << "VITREO_JIT_IR excludes 64-bit blocks";
    // Candidates the Tier-1 frontend or backend may not lower; the first one
    // this build declines is used (asserted below, not assumed).
    // Each is deterministic and defines every RFLAGS bit it changes.
    const std::vector<std::vector<u8>> candidates = {
        {0xD9, 0xE8},                    // fld1
        {0xF2, 0x0F, 0x38, 0xF1, 0xC3},  // crc32 eax, ebx
        {0x66, 0x0F, 0x38, 0x00, 0xC1},  // pshufb xmm0, xmm1
        {0x48, 0x0F, 0xC8},              // bswap rax
        {0xF3, 0x48, 0x0F, 0xB8, 0xC3},  // popcnt rax, rbx
    };
    std::vector<u8> code;
    {
        TierThreshold untiered(0);
        for (const std::vector<u8>& c : candidates) {
            ASSERT_TRUE(m_memory->write_bytes(kCode64, c.data(), c.size()));
            BasicBlock b = build64(c);
            if (b.instruction_count == 0) continue;
            CompiledBlock* cb = m_jit->compile(b);
            if (cb != nullptr && !m_jit->ir_emit_active()) {
                code = c;
                break;
            }
        }
    }
    if (code.empty()) GTEST_SKIP() << "the IR lowers every candidate";
    m_cache = std::make_unique<BlockCache>();
    m_jit->set_block_cache(m_cache.get());
    JitCompileMemo::clear_compile_memo();
    JitCompileMemo::enable_compile_memo();

    constexpr u32 kThreshold = 8;
    TierThreshold tier(kThreshold);
    const GuestAddr stop = kCode64 + code.size();
    CompiledBlock first{};
    BasicBlock* cached = publish(code, first);
    ASSERT_NE(cached, nullptr);
    ASSERT_TRUE(m_jit->last_compile_was_tier0());
    ASSERT_EQ(tier_sites(first).size(), 1u);

    // A second thread runs the same counted translation from the memo.
    JitCompiler other(*m_memory);
    BlockCache other_cache;
    other.set_block_cache(&other_cache);
    BasicBlock* other_cached = other_cache.insert(build64(code));
    CompiledBlock* shared = other.compile(*other_cached);
    ASSERT_NE(shared, nullptr);
    ASSERT_EQ(shared->code, first.code) << "the memo did not share the tier-0 code";
    CompiledBlock shared_copy = *shared;
    other.publish_compiled_block(*other_cached, *shared);

    const auto s0 = JitCompiler::ir_tier_stats();
    for (u32 k = 1; k <= kThreshold; ++k) {
        run_and_compare(first, seeded(k), stop, "tier 0");
        m_jit->service_cold_residency();
    }
    ASSERT_EQ(cached->compiled_code, nullptr) << "the threshold must withdraw";

    BasicBlock redo = build64(code);
    CompiledBlock* again = m_jit->compile(redo);
    ASSERT_NE(again, nullptr);
    const auto s1 = JitCompiler::ir_tier_stats();
    EXPECT_EQ(s1.declines - s0.declines, 1u);
    EXPECT_EQ(s1.tier1_compiles, s0.tier1_compiles);
    EXPECT_EQ(s1.tier0_compiles, s0.tier0_compiles) << "a counted retry";
    EXPECT_EQ(s1.uncounted - s0.uncounted, 1u) << "the retry is not uncounted";
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    EXPECT_NE(again->code, first.code) << "the counted translation was adopted";
    EXPECT_TRUE(tier_sites(*again).empty()) << "the retry kept a counter site";
    CompiledBlock uncounted = *again;
    m_jit->publish_compiled_block(*cached, *again);

    // The other thread's next entry asks; its request withdraws the counted
    // translation rather than parking the shared counter.
    {
        CpuState in = seeded(40);
        CpuState ref = in;
        {
            Interpreter interp(ref, *m_memory);
            for (u32 n = 0; ref.rip != stop && n < 1000; ++n) {
                const ExecResult r = interp.step();
                ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch);
            }
        }
        CpuState jit = in;
        ASSERT_EQ(test::execute_like_dispatcher(other, shared_copy, jit),
                  JitExecResult::Continue);
        expect_same_state(jit, ref, "other thread, counted tier 0");
    }
    other.service_cold_residency();
    const auto s2 = JitCompiler::ir_tier_stats();
    EXPECT_EQ(s2.parked, s1.parked) << "the counter was parked, not released";
    EXPECT_EQ(s2.withdrawals - s1.withdrawals, 1u);
    EXPECT_EQ(other_cached->compiled_code, nullptr);
    CompiledBlock* swapped = other.compile(*other_cached);
    ASSERT_NE(swapped, nullptr);
    EXPECT_EQ(swapped->code, uncounted.code) << "the uncounted code was not adopted";
    EXPECT_TRUE(tier_sites(*swapped).empty());
    EXPECT_EQ(JitCompiler::ir_tier_stats().declines, s1.declines);

    // From here on no entry of either thread reaches the counter.
    for (u32 k = 0; k < 4 * kThreshold; ++k) {
        run_and_compare(uncounted, seeded(60 + k), stop, "uncounted tier 0");
        m_jit->service_cold_residency();
        other.service_cold_residency();
    }
    EXPECT_EQ(JitCompiler::ir_tier_stats().requests, s2.requests);

    // With no memo record left, the identity compiles straight to an
    // uncounted tier 0 without building the IR again.
    JitCompileMemo::clear_compile_memo();
    CompiledBlock fresh = recompile(code);
    ASSERT_NE(fresh.code, nullptr);
    EXPECT_TRUE(m_jit->last_compile_was_tier0());
    EXPECT_TRUE(tier_sites(fresh).empty());
    EXPECT_EQ(JitCompiler::ir_tier_stats().declines, s1.declines);
    run_and_compare(fresh, seeded(99), stop, "uncounted tier 0");
}

class JitIrTier32Test : public JitLinearTest {
protected:
    ScopedCompileMemoIsolation m_memo;
};

TEST_F(JitIrTier32Test, MidBlockLoopBackCountsEveryIteration) {
    if (!gate_open(false)) GTEST_SKIP() << "VITREO_JIT_IR excludes 32-bit blocks";
    constexpr u32 kThreshold = 16;
    TierThreshold tier(kThreshold);
    // mov ecx, 50 ; top: dec ecx ; jnz top -- the loop header is mid-block.
    const std::vector<u8> code = {0xB9, 0x32, 0x00, 0x00, 0x00, 0x49, 0x75, 0xFD};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock* cached = m_cache->insert(build_block(code));
    CompiledBlock* cb = m_jit->compile(*cached);
    ASSERT_NE(cb, nullptr);
    CompiledBlock first = *cb;
    m_jit->publish_compiled_block(*cached, *cb);
    if (!m_jit->last_compile_was_tier0()) GTEST_SKIP() << "block not linear 32-bit";
    const std::vector<u32> sites = tier_sites(first);
    ASSERT_EQ(sites.size(), 2u) << "body start and the mid-block loop-back";
    ASSERT_EQ(sites[0], sites[1]) << "one slot per translation";

    CpuState ref;
    init_cpu(ref);
    {
        Interpreter interp(ref, *m_memory);
        while (ref.rip != kCode + code.size()) {
            const ExecResult r = interp.step();
            ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch);
        }
    }
    CpuState jit;
    init_cpu(jit);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, first, jit), JitExecResult::Continue);
    expect_same_state(jit, ref, "x86 loop at tier 0");
    EXPECT_GE(JitCompiler::tier_counter_value_for_test(sites[0]), kThreshold);
    m_jit->service_cold_residency();
    EXPECT_EQ(cached->compiled_code, nullptr) << "a hot mid-block loop must tier up";
}
