// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: control flow. JMP/Jcc/CALL rel,
// RET and RET imm16, JMP/CALL r/m64 and far JMP/CALL m16:x, together
// with the x64 ABI call-alignment diagnostic (VITREO_ABI_ALIGN_TRAP)
// that the two CALL lowerings feed.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// Microsoft x64 ABI: RSP is 16-byte aligned at every call site, so after a
// CALL pushes its 8-byte return address the callee sees RSP ≡ 8 (mod 16). A
// guest whose stack is shifted off that faults on every aligned XMM spill its
// compiler placed at a fixed frame offset, and the fault surfaces arbitrarily
// far from the instruction that lost the alignment. `new_rsp` is the value the
// push commits and cpu->rip is the CALL itself, so the first violation names
// the exact call site. Reported once per thread; emitted only under
// VITREO_ABI_ALIGN_TRAP so ordinary codegen pays nothing.
static bool abi_align_trap_enabled() {
    static const bool enabled = std::getenv("VITREO_ABI_ALIGN_TRAP") != nullptr;
    return enabled;
}

// Stack-probe and cookie-check helpers (__chkstk, __security_check_cookie,
// _alloca_probe) are deliberately exempt from the alignment rule: MSVC calls
// them from inside a prologue before the frame is aligned, and their RET
// restores whatever the caller had. Reporting distinct call sites with the
// target address lets those be told apart from a genuine persistent shift.
// Per-callee tally of the entry alignment its callers produce. A callee that is
// only ever reached on one parity is being called through a private convention
// (MSVC's __chkstk, or a managed-runtime trampoline that pops its own return address and
// therefore wants the opposite parity); a callee reached on BOTH parities has
// at least one caller whose frame is wrong, and that is the real signal.
namespace {
struct CallAlignTally {
    std::atomic<u64> target{0};
    std::atomic<u64> entry_mod8{0};   // ABI-standard: rsp ≡ 8 after the push
    std::atomic<u64> entry_mod0{0};
};
// Open addressing over every callee seen, not just the suspects: deciding that
// a callee is reached on both parities requires having counted its correct
// entries too, and those overwhelmingly happen before the first bad one.
constexpr u32 kCallAlignSlots = 1u << 16;
constexpr u32 kCallAlignProbe = 16;
CallAlignTally g_call_align[kCallAlignSlots];

CallAlignTally* find_call_align_slot(u64 target) {
    u64 h = target * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    for (u32 probe = 0; probe < kCallAlignProbe; ++probe) {
        auto& slot = g_call_align[(h + probe) & (kCallAlignSlots - 1)];
        u64 owner = slot.target.load(std::memory_order_relaxed);
        if (owner == target) return &slot;
        if (owner == 0) {
            u64 expected = 0;
            if (slot.target.compare_exchange_strong(expected, target,
                                                    std::memory_order_relaxed) ||
                expected == target) {
                return &slot;
            }
        }
    }
    return nullptr;
}

// Returns true when this misaligned entry is the anomalous kind: the same
// callee has already been entered on the ABI-standard parity, so the two
// callers cannot both be right and the frame of this one is wrong.
bool note_call_alignment(u64 target, bool standard) {
    auto* slot = find_call_align_slot(target);
    if (slot == nullptr) return false;
    auto& counter = standard ? slot->entry_mod8 : slot->entry_mod0;
    counter.fetch_add(1, std::memory_order_relaxed);
    if (standard) return false;
    return slot->entry_mod8.load(std::memory_order_relaxed) != 0;
}
}  // namespace

extern "C" void jit_dump_call_alignment_tally() {
    for (u32 i = 0; i < kCallAlignSlots; ++i) {
        const u64 target = g_call_align[i].target.load(std::memory_order_relaxed);
        if (target == 0) continue;
        const u64 standard = g_call_align[i].entry_mod8.load(
            std::memory_order_relaxed);
        const u64 flipped = g_call_align[i].entry_mod0.load(
            std::memory_order_relaxed);
        if (standard == 0 || flipped == 0) continue;  // consistent convention
        std::fprintf(stderr,
            "[abi-align] MIXED callee=0x%llx entry_mod16_8=%llu mod16_0=%llu\n",
            static_cast<unsigned long long>(target),
            static_cast<unsigned long long>(standard),
            static_cast<unsigned long long>(flipped));
    }
}

extern "C" void jit_check_call_alignment(CpuState* cpu, GuestMemory* mem,
                                         u64 new_rsp, u64 target) noexcept {
    const bool anomalous = note_call_alignment(target, (new_rsp & 15u) == 8u);
    if (cpu == nullptr || !anomalous) return;
    static constexpr u32 kMaxReports = 24;
    static thread_local u64 reported[kMaxReports]{};
    static thread_local u32 reported_count = 0;
    for (u32 i = 0; i < reported_count; ++i) {
        if (reported[i] == cpu->rip) return;
    }
    if (reported_count >= kMaxReports) return;
    reported[reported_count++] = cpu->rip;
    // Dump per distinct callee rather than per call site: one runtime-generated
    // helper is reached from dozens of call sites, and it is the callee's frame
    // handling that says whether the alignment was meant to be restored.
    static constexpr u32 kMaxDumped = 4;
    static thread_local u64 dumped[kMaxDumped]{};
    static thread_local u32 dumped_count = 0;
    bool first = true;
    for (u32 i = 0; i < dumped_count; ++i) {
        if (dumped[i] == target) { first = false; break; }
    }
    if (first && dumped_count < kMaxDumped) {
        dumped[dumped_count++] = target;
    } else {
        first = false;
    }
    std::fprintf(stderr,
        "[abi-align] VIOLATION x64 CALL at rip=0x%llx target=0x%llx pushes "
        "return address leaving rsp=0x%llx (mod16=%llu, expected 8)\n",
        static_cast<unsigned long long>(cpu->rip),
        static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(new_rsp),
        static_cast<unsigned long long>(new_rsp & 15u));
    if (!first || mem == nullptr) return;
    // Runtime-generated callees have no image on disk to disassemble, and the
    // frame that lost the alignment is above this one, so the first violation
    // dumps the callee's entry bytes and the live return-address chain.
    const auto dump_bytes = [&](const char* what, u64 addr, u32 count) {
        char line[3 * 32 + 1];
        for (u32 offset = 0; offset < count; offset += 32) {
            u32 n = 0;
            u32 got = 0;
            for (; got < 32 && offset + got < count; ++got) {
                auto b = mem->read_u8(static_cast<GuestAddr>(addr + offset + got));
                if (!b.is_ok()) break;
                n += static_cast<u32>(std::snprintf(line + n, sizeof(line) - n,
                                                   "%02x ", b.value()));
            }
            if (got == 0) break;
            std::fprintf(stderr, "[abi-align]   %s+0x%02x %s\n", what, offset,
                         line);
        }
    };
    dump_bytes("callee", target, 64);
    dump_bytes("callsite", cpu->rip, 32);
    for (u32 i = 0; i < 24; ++i) {
        auto v = mem->read_u64(static_cast<GuestAddr>(new_rsp + i * 8));
        if (!v.is_ok()) break;
        std::fprintf(stderr, "[abi-align]   stack+0x%03x %016llx\n", i * 8,
                     static_cast<unsigned long long>(v.value()));
    }
}

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_control64(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip,
    bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // Direct-target control flow — 64-bit
    //
    // These end the block. Linked tail-continue chains directly into the
    // target's compiled prologue (Stage B1) without round-tripping through
    // the dispatcher. Falls back to the standard epilogue on cache miss.
    // ========================================================================

    case Opcode::JMP_REL8:
    case Opcode::JMP_REL32: {
        u64 target = next_rip + static_cast<u64>(static_cast<i64>(op0.imm));
        // Intel SDM Vol. 2 JMP/CALL/Jcc: a non-canonical target is #GP(0) at
        // the branch with nothing committed (noncanonical_branch_probe). The
        // target is a translation-time constant here, so such a branch is
        // left to the interpreter, which raises it exactly.
        if (static_cast<u64>(static_cast<i64>(target << 16) >> 16) != target) {
            return false;
        }
        u32 ji_off = jit_iters_offset();
        emit_update_rip(target);
        emit_tail_link_or_exit(target, NzcvState::None, LinkEdge::JmpRel);
        return true;
    }

    case Opcode::JZ_REL8:  case Opcode::JZ_REL32:
    case Opcode::JNZ_REL8: case Opcode::JNZ_REL32:
    case Opcode::JB_REL8:  case Opcode::JB_REL32:
    case Opcode::JNB_REL8: case Opcode::JNB_REL32:
    case Opcode::JBE_REL8: case Opcode::JBE_REL32:
    case Opcode::JNBE_REL8: case Opcode::JNBE_REL32:
    case Opcode::JL_REL8:  case Opcode::JL_REL32:
    case Opcode::JNL_REL8: case Opcode::JNL_REL32:
    case Opcode::JLE_REL8: case Opcode::JLE_REL32:
    case Opcode::JNLE_REL8: case Opcode::JNLE_REL32:
    case Opcode::JS_REL8:  case Opcode::JS_REL32:
    case Opcode::JNS_REL8: case Opcode::JNS_REL32:
    case Opcode::JO_REL8:  case Opcode::JO_REL32:
    case Opcode::JNO_REL8: case Opcode::JNO_REL32:
    case Opcode::JP_REL8:  case Opcode::JP_REL32:
    case Opcode::JNP_REL8: case Opcode::JNP_REL32: {
        u8 cond = jcc_condition_code(instr.opcode);
        if (cond == 0xFF) return false;

        u64 target = next_rip + static_cast<u64>(static_cast<i64>(op0.imm));
        // Intel SDM Vol. 2 JMP/CALL/Jcc: a non-canonical target is #GP(0) at
        // the branch with nothing committed (noncanonical_branch_probe). The
        // target is a translation-time constant here, so such a branch is
        // left to the interpreter, which raises it exactly.
        if (static_cast<u64>(static_cast<i64>(target << 16) >> 16) != target) {
            return false;
        }

        // When the immediately preceding instruction left live ARM64 flags for
        // this condition, read them directly. Otherwise call the cond fn:
        // X0 = CpuState* ; X16 = cond_fns[cond] ; BLR X16. The helper
        // rematerializes the condition from lazy flag state and stays the
        // reference for conditions ARM64 cannot express (parity) and whenever
        // NZCV did not survive. CSET is the first emit, so a mid-trace
        // forward Jcc can consume NZCV before the taken-path stores clobber it.
        const int fused = fused_arm_cond(m_flags.incoming_nzcv, cond);
        // Block-start backedges enter with the same conservative metadata as
        // the original prologue. Arbitrary mid-block backedges need separate
        // loop-header flag-state analysis before they can safely bypass their
        // statically compiled predecessor.
        const bool is_loop_back = target == m_block_start_addr;
        if (!is_last && !is_loop_back) {
            // The internal edge has no cold stub, so it may only be taken
            // when nothing is owed on the taken path: either the predecessor
            // deferred no lazy-flag store at all, or liveness discharged it
            // with the internal-join proof. Taking it while a deferred record
            // is outstanding and unproved drops that store on the taken edge
            // (stale AF/PF), which is what wedges a launcher UI at startup.
            if (fused >= 0 &&
                (!m_incoming_stub_store.valid || m_internal_join_jcc_current) &&
                is_forward_in_block_target(target, next_rip)) {
                queue_forward_in_block_jcc(static_cast<u8>(fused), target);
                // Liveness proved the deferred record dead at a non-faulting
                // writer reached by both paths, so this internal edge has no
                // cold stub on which to replay it. m_internal_join_jcc_current
                // is that proof: without it the predecessor's omitted store
                // still needs the cold exit below, and dropping it here loses
                // AF/PF on the taken edge. That mismatch is what wedges a launcher
                // UI (it stalls before opening its loopback websocket).
                m_incoming_stub_store = PendingStubStore{};
                m_flags.nzcv = m_flags.incoming_nzcv;
                vitreo::diagnostics::perf_note_jcc_emit_fused();
                return true;
            }
            queue_mid_block_jcc_exit(fused, cond, target);
            return true;
        }

        const bool fused_jcc = fused >= 0;
        // Jcc-first block: the predecessor published its NZCV plus a runtime
        // nzcv_kind byte. When that byte is nonzero the flags in PSTATE are
        // the ones this Jcc must read, so branch on them natively instead of
        // rematerializing through the cond helper. kind == 0 (no publisher, a
        // CF/partial-flag overlay, or a Sub8 zero-extended SUBS) falls into
        // the lazy path, which is the semantic reference. Same contract and
        // same guards as the 32-bit lowering in ops/control.cpp.
        const bool jcc_first_carried =
            !fused_jcc && !is_loop_back && can_try_carried_jcc(cond);
        const int carried_ac = jcc_first_carried
            ? fused_arm_cond(NzcvState::Sub, cond, KindGuarded{}) : -1;
        NzcvState jcc_carry = NzcvState::None;
        if (fused_jcc) {
            jcc_carry = m_flags.incoming_nzcv;
            m_flags.nzcv = m_flags.incoming_nzcv;
            vitreo::diagnostics::perf_note_jcc_emit_fused();
        } else if (jcc_first_carried && carried_ac >= 0) {
            jcc_carry = NzcvState::Carried;
            m_flags.nzcv = NzcvState::Carried;
            vitreo::diagnostics::perf_note_jcc_emit_carried();
        } else {
            jcc_carry = emit_eval_jcc(cond, fused);
            vitreo::diagnostics::perf_note_jcc_emit_helper();
        }

        if (is_loop_back) {
            // Intel SDM Vol. 2A, Jcc: a true condition transfers to the
            // relative target without changing flags. Keep backward x64
            // branches native, but use the same bounded-dispatch contract as
            // the 32-bit lowering so the runtime regains control regularly.
            usize skip_pos = m_code_buf.size();
            emit(0);

            emit(arm64::movz_w(REG_SCRATCH1,
                               static_cast<u16>(MAX_IN_BLOCK_LOOP_ITERS)));
            emit(arm64::subs_w(31, REG_LOOP_CTR, REG_SCRATCH1));
            const usize bcs_pos = m_code_buf.size();
            emit(0);
            emit(arm64::add_w_imm(REG_LOOP_CTR, REG_LOOP_CTR, 1));
            // The cap check uses host SUBS. A loop header may immediately
            // read guest flags (ADC/SBB/Jcc), so force that entry to consult
            // the architectural lazy/RFLAGS state rather than cap NZCV.
            emit_store_nzcv_kind(NzcvState::None);
            i32 loop_off = static_cast<i32>(m_body_start_pos
                         - m_code_buf.size()) * 4;
            emit(arm64::b_imm(loop_off));

            const usize cap_exit_pos = m_code_buf.size();
            m_code_buf[bcs_pos] = arm64::b_cond(
                arm64::CS, static_cast<i32>(cap_exit_pos - bcs_pos) * 4);
            u32 ji_off = jit_iters_offset();
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
            emit_update_rip(instr_rip);
            emit_epilogue(JitExecResult::Continue);

            const usize nt_pos = m_code_buf.size();
            const i32 skip_off = static_cast<i32>(nt_pos - skip_pos) * 4;
            if (fused_jcc) {
                m_code_buf[skip_pos] = arm64::b_cond(
                    static_cast<u8>(fused) ^ 1u, skip_off);
            } else {
                m_code_buf[skip_pos] = arm64::cbz_w(0, skip_off);
            }

            emit_update_rip(next_rip);
            emit_tail_link_or_exit(next_rip, jcc_carry, LinkEdge::TermFall);
            return true;
        }

        {
            usize kind_cbz = static_cast<usize>(-1);
            if (jcc_first_carried && carried_ac >= 0) {
                emit(arm64::ldrb_uoff(1, REG_CPU_STATE, nzcv_kind_offset()));
                kind_cbz = m_code_buf.size();
                emit(0);  // CBZ W1, .lazy
            }

            usize skip_pos = m_code_buf.size();
            emit(0);

            // Taken leg
            const usize taken_pos = m_code_buf.size();
            u32 ji_off = jit_iters_offset();
            emit_update_rip(target);
            emit_tail_link_or_exit(target, jcc_carry, LinkEdge::TermJcc);

            // .lazy — no publisher NZCV (or an overlay/Sub8 kind of 0).
            // Rematerialize the condition from the architectural lazy/RFLAGS
            // state and republish kind 0 so both outgoing edges agree.
            usize lazy_cbnz = static_cast<usize>(-1);
            if (kind_cbz != static_cast<usize>(-1)) {
                const usize lazy = m_code_buf.size();
                m_code_buf[kind_cbz] = arm64::cbz_w(
                    1, static_cast<i32>(lazy - kind_cbz) * 4);
                emit_eval_lazy_condition(cond);
                emit_store_nzcv_kind(NzcvState::None);
                lazy_cbnz = m_code_buf.size();
                emit(0);  // CBNZ W0, .taken
            }

            // .not_taken (fall through)
            usize nt_pos = m_code_buf.size();
            i32 skip_off = static_cast<i32>(nt_pos - skip_pos) * 4;
            if (fused_jcc) {
                m_code_buf[skip_pos] = arm64::b_cond(
                    static_cast<u8>(fused) ^ 1u, skip_off);
            } else if (kind_cbz != static_cast<usize>(-1)) {
                m_code_buf[skip_pos] = arm64::b_cond(
                    static_cast<u8>(carried_ac) ^ 1u, skip_off);
                m_code_buf[lazy_cbnz] = arm64::cbnz_w(
                    0, static_cast<i32>(taken_pos - lazy_cbnz) * 4);
            } else {
                m_code_buf[skip_pos] = arm64::cbz_w(0, skip_off);
            }

            emit_update_rip(next_rip);
            emit_tail_link_or_exit(next_rip, jcc_carry, LinkEdge::TermFall);
        }
        return true;
    }

    case Opcode::CALL_REL32: {
        u64 target = next_rip + static_cast<u64>(static_cast<i64>(op0.imm));
        // Intel SDM Vol. 2 JMP/CALL/Jcc: a non-canonical target is #GP(0) at
        // the branch with nothing committed (noncanonical_branch_probe). The
        // target is a translation-time constant here, so such a branch is
        // left to the interpreter, which raises it exactly.
        if (static_cast<u64>(static_cast<i64>(target << 16) >> 16) != target) {
            return false;
        }
        // Push 8-byte return address: RSP -= 8 ; [RSP] = next_rip
        const u8 rsp_index = static_cast<u8>(RegIndex::RSP);
        const bool pinned_rsp = m_pins_live &&
            m_pin_arm[rsp_index] != 0xFF;
        const u8 old_rsp_reg = pinned_rsp ? m_pin_arm[rsp_index]
                                          : REG_SCRATCH2;
        if (!pinned_rsp) emit_load_gpr64(old_rsp_reg, rsp_index);
        // Keep architectural RSP unchanged until the fault-capable stack
        // write retires. Intel SDM Vol. 2 PUSH/CALL: a stack-write fault
        // aborts without committing RSP. On Windows that fault is how the
        // stack grows — the guard page is committed and the instruction
        // retries. A software NT_TIB.StackLimit compare that raises
        // STATUS_STACK_OVERFLOW before the store prevents that growth.
        emit(arm64::sub_x_imm(REG_CALL_TMP, old_rsp_reg, 8));
        // A fault on the push needs no published RIP here: the store's cold
        // stub publishes this instruction's RIP before its helper can raise,
        // and a hardware fault recovers it from the host-PC map. Only the
        // alignment diagnostic below reads cpu->rip as the CALL site.
        if (abi_align_trap_enabled()) emit_update_rip(instr_rip);
        if (abi_align_trap_enabled()) {
            emit(arm64::mov_x(2, REG_CALL_TMP));
            emit_mov_imm64(3, static_cast<i64>(target));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                                  &jit_check_call_alignment)));
            emit_helper_blr();
        }
        emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(next_rip));
        emit_guest_store64(REG_SCRATCH0, REG_CALL_TMP);
        if (pinned_rsp) {
            emit(arm64::mov_x(m_pin_arm[rsp_index], REG_CALL_TMP));
            m_esp_frame_live = false;
        } else {
            emit_store_gpr64(REG_CALL_TMP, rsp_index);
        }
        // No emit_rsb_push here, unlike the 32-bit CALL lowering. Wiring the
        // software return-address stack + sticky RET ring into the x64 path
        // was measured on tools/jit_bench call_ret: 50ns -> 70ns for the RET
        // sticky/RSB probe alone, 79ns with the matching pushes. The x64 RET
        // tail already reaches its target through L1 at a 97-100% direct-link
        // ratio, so the probes and the per-hop MRU ring write are pure added
        // latency in front of a lookup that was already hitting.

        u32 ji_off = jit_iters_offset();
        if (emit_linked_call_continue(target, next_rip)) {
            return true;
        }
        emit_update_rip(target);
        emit_tail_link_or_exit(target, NzcvState::None, LinkEdge::CallRel);
        return true;
    }

    case Opcode::RET_NEAR: {
        // RIP = [RSP] (8 bytes) ; RSP += 8 ; can't statically link (dynamic target).
        const u8 rsp_index = static_cast<u8>(RegIndex::RSP);
        const bool pinned_rsp = m_pins_live &&
            m_pin_arm[rsp_index] != 0xFF;
        const u8 rsp_reg = pinned_rsp ? m_pin_arm[rsp_index]
                                      : REG_CALL_TMP;
        if (!pinned_rsp) emit_load_gpr64(rsp_reg, rsp_index);
        emit_guest_load64(REG_SCRATCH1, rsp_reg);
        emit(arm64::add_x_imm(rsp_reg, rsp_reg, 8));
        if (pinned_rsp) m_esp_frame_live = false;
        else emit_store_gpr64(rsp_reg, rsp_index);
        // RET: return natively to a matching continuation frame, else probe
        // L1 with the popped RIP still live in X1.
        if (!emit_linked_tail_reg(REG_SCRATCH1, NzcvState::None, false,
                                  /*native_ret=*/true)) {
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                   rip_offset() >> 3));
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    case Opcode::RET_NEAR_IMM16: {
        // Intel SDM Vol. 2 RET: RIP := Pop() (operand size forced to 64 for a
        // near branch, 66H or not), then "RSP := RSP + SRC" with SRC the
        // UNSIGNED imm16. 8 + imm16 reaches 65543, past ADD's 12-bit field.
        const u32 total_pop = 8u + static_cast<u16>(op0.imm);
        const u8 rsp_index = static_cast<u8>(RegIndex::RSP);
        const bool pinned_rsp = m_pins_live &&
            m_pin_arm[rsp_index] != 0xFF;
        const u8 rsp_reg = pinned_rsp ? m_pin_arm[rsp_index]
                                      : REG_CALL_TMP;
        if (!pinned_rsp) emit_load_gpr64(rsp_reg, rsp_index);
        emit_guest_load64(REG_SCRATCH1, rsp_reg);
        emit_add_x_uimm24(rsp_reg, rsp_reg, total_pop);
        if (pinned_rsp) m_esp_frame_live = false;
        else emit_store_gpr64(rsp_reg, rsp_index);
        // RET imm16: same native return and dynamic L1 tail as plain RET.
        if (!emit_linked_tail_reg(REG_SCRATCH1, NzcvState::None, false,
                                  /*native_ret=*/true)) {
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                   rip_offset() >> 3));
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // ========================================================================
    // Indirect control flow — 64-bit
    //
    // Target is computed at runtime from a register or memory operand. We
    // can't link the tail because the destination is unknown at compile
    // time; instead we update cpu->rip and return to the dispatcher.
    // ========================================================================

    // JMP r/m64  — REX.W FF /4
    case Opcode::JMP_RM64: {
        // Compute target into X1
        if (is_register_operand(op0.type)) {
            emit_load_gpr64(REG_SCRATCH1, op0.reg);
        } else {
            // A pinned base is the address itself (emit_ea64), so the load
            // reads it in place instead of through two copies.
            const u8 ea = emit_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(ea, instr, op0);
            emit_guest_load64(REG_SCRATCH1, ea);
        }
        if (!emit_linked_tail_reg(REG_SCRATCH1, NzcvState::None, false, false,
                                  /*api_call_tail=*/true)) {
            emit(arm64::str_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                   rip_offset() >> 3));
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // CALL r/m64  — REX.W FF /2
    // Pushes 8-byte return address (next_rip), then sets RIP from operand.
    case Opcode::CALL_RM64: {
        // Compute the indirect target FIRST and stash it in X21
        // (callee-saved temp). emit_guest_store64 below clobbers X0/X1/X2
        // and may load from RIP-relative source, but reads of the source
        // operand must complete before RSP is mutated (since the operand
        // may itself reference [RSP+disp]).
        if (is_register_operand(op0.type)) {
            // X21 is the required across-helper home for the target. Resolve
            // a register operand there directly instead of materializing it
            // in X1 and copying it: CALL reads its operand before committing
            // RSP, and neither choice changes the fault-capable stack push.
            emit_load_gpr64(REG_CALL_TMP, op0.reg);
        } else {
            // Straight into the X21 home, from the pinned base in place when
            // the operand is [pin] or [pin+imm] (emit_ea64).
            const u8 ea = emit_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(ea, instr, op0);
            emit_guest_load64(REG_CALL_TMP, ea);
        }
        // Push next_rip:  RSP -= 8 ; [RSP] = next_rip
        const u8 rsp_index = static_cast<u8>(RegIndex::RSP);
        const bool pinned_rsp = m_pins_live &&
            m_pin_arm[rsp_index] != 0xFF;
        const u8 old_rsp_reg = pinned_rsp ? m_pin_arm[rsp_index]
                                          : REG_SCRATCH2;
        // X21 already owns the indirect target. Form the tentative stack
        // address in caller-saved X2; emit_guest_store64 copies it into X9
        // before any guard/helper clobber, while architectural RSP stays old.
        // Same guard-page growth rule as CALL_REL32: the store, not a
        // StackLimit compare, is what grows the stack. Same precise-RIP rule
        // too: the push's cold stub or the host-PC map supplies it.
        if (abi_align_trap_enabled()) emit_update_rip(instr_rip);
        if (!pinned_rsp) emit_load_gpr64(old_rsp_reg, rsp_index);
        emit(arm64::sub_x_imm(REG_SCRATCH2, old_rsp_reg, 8));
        if (abi_align_trap_enabled()) {
            // REG_SCRATCH2 already is X2, the third argument slot.
            emit(arm64::mov_x(3, REG_CALL_TMP));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                                  &jit_check_call_alignment)));
            emit_helper_blr();
            if (!pinned_rsp) emit_load_gpr64(old_rsp_reg, rsp_index);
            emit(arm64::sub_x_imm(REG_SCRATCH2, old_rsp_reg, 8));
        }
        emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(next_rip));
        emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        if (pinned_rsp) {
            emit(arm64::sub_x_imm(m_pin_arm[rsp_index],
                                  m_pin_arm[rsp_index], 8));
            m_esp_frame_live = false;
        } else {
            emit_load_gpr64(REG_SCRATCH2, rsp_index);
            emit(arm64::sub_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
            emit_store_gpr64(REG_SCRATCH2, rsp_index);
        }
        // No emit_rsb_push — see CALL_REL32 above for the measurement.

        if (!emit_linked_tail_reg(REG_CALL_TMP, NzcvState::None, false, false,
                                  /*api_call_tail=*/true)) {
            emit(arm64::str_x_uoff(REG_CALL_TMP, REG_CPU_STATE,
                                   rip_offset() >> 3));
            emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                                   jit_iters_offset() >> 2));
            emit_epilogue(JitExecResult::Continue);
        }
        return true;
    }

    // Far JMP/CALL m16:16 (66H) / m16:64 — Intel SDM Vol. 2, same-privilege
    // user-mode. Memory is offset (2 or 8) then selector (2). RIP := zero-
    // extended offset. Far CALL still pushes CS then RIP in 8-byte slots in
    // long mode (RETF pops RIP then CS). No call gates.
    case Opcode::JMP_M16_16:
    case Opcode::CALL_M16_16:
    case Opcode::JMP_M16_64:
    case Opcode::CALL_M16_64: {
        if (!is_memory_operand(op0.type)) {
            return false;
        }
        const u32 cs_off = static_cast<u32>(
            offsetof(CpuState, seg) + offsetof(SegmentRegs, cs));
        const u32 rip_off = rip_offset();
        if (!fits_ldr_x_offset(rip_off) || (cs_off & 1u) != 0 ||
            (cs_off >> 1) >= 4096u) {
            return false;
        }
        const bool op16 = instr.opcode == Opcode::JMP_M16_16 ||
                          instr.opcode == Opcode::CALL_M16_16;
        const bool is_call = instr.opcode == Opcode::CALL_M16_16 ||
                             instr.opcode == Opcode::CALL_M16_64;
        const u32 off_size = op16 ? 2u : 8u;

        emit_compute_ea64(REG_CALL_TMP, op0.mem, next_rip);
        emit_m128_align_check_if_required(REG_CALL_TMP, instr, op0);
        if (op16) {
            emit_guest_load16_64(REG_SCRATCH0, REG_CALL_TMP);
        } else {
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
        }
        emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, off_size));
        // Offset in X21 (callee-saved) so the selector load's helper/BLR
        // path cannot drop it. EA is finished after the +off_size.
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
        emit_guest_load16_64(REG_SCRATCH1, REG_SCRATCH2);

        if (is_call) {
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CPU_STATE, cs_off));
            emit(arm64::ldrh_reg(REG_SCRATCH0, REG_SCRATCH2, 31));
            emit(arm64::strh_uoff(REG_SCRATCH1, REG_CPU_STATE, cs_off >> 1));
            emit_load_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RSP));
            emit(arm64::sub_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
            emit_store_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RSP));
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);

            emit_load_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RSP));
            emit(arm64::sub_x_imm(REG_SCRATCH2, REG_SCRATCH2, 8));
            emit_store_gpr64(REG_SCRATCH2, static_cast<u8>(RegIndex::RSP));
            emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(next_rip));
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit(arm64::strh_uoff(REG_SCRATCH1, REG_CPU_STATE, cs_off >> 1));
        }
        emit(arm64::str_x_uoff(REG_CALL_TMP, REG_CPU_STATE, rip_off >> 3));
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_epilogue(JitExecResult::Continue);
        return true;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
