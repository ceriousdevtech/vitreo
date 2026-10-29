// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Mid-block Jcc B.cond retarget (jit_compiler_cold_residency.hpp): a mid-block
// Jcc that carries live NZCV into its forward direct-link slot branches
// straight to the successor's linked entry once that slot is current, and goes
// back to the slot whenever the slot fails closed. Expected outcomes are the
// Jcc's architectural ones (Intel SDM Vol. 2A, Jcc: jump iff the condition
// holds), cross-checked against the interpreter's lazy-condition evaluation.

#include "jit_exec_test_fixture.hpp"

#include <cstdlib>

namespace {

struct RetargetOn {
    // Threshold 0: the first compile is the IR tier, the one hot blocks run.
    explicit RetargetOn(int retarget = 1) {
        JitCompiler::set_cold_residency_for_test(1);
        JitCompiler::set_jcc_retarget_for_test(retarget);
        JitCompiler::set_ir_tier_threshold_for_test(0);
    }
    ~RetargetOn() {
        JitCompiler::set_ir_tier_threshold_for_test(-1);
        JitCompiler::set_jcc_retarget_for_test(-1);
        JitCompiler::set_cold_residency_for_test(-1);
    }
};

// The conditional branches -- B.cond (Arm ARM C6.2.26), CBZ/CBNZ (C6.2.47,
// C6.2.46) -- in a block's hot part that branch to `entry`.
std::vector<const u32*> b_conds_to(const CompiledBlock& cb, const void* entry) {
    std::vector<const u32*> out;
    const auto* w = static_cast<const u32*>(cb.code);
    for (usize i = 0; i < cb.code_size / sizeof(u32); ++i) {
        if ((w[i] & 0xFF000010u) != 0x54000000u &&
            (w[i] & 0x7E000000u) != 0x34000000u) {
            continue;
        }
        const i64 imm19 = static_cast<i64>(static_cast<i32>(w[i] << 8) >> 13);
        if (reinterpret_cast<const u8*>(w + i) + imm19 * 4 ==
            static_cast<const u8*>(entry)) {
            out.push_back(w + i);
        }
    }
    return out;
}

// Incoming flags of `cmp a, b` (32-bit): the lazy record, and for `carried`
// the predecessor's live NZCV with its Sub kind, as a linked-tail miss
// leaves them (CpuState::nzcv_kind / nzcv_bits).
void set_incoming_cmp32(CpuState& cpu, u32 a, u32 b, bool carried) {
    const u32 r = a - b;
    cpu.flag_op = CpuState::FlagOp::Sub32;
    cpu.flag_src = b;
    cpu.flag_dst = r;
    cpu.cf_overlay = 0;
    cpu.of_overlay = 0;
    u32 nzcv = 0;
    if (r & 0x80000000u) nzcv |= 1u << 31;                 // N
    if (r == 0) nzcv |= 1u << 30;                          // Z
    if (a >= b) nzcv |= 1u << 29;                          // C: no borrow
    if (((a ^ b) & (a ^ r)) & 0x80000000u) nzcv |= 1u << 28;  // V
    cpu.nzcv_kind = carried ? static_cast<u8>(NzcvState::Sub) : 0;
    cpu.nzcv_bits = carried ? nzcv : 0;
}

bool interpreter_takes_jz(u32 a, u32 b) {
    CpuState reference;
    reference.init_long_mode();
    set_incoming_cmp32(reference, a, b, false);
    const int take = reference.try_lazy_condition(0x4);  // E / Z
    EXPECT_GE(take, 0);
    return take == 1;
}

}  // namespace

class JitJccRetargetTest : public JitExecTest {
protected:
    void SetUp() override {
        JitExecTest::SetUp();
        m_jit->set_block_cache(&m_cache);
    }
    void TearDown() override { m_jit->set_block_cache(nullptr); }

    CompiledBlock publish(GuestAddr address, const u8* code, usize len,
                          bool is64 = true) {
        BasicBlock decoded = is64 ? build_block64_at(address, code, len)
                                  : build_block32_at(address, code, len);
        BasicBlock* cached = m_cache.insert(std::move(decoded));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* cb = cached ? m_jit->compile(*cached) : nullptr;
        EXPECT_NE(cb, nullptr);
        if (cb) m_jit->publish_compiled_block(*cached, *cb);
        return cb ? compiled_from_cache(*cached) : CompiledBlock{};
    }

    BasicBlock build_block32_at(GuestAddr address, const u8* code, usize len) {
        EXPECT_TRUE(m_memory->write_bytes(address, code, len));
        BasicBlock block;
        block.start_addr = address;
        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    // 0x1100: mov ebx, 7 ; jmp 0x1200
    void publish_taken(bool is64 = true) {
        static const u8 taken[] = {0xBB, 0x07, 0x00, 0x00, 0x00,
                                   0xE9, 0xF6, 0x00, 0x00, 0x00};
        m_taken = publish(0x1100, taken, sizeof(taken), is64);
    }

    // 0x1000: mov ecx, 1 ; jz 0x1100 ; mov ebx, 2 ; jmp 0x1300
    // The jz reads the predecessor's flags: no flag writer precedes it.
    CompiledBlock publish_carried_caller(bool is64 = true) {
        static const u8 caller[] = {
            0xB9, 0x01, 0x00, 0x00, 0x00,
            0x0F, 0x84, 0xF5, 0x00, 0x00, 0x00,
            0xBB, 0x02, 0x00, 0x00, 0x00,
            0xE9, 0xEB, 0x02, 0x00, 0x00};
        return publish(0x1000, caller, sizeof(caller), is64);
    }

    void run_and_check(CompiledBlock& entry, u32 a, u32 b, bool carried) {
        SCOPED_TRACE(::testing::Message() << "cmp " << a << ", " << b
                                          << (carried ? " (carried NZCV)" : " (lazy)"));
        const bool taken = interpreter_takes_jz(a, b);
        m_cpu->gpr.set64(EBX, 0);
        m_cpu->gpr.set64(ECX, 0);
        set_incoming_cmp32(*m_cpu, a, b, carried);
        m_cpu->rip = 0x1000;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, entry, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get64(ECX), 1u);
        EXPECT_EQ(m_cpu->gpr.get64(EBX), taken ? 7u : 2u);
        EXPECT_EQ(m_cpu->rip, taken ? 0x1200u : 0x1300u);
    }

    BlockCache m_cache;
    CompiledBlock m_taken{};
};

TEST_F(JitJccRetargetTest, CarriedMidBlockJccBranchesStraightToItsSuccessor) {
    JitLinkGuard link_guard;
    RetargetOn on;
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::MidJcc)) {
        GTEST_SKIP() << "mid-block Jcc direct links are disabled";
    }
    publish_taken();
    ASSERT_NE(m_taken.link_entry(), nullptr);
    const auto before = JitCompiler::cold_residency_stats();
    CompiledBlock caller = publish_carried_caller();
    const auto after = JitCompiler::cold_residency_stats();
    const char* ir_env = std::getenv("VITREO_JIT_IR");
    EXPECT_EQ(m_jit->ir_emit_active(), ir_env == nullptr || ir_env[0] == '\0' ||
                                           JitCompiler::ir_enabled_for(true))
        << "the retarget must cover the default tier of a 64-bit block";
    EXPECT_GE(after.retarget_candidates - before.retarget_candidates, 1u);
    // Tier 0 patches both the carried B.cond and the evaluated CBNZ W0; the
    // IR tier has the CBNZ W0 alone.
    const usize patched = b_conds_to(caller, m_taken.link_entry()).size();
    ASSERT_GE(patched, 1u)
        << "the mid-block jz's branch was not patched to the successor";
    EXPECT_EQ(after.retargets - before.retargets, patched);

    for (const bool carried : {true, false}) {
        run_and_check(caller, 0, 0, carried);    // ZF=1: taken
        run_and_check(caller, 1, 0, carried);    // ZF=0: fall through
        run_and_check(caller, 0, 1, carried);
    }
}

TEST_F(JitJccRetargetTest, GenerationBumpRestoresTheSlotEdge) {
    JitLinkGuard link_guard;
    RetargetOn on;
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::MidJcc)) {
        GTEST_SKIP() << "mid-block Jcc direct links are disabled";
    }
    publish_taken();
    CompiledBlock caller = publish_carried_caller();
    const usize patched = b_conds_to(caller, m_taken.link_entry()).size();
    ASSERT_GE(patched, 1u);

    // Every slot fails closed at once (an SMC invalidation): the B.cond has
    // no guard of its own, so it must stop reaching the successor directly.
    const auto before = JitCompiler::cold_residency_stats();
    JitCompiler::bump_link_generation_for_test();
    EXPECT_EQ(JitCompiler::cold_residency_stats().retarget_undos -
                  before.retarget_undos, patched);
    EXPECT_TRUE(b_conds_to(caller, m_taken.link_entry()).empty());
    run_and_check(caller, 0, 0, true);
    run_and_check(caller, 1, 0, true);

    // Republishing the successor re-patches the slot at the new generation,
    // and the B.cond with it.
    publish_taken();
    EXPECT_EQ(b_conds_to(caller, m_taken.link_entry()).size(), patched);
    run_and_check(caller, 0, 0, true);
}

TEST_F(JitJccRetargetTest, WithdrawingOneTargetRestoresOnlyItsEdges) {
    JitLinkGuard link_guard;
    RetargetOn on;
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::MidJcc)) {
        GTEST_SKIP() << "mid-block Jcc direct links are disabled";
    }
    // 0x1500: mov ebx, 7 ; jmp 0x1200
    static const u8 other[] = {0xBB, 0x07, 0x00, 0x00, 0x00,
                               0xE9, 0xF6, 0xFC, 0xFF, 0xFF};
    // 0x1400: mov ecx, 1 ; jz 0x1500 ; mov ebx, 2 ; jmp 0x1300
    static const u8 other_caller[] = {
        0xB9, 0x01, 0x00, 0x00, 0x00,
        0x0F, 0x84, 0xF5, 0x00, 0x00, 0x00,
        0xBB, 0x02, 0x00, 0x00, 0x00,
        0xE9, 0xEB, 0xFE, 0xFF, 0xFF};
    publish_taken();
    const CompiledBlock other_target = publish(0x1500, other, sizeof(other));
    ASSERT_NE(other_target.link_entry(), nullptr);
    CompiledBlock caller = publish_carried_caller();
    const CompiledBlock second = publish(0x1400, other_caller, sizeof(other_caller));
    const usize patched = b_conds_to(caller, m_taken.link_entry()).size();
    const usize other_patched = b_conds_to(second, other_target.link_entry()).size();
    ASSERT_GE(patched, 1u);
    ASSERT_GE(other_patched, 1u);

    // A residency withdrawal of 0x1100 fails only the edges into it closed.
    const auto before = JitCompiler::cold_residency_stats();
    JitCompiler::undo_jcc_retargets_for_test(m_taken.code);
    EXPECT_EQ(JitCompiler::cold_residency_stats().retarget_undos -
                  before.retarget_undos, patched);
    EXPECT_TRUE(b_conds_to(caller, m_taken.link_entry()).empty());
    EXPECT_EQ(b_conds_to(second, other_target.link_entry()).size(), other_patched);
    run_and_check(caller, 0, 0, true);
    run_and_check(caller, 1, 0, true);

    // A second withdrawal of the same target has nothing left to restore;
    // the generation bump still reaches the other target's edges.
    const auto mid = JitCompiler::cold_residency_stats();
    JitCompiler::undo_jcc_retargets_for_test(m_taken.code);
    EXPECT_EQ(JitCompiler::cold_residency_stats().retarget_undos, mid.retarget_undos);
    JitCompiler::bump_link_generation_for_test();
    EXPECT_EQ(JitCompiler::cold_residency_stats().retarget_undos - mid.retarget_undos,
              other_patched);
    EXPECT_TRUE(b_conds_to(second, other_target.link_entry()).empty());
}

TEST_F(JitJccRetargetTest, KillSwitchKeepsTheSlotEdge) {
    JitLinkGuard link_guard;
    RetargetOn on(/*retarget=*/0);
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::MidJcc)) {
        GTEST_SKIP() << "mid-block Jcc direct links are disabled";
    }
    publish_taken();
    const auto before = JitCompiler::cold_residency_stats();
    CompiledBlock caller = publish_carried_caller();
    EXPECT_EQ(JitCompiler::cold_residency_stats().retargets, before.retargets);
    EXPECT_TRUE(b_conds_to(caller, m_taken.link_entry()).empty());
    run_and_check(caller, 0, 0, true);
    run_and_check(caller, 1, 0, true);
}

// A Jcc whose flags the block itself produced. The successor must never
// see a stale NZCV kind: whenever nzcv_kind is nonzero after the block, the
// saved NZCV agrees with the block's own `cmp` (ZF = (a == b), Intel SDM
// Vol. 2A CMP). Tier 0 publishes the kind in the exit stub before the slot,
// so that edge keeps its stub. The IR tier clears the kind once in the hot
// body, so its exit carries nothing and is retargeted.
TEST_F(JitJccRetargetTest, ExitAfterAnInBlockFlagWriterNeverLeavesAStaleKind) {
    JitLinkGuard link_guard;
    RetargetOn on;
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::MidJcc)) {
        GTEST_SKIP() << "mid-block Jcc direct links are disabled";
    }
    publish_taken();
    // 0x1000: cmp eax, edx ; jz 0x1100 ; mov ebx, 2 ; jmp 0x1300
    const u8 caller_code[] = {
        0x39, 0xD0,
        0x0F, 0x84, 0xF8, 0x00, 0x00, 0x00,
        0xBB, 0x02, 0x00, 0x00, 0x00,
        0xE9, 0xEE, 0x02, 0x00, 0x00};
    const auto before = JitCompiler::cold_residency_stats();
    CompiledBlock caller = publish(0x1000, caller_code, sizeof(caller_code));
    const bool ir = m_jit->ir_emit_active();
    if (!ir) {
        EXPECT_EQ(JitCompiler::cold_residency_stats().retargets, before.retargets);
        EXPECT_TRUE(b_conds_to(caller, m_taken.link_entry()).empty());
    } else {
        EXPECT_EQ(JitCompiler::cold_residency_stats().retargets - before.retargets,
                  1u);
    }
    for (const auto [a, d] : {std::pair<u32, u32>{5, 5}, {5, 6}}) {
        // A stale carried kind whose Z contradicts both outcomes' cmp.
        set_incoming_cmp32(*m_cpu, 1, 2, /*carried=*/true);
        m_cpu->nzcv_bits = a == d ? 0u : (1u << 30);
        m_cpu->gpr.set64(EAX, a);
        m_cpu->gpr.set64(EDX, d);
        m_cpu->gpr.set64(EBX, 0);
        m_cpu->rip = 0x1000;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, caller, *m_cpu),
                  JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get64(EBX), a == d ? 7u : 2u);
        EXPECT_EQ(m_cpu->rip, a == d ? 0x1200u : 0x1300u);
        if (m_cpu->nzcv_kind != 0) {
            EXPECT_EQ((m_cpu->nzcv_bits >> 30) & 1u, a == d ? 1u : 0u)
                << "a nonzero kind left NZCV that is not the block's cmp";
        }
        m_cpu->materialize_lazy_flags();
        EXPECT_EQ(m_cpu->flags.zf(), a == d);
        EXPECT_EQ(m_cpu->flags.cf(), a < d);
    }
}

// The same edge in protected (32-bit) mode: whatever the 32-bit lowering
// emits for the mid-block jz, both outcomes are architectural, and any B.cond
// that reaches the successor directly reaches its linked entry.
TEST_F(JitJccRetargetTest, ThirtyTwoBitCallerStaysArchitectural) {
    JitLinkGuard link_guard;
    RetargetOn on;
    publish_taken(/*is64=*/false);
    CompiledBlock caller = publish_carried_caller(/*is64=*/false);
    ASSERT_NE(caller.code, nullptr);
    if (m_taken.link_entry() != nullptr) {
        EXPECT_LE(b_conds_to(caller, m_taken.link_entry()).size(), 2u);
    }
    for (const bool carried : {true, false}) {
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        run_and_check(caller, 0, 0, carried);
        run_and_check(caller, 2, 1, carried);
    }
    JitCompiler::bump_link_generation_for_test();
    if (m_taken.link_entry() != nullptr) {
        EXPECT_TRUE(b_conds_to(caller, m_taken.link_entry()).empty());
    }
    run_and_check(caller, 0, 0, true);
}
