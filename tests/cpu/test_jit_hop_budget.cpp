// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — backedge safepoint hop budget on linked edges

#include "jit_exec_test_fixture.hpp"

// ---------------------------------------------------------------------------
// Backedge safepoint (block-link phase 1).
//
// The linked-tail hop budget (ADD W22 / UBFX W4, W22, #12, #1 / CBNZ W4) exists
// so a chain still returns to execute() for the 16 ms GUI pump and so the
// ArenaReadLease is not held indefinitely. It is now emitted only on guest
// edges that do NOT increase the block start RIP, because a chain of purely
// forward edges has a strictly increasing block-start sequence and therefore
// cannot cycle.
// ---------------------------------------------------------------------------

namespace {

// The distinctive word of the hop-budget sequence.
//
// This was CMP W3, #kMaxLinkedHops until the budget was made flag-neutral so
// it would stop clobbering NZCV (and stop forcing a spill/fill of it) on every
// backward hop, then LSR W4, W3, #12 over a JitContext counter. The test word
// tracks the emitter: it is now the bit test on the W22 count.
constexpr u32 kHopBudgetCmp = JitCompiler::kHopCapTestWord;  // UBFX W4, W22, #12, #1

}  // namespace

// TRUE NEGATIVE CONTROL: fails before the change (the budget used to be
// unconditional, so the forward-edge block also carried kHopBudgetCmp).
TEST_F(JitExecTest, ForwardJmpEdgeCarriesNoHopBudget) {
    JitLinkGuard link_guard;
    // 0x1000: JMP +0 -> 0x1002. Target > block start: forward edge.
    const u8 code[] = {0xEB, 0x00};
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_FALSE(code_contains_word(*cb, kHopBudgetCmp))
        << "forward linked tail must not pay the 5-word hop budget";
}

// The fallback stub's L1 probe budgets once per way when its way-1 hit runs
// its own copy of the hit suffix (JitCompiler::l1_way1_fallthrough_enabled),
// and once when way 1 branches back to the way-0 suffix. Either way every
// path through the stub pays exactly one budget.
namespace {
unsigned stub_budget_words() {
    return JitCompiler::l1_way1_fallthrough_enabled() ? 2u : 1u;
}
}  // namespace

// REGRESSION TEST: cannot fail before the change (the budget was always
// there); locks in that backward edges keep it. A patched backward edge pays
// it in its direct-link slot; the unpatched fallback stub keeps its own (one
// per L1 way path, stub_budget_words), so a slotted edge carries one more.
TEST_F(JitExecTest, BackwardJmpEdgeKeepsHopBudget) {
    SharedExitStubsOff legacy_exits;
    JitLinkGuard link_guard;
    // 0x1000: NOP ; JMP -3 -> 0x1000. Target <= block start: backward edge.
    const u8 code[] = {0x90, 0xEB, 0xFD};
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    if (m_jit->ir_emit_active())
        GTEST_SKIP() << "counts the stencil tail's budget words";
    const bool slotted =
        JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::JmpRel) &&
        JitCompiler::backward_link_enabled();
    EXPECT_EQ(count_word(*cb, kHopBudgetCmp),
              (slotted ? 1u : 0u) + stub_budget_words())
        << "backward linked tail must keep the hop budget on every path";
}

// TRUE NEGATIVE CONTROL: a terminal Jcc has two edges; only the backward one
// may carry a budget. Before the change both legs carried one.
TEST_F(JitExecTest, TerminalJccBudgetsOnlyTheBackwardLeg) {
    SharedExitStubsOff legacy_exits;
    JitLinkGuard link_guard;
    // 0x1000: NOP ; JNZ -5 (taken -> 0x0FFE, backward and OUTSIDE the block,
    //                       so it is a chain edge and not an in-block loop;
    //                       fall-through -> 0x1003, forward)
    const u8 code[] = {0x90, 0x75, 0xFB};
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    if (m_jit->ir_emit_active())
        GTEST_SKIP() << "counts the stencil tail's budget words";
    // The backward leg budgets in its slot (when it has one) and on each L1
    // way path of its fallback stub; the forward fall-through leg budgets
    // nowhere.
    const bool slotted =
        JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::TermJcc) &&
        JitCompiler::backward_link_enabled();
    EXPECT_EQ(count_word(*cb, kHopBudgetCmp),
              (slotted ? 1u : 0u) + stub_budget_words())
        << "exactly one of the two Jcc legs is a guest-backward edge";
}

// REGRESSION TEST: a guest backward branch still terminates its chain at
// kMaxLinkedHops instead of spinning forever inside JIT code.
TEST_F(JitExecTest, BackwardChainStillHitsTheHopCap) {
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);

    // 0x1000: INC EAX ; JMP -3 -> 0x1000  (self-chaining backward edge)
    const u8 code[] = {0x40, 0xEB, 0xFD};
    BasicBlock decoded = build_block(code, sizeof(code));
    BasicBlock* cached = cache.insert(decoded);
    ASSERT_NE(cached, nullptr);
    CompiledBlock* cb = m_jit->compile(*cached);
    ASSERT_NE(cb, nullptr);
    publish_compiled(*cached, *cb);
    ASSERT_NE(cache.lookup(0x1000), nullptr);  // seed the L1 slot

    m_cpu->gpr.set32(EAX, 0);
    m_cpu->rip = 0x1000;
    const JitExecResult r = test::execute_like_dispatcher(*m_jit, *cb, *m_cpu);
    EXPECT_EQ(r, JitExecResult::Continue);
    const u32 eax = m_cpu->gpr.get32(EAX);
    EXPECT_GT(eax, 1u) << "the chain never hopped, so the cap proves nothing";
    EXPECT_LE(eax, JitCompiler::kMaxLinkedHops + 1u)
        << "backward chain ran past the hop cap";
    // 1 entry execution + 4094 hops: W22 starts at 1 in the prologue and is
    // bumped before its bit-12 test, so hop number kMaxLinkedHops - 1 itself
    // bails to the dispatcher (kHopCapTestWord).
    EXPECT_EQ(eax, JitCompiler::kMaxLinkedHops - 1u)
        << "hop cap did not trip at exactly kMaxLinkedHops - 1 blocks";
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    m_jit->set_block_cache(nullptr);
}

// The hop budget counts in the chain-wide loop register (W22), which every
// dispatcher entry already sets to 1, instead of a JitContext word: the old
// LDR/ADD/STR round trip was two extra words and a memory RMW on every
// indirect hop. RED before the change (the budget tested W3, loaded from and
// stored back to JitContext::linked_hop_count).
TEST_F(JitExecTest, HopBudgetCountsInTheLoopRegisterNotJitContext) {
    SharedExitStubsOff legacy_exits;
    JitLinkGuard link_guard;
    using namespace vitreo::cpu::jit;
    const u32 bump = arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1);
    ASSERT_EQ(kHopBudgetCmp, arm64::ubfx_w(4, REG_LOOP_CTR, 12, 1));
    ASSERT_EQ(kHopBudgetCmp, 0x530C32C4u);  // llvm-mc: ubfx w4, w22, #12, #1

    struct Form { const char* name; std::vector<u8> code; };
    const Form forms[] = {
        {"backward jmp rel8", {0x90, 0xEB, 0xFD}},
        {"jmp eax", {0xB8, 0x00, 0x20, 0x00, 0x00, 0xFF, 0xE0}},
        {"ret", {0xC3}},
    };
    for (const Form& form : forms) {
        BasicBlock block = build_block(form.code.data(), form.code.size());
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << form.name;
        EXPECT_GE(count_word(*cb, kHopBudgetCmp), 1u) << form.name;
        // Each test word is preceded by the W22 bump it tests.
        for (const WordRange& range : block_word_ranges(*cb)) {
            for (const u32* w = range.begin; w != range.end; ++w) {
                if (*w != kHopBudgetCmp) continue;
                ASSERT_NE(w, range.begin) << form.name;
                EXPECT_EQ(w[-1], bump) << form.name;
            }
        }
    }
}

// REGRESSION TEST: an indirect-only chain (JMP reg back to its own block
// through the L1) stops at the same exact count as the direct one.
TEST_F(JitExecTest, IndirectChainStillHitsTheHopCap) {
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);

    // 0x1000: INC EAX ; JMP EBX  (EBX = 0x1000)
    const u8 code[] = {0x40, 0xFF, 0xE3};
    BasicBlock decoded = build_block(code, sizeof(code));
    BasicBlock* cached = cache.insert(decoded);
    ASSERT_NE(cached, nullptr);
    CompiledBlock* cb = m_jit->compile(*cached);
    ASSERT_NE(cb, nullptr);
    publish_compiled(*cached, *cb);
    ASSERT_NE(cache.lookup(0x1000), nullptr);  // seed the L1 slot

    m_cpu->gpr.set32(EAX, 0);
    m_cpu->gpr.set32(EBX, 0x1000);
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *cb, *m_cpu), JitExecResult::Continue);
    // Same count as the direct chain: the entry and 4094 hops.
    EXPECT_EQ(m_cpu->gpr.get32(EAX), JitCompiler::kMaxLinkedHops - 1u)
        << "indirect chain did not stop at exactly kMaxLinkedHops - 1 blocks";
    EXPECT_EQ(m_cpu->rip, 0x1000u);
    m_jit->set_block_cache(nullptr);
}

// REGRESSION TEST: in-block loop iterations share W22 with the hop budget.
// Whatever count a loop leaves behind, a following indirect self-chain still
// returns to the dispatcher within kMaxLinkedHops + 1 hops. The loop leaves
// W22 = trips; 8191 is the worst case (the next 4096 values have bit 12
// clear), so EAX = 1 entry + 4096 hops there.
TEST_F(JitExecTest, HopsAfterAnInBlockLoopStayBounded) {
    JitLinkGuard link_guard;
    // 0x1000: INC EDX ; DEC ECX ; JNZ 0x1000   (in-block loop)
    // 0x1004: INC EAX ; JMP EBX                (EBX = 0x1004, indirect)
    const u8 loop[] = {0x42, 0x49, 0x75, 0xFC};
    const u8 hop[] = {0x40, 0xFF, 0xE3};
    for (const u32 trips : {3000u, 4200u, 8191u, 8500u, 20000u}) {
        BlockCache cache;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(&cache);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, loop, sizeof(loop)));
        ASSERT_TRUE(m_memory->write_bytes(0x1004, hop, sizeof(hop)));
        CompiledBlock entry{};
        for (const GuestAddr address : {GuestAddr{0x1004}, GuestAddr{0x1000}}) {
            const u8* bytes = address == 0x1000 ? loop : hop;
            const usize len = address == 0x1000 ? sizeof(loop) : sizeof(hop);
            BasicBlock decoded = build_block(bytes, len);
            decoded.start_addr = address;
            BasicBlock* cached = cache.insert(std::move(decoded));
            ASSERT_NE(cached, nullptr);
            CompiledBlock* cb = m_jit->compile(*cached);
            ASSERT_NE(cb, nullptr);
            m_jit->publish_compiled_block(*cached, *cb);
            ASSERT_NE(cache.lookup(address), nullptr);
            entry = compiled_from_cache(*cached);
        }
        m_cpu->gpr.set32(EAX, 0);
        m_cpu->gpr.set32(ECX, trips);
        m_cpu->gpr.set32(EDX, 0);
        m_cpu->gpr.set32(EBX, 0x1004);
        m_cpu->rip = 0x1000;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, entry, *m_cpu), JitExecResult::Continue)
            << trips;
        EXPECT_GE(m_cpu->gpr.get32(EAX), 1u) << trips;
        EXPECT_LE(m_cpu->gpr.get32(EAX), JitCompiler::kMaxLinkedHops + 1u)
            << trips << " loop trips: the hop chain ran past the cap";
        m_jit->set_block_cache(nullptr);
    }
}

// REGRESSION TEST: publishing a caller resolves only ITS edges. It used to
// re-patch every recorded site into each of its targets -- quadratic in a
// popular callee's caller count, two W^X flips and two icache flushes per
// site -- which stalled compiling threads long enough to delay the Steam
// client UI's first present by ~30 s once CALL rel sites were linked.
TEST_F(JitExecTest, PublishingACallerPatchesOnlyItsOwnSites) {
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::CallRel)) {
        GTEST_SKIP() << "CALL rel direct links are disabled";
    }
    auto publish = [&](GuestAddr address, const u8* code, usize len) {
        BasicBlock decoded = build_block64_at(address, code, len);
        BasicBlock* cached = cache.insert(std::move(decoded));
        ASSERT_NE(cached, nullptr);
        CompiledBlock* cb = m_jit->compile(*cached);
        ASSERT_NE(cb, nullptr);
        m_jit->publish_compiled_block(*cached, *cb);
    };
    const u8 target[] = {0xC3};  // 0x2000: ret
    publish(0x2000, target, sizeof(target));
    for (GuestAddr caller = 0x1000; caller < 0x1000 + 8 * 0x40; caller += 0x40) {
        // call 0x2000
        const i32 rel = static_cast<i32>(0x2000 - (caller + 5));
        const u8 call[] = {0xE8, static_cast<u8>(rel), static_cast<u8>(rel >> 8),
                           static_cast<u8>(rel >> 16), static_cast<u8>(rel >> 24)};
        const u64 before = stats::link_patches_performed();
        publish(caller, call, sizeof(call));
        EXPECT_EQ(stats::link_patches_performed() - before, 1u)
            << "caller 0x" << std::hex << caller
            << " re-patched other callers' sites";
    }
    m_jit->set_block_cache(nullptr);
}

// REGRESSION TEST: a mid-block Jcc exit keeps its own direct-link slot when
// the block's terminal carries one too. Mid-block exits are flushed after the
// terminal tail, and a "no slot yet" gate left from the single-slot design
// handed the block's only slot to the terminal: every taken mid-block Jcc of
// a trace ending in a JMP rel (and, with CALL rel links, in a CALL) paid the
// L1 probe instead.
TEST_F(JitExecTest, MidBlockJccKeepsItsSlotBesideATerminalSlot) {
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::MidJcc)) {
        GTEST_SKIP() << "mid-block Jcc direct links are disabled";
    }
    auto publish = [&](GuestAddr address, const u8* code, usize len) {
        BasicBlock decoded = build_block64_at(address, code, len);
        BasicBlock* cached = cache.insert(std::move(decoded));
        EXPECT_NE(cached, nullptr);
        CompiledBlock* cb = cached ? m_jit->compile(*cached) : nullptr;
        EXPECT_NE(cb, nullptr);
        if (cb) m_jit->publish_compiled_block(*cached, *cb);
        return cb ? compiled_from_cache(*cached) : CompiledBlock{};
    };
    const u8 callee[] = {0xC3};                                // 0x2000: ret
    const u8 taken[] = {0xBB, 0x07, 0x00, 0x00, 0x00,          // 0x1100: mov ebx, 7
                        0xE9, 0xF6, 0x00, 0x00, 0x00};         //         jmp 0x1200
    publish(0x2000, callee, sizeof(callee));
    publish(0x1100, taken, sizeof(taken));
    struct Case {
        u8 terminal;                    // E9 jmp rel32 / E8 call rel32
        JitCompiler::LinkEdge edge;
        GuestAddr at;
    };
    for (const Case c : {Case{0xE9, JitCompiler::LinkEdge::JmpRel, 0x1000},
                         Case{0xE8, JitCompiler::LinkEdge::CallRel, 0x1040}}) {
        SCOPED_TRACE(c.terminal == 0xE9 ? "terminal jmp" : "terminal call");
        // test eax, eax ; jz 0x1100 ; jmp/call 0x2000
        const i32 jz = static_cast<i32>(0x1100 - (c.at + 8));
        const i32 rel = static_cast<i32>(0x2000 - (c.at + 13));
        const u8 caller[] = {
            0x85, 0xC0,
            0x0F, 0x84, static_cast<u8>(jz), static_cast<u8>(jz >> 8),
            static_cast<u8>(jz >> 16), static_cast<u8>(jz >> 24),
            c.terminal, static_cast<u8>(rel), static_cast<u8>(rel >> 8),
            static_cast<u8>(rel >> 16), static_cast<u8>(rel >> 24)};
        const u64 before = stats::link_patches_performed();
        CompiledBlock entry = publish(c.at, caller, sizeof(caller));
        const u64 terminal_slot = JitCompiler::link_edge_enabled(c.edge) ? 1 : 0;
        EXPECT_EQ(stats::link_patches_performed() - before, 1u + terminal_slot)
            << "the mid-block jz lost its slot to the terminal";

        // The patched taken edge still lands on 0x1100 and runs it.
        m_cpu->gpr.set64(EAX, 0);
        m_cpu->gpr.set64(EBX, 0);
        m_cpu->rip = c.at;
        ASSERT_EQ(test::execute_like_dispatcher(*m_jit, entry, *m_cpu), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get64(EBX), 7u);
        EXPECT_EQ(m_cpu->rip, 0x1200u);
    }
    m_jit->set_block_cache(nullptr);
}

// REGRESSION TEST: the cap is a threshold, not an exact match. A patched
// backward slot that reaches the cap falls through to its counted stub, which
// counts once more; an equality test (EOR #0xFFF + CBZ) then never matched
// again and the chain ran unbounded -- here, all 100000 guest iterations
// without one return to execute().
TEST_F(JitExecTest, PatchedBackwardSlotStillStopsAtTheHopCap) {
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::JmpRel) ||
        !JitCompiler::backward_link_enabled()) {
        GTEST_SKIP() << "backward JMP rel links are disabled";
    }

    // 0x1000: inc eax ; jmp 0x1010                        (forward)
    // 0x1010: cmp eax, 100000 ; jae 0x1020 ; jmp 0x1000   (backward slot)
    const u8 head[] = {0x40, 0xEB, 0x0D};
    const u8 tail[] = {0x3D, 0xA0, 0x86, 0x01, 0x00, 0x73, 0x09, 0xEB, 0xE7};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, head, sizeof(head)));
    ASSERT_TRUE(m_memory->write_bytes(0x1010, tail, sizeof(tail)));
    CompiledBlock entry{};
    for (const GuestAddr address : {GuestAddr{0x1010}, GuestAddr{0x1000}}) {
        const u8* code = address == 0x1000 ? head : tail;
        const usize len = address == 0x1000 ? sizeof(head) : sizeof(tail);
        BasicBlock decoded = build_block(code, len);
        decoded.start_addr = address;
        BasicBlock* cached = cache.insert(std::move(decoded));
        ASSERT_NE(cached, nullptr);
        CompiledBlock* cb = m_jit->compile(*cached);
        ASSERT_NE(cb, nullptr);
        m_jit->publish_compiled_block(*cached, *cb);
        ASSERT_NE(cache.lookup(address), nullptr);
        entry = compiled_from_cache(*cached);
    }

    m_cpu->gpr.set32(EAX, 0);
    m_cpu->rip = 0x1000;
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, entry, *m_cpu), JitExecResult::Continue);
    const u32 eax = m_cpu->gpr.get32(EAX);
    EXPECT_GT(eax, 1u) << "the chain never hopped, so the cap proves nothing";
    EXPECT_LE(eax, JitCompiler::kMaxLinkedHops + 1u)
        << "the chain ran past the hop cap";
    m_jit->set_block_cache(nullptr);
}

// REGRESSION TEST: a forward chain has no budget at all, so it must terminate
// on its own — it does, because block start RIPs strictly increase and the
// last block's target is simply not resident.
TEST_F(JitExecTest, ForwardChainTerminatesWithoutABudget) {
    JitLinkGuard link_guard;
    BlockCache cache;
    m_jit->set_block_cache(&cache);

    // Three blocks, each INC EAX then JMP forward to the next.
    //   0x1000: INC EAX ; JMP +0 -> 0x1003
    //   0x1003: INC EAX ; JMP +0 -> 0x1006
    //   0x1006: INC EAX ; JMP +0 -> 0x1009   (0x1009 not compiled -> exit)
    const u8 code[] = {0x40, 0xEB, 0x00, 0x40, 0xEB, 0x00, 0x40, 0xEB, 0x00};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

    CompiledBlock first{};
    for (int i = 0; i < 3; ++i) {
        const GuestAddr addr = 0x1000 + static_cast<GuestAddr>(i) * 3;
        BasicBlock b;
        b.start_addr = addr;
        MemoryByteReader reader(code + i * 3, 3);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode32, inst)
                != DecodeError::Success) {
                break;
            }
            b.instructions.push_back(inst);
        }
        b.byte_length = static_cast<u32>(reader.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        BasicBlock* cached = cache.insert(b);
        ASSERT_NE(cached, nullptr);
        CompiledBlock* cb = m_jit->compile(*cached);
        ASSERT_NE(cb, nullptr);
        // No budget anywhere in this chain: every edge is forward.
        EXPECT_FALSE(code_contains_word(*cb, kHopBudgetCmp))
            << "forward-only block at 0x" << std::hex << addr;
        publish_compiled(*cached, *cb);
        ASSERT_NE(cache.lookup(addr), nullptr);
        // compile() returns a pointer into a vector that later compiles may
        // reallocate — take a copy of the entry block.
        if (i == 0) first = *cb;
    }

    m_cpu->gpr.set32(EAX, 0);
    m_cpu->rip = 0x1000;
    const JitExecResult r = test::execute_like_dispatcher(*m_jit, first, *m_cpu);
    EXPECT_EQ(r, JitExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(EAX), 3u)
        << "all three forward-linked blocks should have run exactly once";
    EXPECT_EQ(m_cpu->rip, 0x1009u);
    m_jit->set_block_cache(nullptr);
}

// TRUE NEGATIVE CONTROL for the NZCV half of the change: a forward linked
// tail no longer spills/fills NZCV around the L1 probe, because the probe's
// start_addr compare is now EOR/CBNZ instead of SUBS/B.NE. If any word of
// that tail clobbers PSTATE, the flags CMP left for the target block are
// destroyed and SETZ reads garbage. (Before the change this passed via the
// spill/fill; it fails immediately if the spill is dropped without also
// making the probe flag-neutral, which is exactly the bug being guarded.)
TEST_F(JitExecTest, ForwardHopPreservesLiveNzcvAcrossTheChain) {
    for (u32 eax : {4u, 5u, 6u}) {
        JitLinkGuard link_guard;
        BlockCache cache;
        m_jit->set_block_cache(&cache);

        // 0x1000: CMP EAX, 5 ; JMP +0 -> 0x1005
        // 0x1005: SETZ BL     ; (block ends)
        const u8 code[] = {0x83, 0xF8, 0x05, 0xEB, 0x00, 0x0F, 0x94, 0xC3};
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

        const GuestAddr addrs[2] = {0x1000, 0x1005};
        const usize lens[2] = {5, 3};
        CompiledBlock first{};
        for (int i = 0; i < 2; ++i) {
            BasicBlock b;
            b.start_addr = addrs[i];
            MemoryByteReader reader(code + (i == 0 ? 0 : 5), lens[i]);
            while (reader.remaining() > 0) {
                DecodedInstruction inst;
                if (decode_instruction(reader, DecodeMode::Mode32, inst)
                    != DecodeError::Success) {
                    break;
                }
                b.instructions.push_back(inst);
            }
            b.byte_length = static_cast<u32>(reader.position());
            b.instruction_count = static_cast<u32>(b.instructions.size());
            BasicBlock* cached = cache.insert(b);
            ASSERT_NE(cached, nullptr);
            CompiledBlock* cb = m_jit->compile(*cached);
            ASSERT_NE(cb, nullptr);
            publish_compiled(*cached, *cb);
            ASSERT_NE(cache.lookup(addrs[i]), nullptr);
            if (i == 0) first = *cb;
        }

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, 0xFFFFFF00u);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, first, *m_cpu), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get32(EBX) & 0xFFu, eax == 5 ? 1u : 0u)
            << "SETZ after a forward chain hop, eax=" << eax;
        m_jit->set_block_cache(nullptr);
    }
}

// TRUE NEGATIVE CONTROL for the forward tail's FALLBACK path: when the L1
// probe misses, the flags must still reach CpuState. The forward tail never
// spills to [SP,#8], so emit_persist_live_nzcv has to MRS them out of PSTATE.
// Reading the (never written) spill slot instead yields stack garbage.
TEST_F(JitExecTest, ForwardTailMissPersistsLiveNzcv) {
    for (u32 eax : {4u, 5u}) {
        JitLinkGuard link_guard;
        // No block cache: the L1 base is null, so the tail always falls back.
        const u8 a[] = {0x83, 0xF8, 0x05, 0xEB, 0x00};  // CMP EAX,5 ; JMP 0x1005
        const u8 b[] = {0x0F, 0x94, 0xC3};              // SETZ BL

        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->gpr.set32(EAX, eax);
        m_cpu->gpr.set32(EBX, 0xFFFFFF00u);
        m_cpu->rip = 0x1000;
        EXPECT_EQ(jit_run(a, sizeof(a)), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->rip, 0x1005u);

        BasicBlock blk = build_block(b, sizeof(b));
        blk.start_addr = 0x1005;
        CompiledBlock* cb = m_jit->compile(blk);
        ASSERT_NE(cb, nullptr);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *cb, *m_cpu), JitExecResult::Continue);
        EXPECT_EQ(m_cpu->gpr.get32(EBX) & 0xFFu, eax == 5 ? 1u : 0u)
            << "SETZ after a missed forward tail, eax=" << eax;
    }
}
