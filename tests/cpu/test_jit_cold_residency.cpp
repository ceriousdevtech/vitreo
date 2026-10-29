// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Cold-path residency (jit_compiler_cold_residency.hpp): cold-entry counters,
// the one residency recompile per block identity they request, and the
// stack-entry proof a block keeps failing. The cold piece driven here is a
// linear64 access to a mixed host page, whose fast-path guard sends every
// execution to the access's slow stub; expected guest results are the
// access's architectural ones (Intel SDM Vol. 3A 4.6, #PF -> the guest's
// STATUS_ACCESS_VIOLATION with the faulting address).

#include "jit_linear_fixture.hpp"

#include <cstdlib>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

constexpr u32 kBlrX30 = 0xD63F03C0u;
constexpr u32 kThreshold = JitCompiler::kColdEntryThreshold;

// Threshold 0: residency without tiering (its interaction with the tier
// counter is test_jit_ir_tier.cpp's).
struct ResidencyOn {
    explicit ResidencyOn(JitCompiler& jit) : m_jit(jit) {
        JitCompiler::set_cold_residency_for_test(1);
        JitCompiler::set_ir_tier_threshold_for_test(0);
        m_jit.reset_cold_residency_for_test();
    }
    ~ResidencyOn() {
        m_jit.reset_cold_residency_for_test();
        JitCompiler::set_ir_tier_threshold_for_test(-1);
        JitCompiler::set_cold_residency_for_test(-1);
    }
    JitCompiler& m_jit;
};

// Data words of the block's bound counter sites.
std::vector<u32> counter_words(const CompiledBlock& cb) {
    std::vector<u32> out;
    for (const WordRange& r : block_word_ranges(cb)) {
        for (const u32* w = r.begin; w + 2 < r.end; ++w) {
            if (w[0] == JitCompiler::cold_counter_site_word() && w[1] == kBlrX30) {
                out.push_back(w[2]);
            }
        }
    }
    return out;
}

}  // namespace

class JitColdResidencyTest : public JitLinear64Test {
protected:
    // kData64's host page with one guest page denied: every read of the host
    // page takes its access's slow stub.
    void make_read_mixed_data_page() {
        ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100, kRo));
        ASSERT_TRUE(m_memory->write_u64(kData64 + 0x2000, kRw));
        ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                             MemoryProtection::None));
        ASSERT_NE(m_memory->linear_check_page_map()[kData64 >>
                                                    kLinearHostPageShift] &
                      kLinearCheckMixedRead,
                  0u);
    }

    // kData64's host page with its first guest page read-only: every write
    // of the host page takes its access's slow stub.
    void make_write_mixed_data_page() {
        ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100, kRo));
        ASSERT_TRUE(m_memory->write_u64(kData64 + 0x2000, kRw));
        ASSERT_TRUE(m_memory->protect_region(kData64, 0x1000,
                                             MemoryProtection::Read));
        ASSERT_NE(m_memory->linear_check_page_map()[kData64 >>
                                                    kLinearHostPageShift],
                  0u);
    }

    BasicBlock* publish(const std::vector<u8>& code, CompiledBlock& out) {
        EXPECT_TRUE(m_memory->write_bytes(kCode64, code.data(), code.size()));
        BasicBlock* cached = m_cache->insert(build64(code));
        CompiledBlock* cb = m_jit->compile(*cached);
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) return nullptr;
        // The compile's own record: the cache entry keeps no cold layout.
        out = *cb;
        m_jit->publish_compiled_block(*cached, *cb);
        return cached;
    }

    // A fresh compile of the bytes at kCode64, as the dispatcher's next entry
    // after a withdrawal makes (the cache entry may have released its decode).
    CompiledBlock recompile(const std::vector<u8>& code) {
        BasicBlock fresh = build64(code);
        CompiledBlock* cb = m_jit->compile(fresh);
        EXPECT_NE(cb, nullptr);
        return cb ? *cb : CompiledBlock{};
    }

    JitExecResult run_load(CompiledBlock& cb, GuestAddr ea, u64 want) {
        CpuState cpu = cpu64(ea);
        const JitExecResult r = test::execute_like_dispatcher(*m_jit, cb, cpu);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.rax, want);
        EXPECT_EQ(cpu.rip, kCode64 + 3);
        return r;
    }

    static constexpr u64 kRo = 0x1111111111111111ull;
    static constexpr u64 kRw = 0x2222222222222222ull;
    ScopedCompileMemoIsolation m_memo;
};

TEST_F(JitColdResidencyTest, CounterRequestsOneRecompileThatPlacesTheStubHot) {
    ResidencyOn on(*m_jit);
    make_read_mixed_data_page();
    const std::vector<u8> code = {0x48, 0x8B, 0x03};  // mov rax, [rbx]
    CompiledBlock first{};
    BasicBlock* cached = publish(code, first);
    ASSERT_NE(cached, nullptr);
    // The counters and the recompile are shared by both tiers; under the
    // default policy this block is the IR tier's.
    const char* ir_env = std::getenv("VITREO_JIT_IR");
    const bool ir = ir_env == nullptr || ir_env[0] == '\0' ||
                    JitCompiler::ir_enabled_for(true);
    EXPECT_EQ(m_jit->ir_emit_active(), ir);
    const std::vector<u32> sites = counter_words(first);
    if (sites.empty()) GTEST_SKIP() << "no counted cold piece (split cold off)";
    ASSERT_EQ(sites.size(), 1u) << "one access, one memory stub";

    // Each execution enters the stub once and counts once; nothing is
    // requested below the threshold.
    const auto s0 = JitCompiler::cold_residency_stats();
    for (u32 k = 1; k < kThreshold; ++k) {
        ASSERT_EQ(run_load(first, kData64 + 0x2000, kRw), JitExecResult::Continue);
    }
    EXPECT_EQ(JitCompiler::cold_counter_value_for_test(sites[0]), kThreshold - 1);
    m_jit->service_cold_residency();
    EXPECT_EQ(JitCompiler::cold_residency_stats().requests, s0.requests);
    EXPECT_NE(cached->compiled_code, nullptr);

    // The threshold entry requests; the dispatcher withdraws only this block.
    const u32 link_gen = JitCompiler::link_generation_for_test();
    ASSERT_EQ(run_load(first, kData64 + 0x2000, kRw), JitExecResult::Continue);
    m_jit->service_cold_residency();
    const auto s1 = JitCompiler::cold_residency_stats();
    EXPECT_EQ(s1.requests - s0.requests, 1u);
    EXPECT_EQ(s1.profiles - s0.profiles, 1u);
    EXPECT_EQ(s1.withdrawals - s0.withdrawals, 1u);
    EXPECT_EQ(cached->compiled_code, nullptr);
    EXPECT_EQ(JitCompiler::link_generation_for_test(), link_gen)
        << "a residency recompile must not invalidate every direct edge";

    // The recompile is the residency variant: no counters, the stub placed
    // with the hot part, and the same results on both halves of the page.
    CompiledBlock variant = recompile(code);
    ASSERT_NE(variant.code, nullptr);
    EXPECT_EQ(m_jit->ir_emit_active(), ir);
    const auto s2 = JitCompiler::cold_residency_stats();
    EXPECT_EQ(s2.variant_compiles - s1.variant_compiles, 1u);
    EXPECT_EQ(s2.variant_split_moves - s1.variant_split_moves, 1u);
    EXPECT_TRUE(counter_words(variant).empty());
    EXPECT_LT(variant.cold_size, first.cold_size);
    EXPECT_EQ(run_load(variant, kData64 + 0x2000, kRw), JitExecResult::Continue);
    EXPECT_EQ(run_load(variant, kData64 + 0x100, kRo), JitExecResult::Continue);

    // One per epoch: the old translation (still valid for any thread running
    // it) reaching the threshold again changes nothing.
    for (u32 k = 0; k < kThreshold; ++k) {
        ASSERT_EQ(run_load(first, kData64 + 0x2000, kRw), JitExecResult::Continue);
    }
    m_jit->service_cold_residency();
    const auto s3 = JitCompiler::cold_residency_stats();
    EXPECT_EQ(s3.profiles, s2.profiles);
    EXPECT_EQ(s3.withdrawals, s2.withdrawals);
    CompiledBlock again = recompile(code);
    EXPECT_TRUE(counter_words(again).empty()) << "the profile must persist";

    // New code bytes at the same address are a new epoch, counted afresh.
    const std::vector<u8> rewritten = {0x48, 0x8B, 0x0B};  // mov rcx, [rbx]
    ASSERT_TRUE(m_memory->write_bytes(kCode64, rewritten.data(), rewritten.size()));
    CompiledBlock next = recompile(rewritten);
    EXPECT_EQ(counter_words(next).size(), 1u);
}

TEST_F(JitColdResidencyTest, RecompiledStoreStillFaultsPrecisely) {
    ResidencyOn on(*m_jit);
    make_write_mixed_data_page();
    const std::vector<u8> code = {0x48, 0x89, 0x03};  // mov [rbx], rax
    CompiledBlock first{};
    BasicBlock* cached = publish(code, first);
    ASSERT_NE(cached, nullptr);
    if (counter_words(first).empty()) {
        GTEST_SKIP() << "no counted cold piece (split cold off)";
    }
    for (u32 k = 0; k < kThreshold; ++k) {
        CpuState cpu = cpu64(kData64 + 0x2000);
        cpu.gpr.rax = k;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, first, cpu),
                  JitExecResult::Continue);
        ASSERT_FALSE(cpu.pending_raise.active);
    }
    m_jit->service_cold_residency();
    ASSERT_EQ(cached->compiled_code, nullptr);
    CompiledBlock variant = recompile(code);
    ASSERT_NE(variant.code, nullptr);
    EXPECT_TRUE(counter_words(variant).empty());

    // The read-only page rejects the store at the instruction's RIP with the
    // exact address, leaving memory untouched.
    CpuState ro = cpu64(kData64 + 0x100);
    ro.gpr.rax = 0xDEADBEEFDEADBEEFull;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, variant, ro),
              JitExecResult::Continue);
    ASSERT_TRUE(ro.pending_raise.active);
    EXPECT_EQ(ro.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(ro.pending_raise.args[0], 1u);
    EXPECT_EQ(ro.pending_raise.args[1], kData64 + 0x100);
    EXPECT_EQ(ro.rip, kCode64);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x100).value_or(0), kRo);

    // The read-write page of the same host page takes it.
    CpuState rw = cpu64(kData64 + 0x2000);
    rw.gpr.rax = 0xFEEDFACEFEEDFACEull;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, variant, rw),
              JitExecResult::Continue);
    EXPECT_FALSE(rw.pending_raise.active);
    EXPECT_EQ(rw.rip, kCode64 + 3);
    EXPECT_EQ(m_memory->read_u64(kData64 + 0x2000).value_or(0),
              0xFEEDFACEFEEDFACEull);
}

TEST_F(JitColdResidencyTest, RepeatedStackProofDeoptsDropTheProofOnce) {
    ResidencyOn on(*m_jit);
    // mov rax, [rsp+8] ; mov rcx, [rsp+16]
    const std::vector<u8> code = {0x48, 0x8B, 0x44, 0x24, 0x08,
                                  0x48, 0x8B, 0x4C, 0x24, 0x10};
    constexpr GuestAddr stack = kData64 + 0x1000;
    constexpr u64 a = 0x1122334455667788ull;
    constexpr u64 b = 0x8877665544332211ull;
    ASSERT_TRUE(m_memory->write_u64(stack + 8, a));
    ASSERT_TRUE(m_memory->write_u64(stack + 16, b));
    CompiledBlock first{};
    BasicBlock* cached = publish(code, first);
    ASSERT_NE(cached, nullptr);
    if (!first.requires_protection_lease) {
        GTEST_SKIP() << "the stack-entry proof is not in effect";
    }
    // The stack's host page becomes mixed after the compile: the entry proof
    // rejects every entry and the interpreter runs the block.
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x3000, 0x1000,
                                         MemoryProtection::None));
    const auto s0 = JitCompiler::cold_residency_stats();
    for (u32 k = 0; k < JitCompiler::kStackProofDeoptThreshold; ++k) {
        CpuState cpu = cpu64(0);
        cpu.gpr.rsp = stack;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, first, cpu),
                  JitExecResult::Unsupported);
        ASSERT_EQ(cpu.rip, kCode64);
    }
    m_jit->service_cold_residency();
    const auto s1 = JitCompiler::cold_residency_stats();
    EXPECT_EQ(s1.proof_drops - s0.proof_drops, 1u);
    EXPECT_EQ(s1.withdrawals - s0.withdrawals, 1u);
    EXPECT_EQ(cached->compiled_code, nullptr);

    CompiledBlock variant = recompile(code);
    ASSERT_NE(variant.code, nullptr);
    EXPECT_FALSE(variant.requires_protection_lease);
    for (u32 k = 0; k < 2 * JitCompiler::kStackProofDeoptThreshold; ++k) {
        CpuState cpu = cpu64(0);
        cpu.gpr.rsp = stack;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, variant, cpu),
                  JitExecResult::Continue);
        ASSERT_FALSE(cpu.pending_raise.active);
        ASSERT_EQ(cpu.gpr.rax, a);
        ASSERT_EQ(cpu.gpr.rcx, b);
        ASSERT_EQ(cpu.rip, kCode64 + code.size());
    }
    m_jit->service_cold_residency();
    EXPECT_EQ(JitCompiler::cold_residency_stats().withdrawals, s1.withdrawals);
}
