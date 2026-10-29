// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Native ARM64 codegen for 64-bit (REX.W) x86 instructions.
//
// This is the per-opcode dispatch for 64-bit guests. Any opcode not
// recognised here returns false so the caller can fall back to the
// universal interpreter-delegate path (which is always correct).

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// VEX.256 pure-move forms whose 256-bit lowering already exists in the switch
// below (the `if (instr.prefixes.vex_l)` arms of the MOVDQA/MOVAPS load and
// store cases). The generic VEX bridge below STRIPS vex_l before recursing,
// so those arms are unreachable through it by construction — these opcodes
// must bypass the bridge entirely and fall through to the switch with vex_l
// still set. Pure moves have no vvvv operand and write both lanes outright,
// so none of the bridge's merge / high-lane-zero prologue applies.
[[nodiscard]] static bool vex256_move_lowered_natively(
    const DecodedInstruction& instr) noexcept {
    if (!instr.prefixes.vex_l) return false;
    switch (instr.opcode) {
        case Opcode::MOVDQA_XMM_XMMM128:
        case Opcode::MOVDQU_XMM_XMMM128:
        case Opcode::LDDQU_XMM_M128:
        case Opcode::MOVAPS_XMM_XMMM128:
        case Opcode::MOVUPS_XMM_XMMM128:
        case Opcode::MOVAPD_XMM_XMMM128:
        case Opcode::MOVUPD_XMM_XMMM128:
        case Opcode::MOVDQA_XMMM128_XMM:
        case Opcode::MOVDQU_XMMM128_XMM:
        case Opcode::MOVAPS_XMMM128_XMM:
        case Opcode::MOVUPS_XMMM128_XMM:
        case Opcode::MOVAPD_XMMM128_XMM:
        case Opcode::MOVUPD_XMMM128_XMM:
        case Opcode::MOVNTDQ_M128_XMM:
            return true;
        default:
            return false;
    }
}

namespace {
/// VITREO_JIT_NO_ROUTE64=1 sends every opcode that compile_64bit forwards to
/// the 32-bit compilers back to the delegate. Kill switch for bisecting a
/// regression in that routing.
bool route32_disabled() {
    static const bool off = (std::getenv("VITREO_JIT_NO_ROUTE64") != nullptr);
    return off;
}
}  // namespace

// Non-control-flow op: if this is the last instruction in the block, emit the
// standard tail (advance RIP, then link or return Continue to the dispatcher).
// Shared by compile_64bit and compile_shifts64 since the split.
//
// The `u32 ji_off = jit_iters_offset();` this used to open with was dead --
// emit_tail_link_or_exit computes the offset itself -- and is gone. It emitted
// nothing, so the code is unchanged.
void JitCompiler::finish_op64_block(u64 next_rip, bool is_last) {
    if (!is_last) return;
    emit_update_rip(next_rip);
    emit_tail_link_or_exit(next_rip, m_flags.nzcv);
}

bool JitCompiler::compile_64bit(const DecodedInstruction& instr,
                                 u64 instr_rip, u64 next_rip,
                                 bool is_last) {
    // A VEX-encoded SSE instruction reuses the legacy SSE Opcode with
    // prefixes.vex set, but it is NOT the legacy instruction: VEX.NDS makes it
    // three-operand (dst := vvvv OP rm) and the 128-bit form ZEROES
    // YMM[255:128] of the destination (Intel SDM Vol. 2, "VEX Encoding" /
    // 2.3.5). Every SSE lowering in the switch below is the legacy
    // two-operand, 128-bit-only form and leaves the upper lane alone.
    // compile_sse carries the same bridge, but in a 64-bit block this
    // function runs FIRST, so without this the x64 lowerings claimed VEX
    // forms and silently left a stale high lane — which is exactly how
    // `vpxor xmm,xmm,xmm` before an AVX2 `vpcmpeqb ymm` invents matches in
    // bytes 16..31 of a strlen/memchr scan.
    if (instr.prefixes.vex && ops::opcode_is_vex_sse_family(instr.opcode) &&
        !vex256_move_lowered_natively(instr)) {
        // 256-bit needs the high lane run a second time, the NDD shifts put
        // the destination in vvvv, and VMOVSS/VMOVSD merge after the move:
        // all three are interpreter-bridge-only shapes.
        if (instr.prefixes.vex_l) return false;
        if (ops::is_vex_ndd_shift_imm(instr.opcode)) return false;
        if (instr.opcode == Opcode::MOVSS_XMM_XMMM32 ||
            instr.opcode == Opcode::MOVSD_XMM_XMMM64) {
            return false;
        }

        const bool xmm_dst = instr.operand_count > 0 &&
                             instr.operands[0].type == OperandType::Reg128;
        const u8 vvvv = instr.prefixes.vex_vvvv & 15;
        u8 dst = 0;
        u32 dhi_off = 0;
        if (xmm_dst) {
            dst = instr.operands[0].reg & 15;
            dhi_off = ymm_high_u64_off(dst, 0);
            if (!fits_ldr_q_offset(dhi_off)) return false;
        }

        const bool merge =
            xmm_dst && ops::vex_sse_merges_vvvv(instr) && dst != vvvv;
        if (merge) {
            // A register source that is also the destination would be
            // destroyed by the merge copy.
            if (instr.operand_count > 1 &&
                instr.operands[1].type == OperandType::Reg128 &&
                (instr.operands[1].reg & 15) == dst) {
                return false;
            }
            if (!fits_ldr_q_offset(xmm_u64_off(dst, 0)) ||
                !fits_ldr_q_offset(xmm_u64_off(vvvv, 0))) {
                return false;
            }
        }

        DecodedInstruction legacy = instr;
        legacy.prefixes.vex = false;
        legacy.prefixes.vex_l = false;

        // Both prologue steps run BEFORE the legacy lowering: on the block's
        // last instruction that lowering emits the block exit, so anything
        // appended after it is unreachable. Neither step can disturb it — the
        // legacy lowerings read and write only the XMM half.
        const usize mark = m_code_buf.size();
        if (merge) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, xmm_u64_off(vvvv, 0) >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, xmm_u64_off(dst, 0) >> 4));
        }
        if (xmm_dst) {
            emit(arm64::eor_v16b(5, 5, 5));
            emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
        }
        if (!compile_64bit(legacy, instr_rip, next_rip, is_last)) {
            m_code_buf.resize(mark);
            return false;
        }
        return true;
    }

    // ========================================================================
    // SHL / SHR / SAR RM64 — 64-bit shifts.
    //
    // Lazy flag convention (FlagOp::Shl64 / Shr64 / Sar64):
    //   flag_src = original value (full 64-bit)
    //   flag_dst = result         (full 64-bit)
    // Materialization reconstructs ZF/SF/PF from flag_dst. The count does not
    // fit beside a 64-bit result, so CF (last bit shifted out; Intel SDM
    // Vol. 2 SAL/SAR/SHL/SHR) is written via cf_overlay. Count==1 uses the
    // Shl64_1 / Shr64_1 / Sar64_1 ops, which also define OF.
    //
    // x86 semantics: when count == 0, flags are unchanged. For IMM8 forms with
    // a non-zero compile-time count, no branch is needed. For CL forms a CBZ
    // guards the flag write.
    // ========================================================================
    // 64-bit shifts and rotates live in their own translation unit;
    // nullopt means "not one of those".
    if (auto sh = compile_shifts64(instr, next_rip, is_last);
        sh.has_value()) {
        return *sh;
    }

    // The rest of the 64-bit opcode space, one family per ops/*64.cpp
    // (ADR-0006 step 3; the arms are the old single switch's, verbatim). A
    // family returns nullopt for an opcode outside it, false to decline,
    // and true once its arm has run the end-of-block tail. Case labels are
    // disjoint across families, so chain order is not observable.
    if (auto r = compile_mov64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_bit_ops64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_narrow_alu64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_arithmetic64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_adc_sbb64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_compare64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_unary64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_stack64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_control64(instr, instr_rip, next_rip, is_last);
        r.has_value())
        return *r;
    if (auto r = compile_flags64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_mul_div64(instr, instr_rip, next_rip, is_last);
        r.has_value())
        return *r;
    if (auto r = compile_misc64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_string_ops64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_sse64(instr, next_rip, is_last); r.has_value())
        return *r;
    if (auto r = compile_sse_move64(instr, next_rip, is_last); r.has_value())
        return *r;

    switch (instr.opcode) {

    // ---- SHLD/SHRD with 32-bit operands, inside a 64-bit block -------------
    //
    // compile_shifts already lowers these, but the whole 32-bit native chain
    // is gated on `!m_block_is_64bit` (instruction_compilation.cpp), so in an
    // x64 block they fell through to the delegate. Measured on
    // tools/jit_bench shld_hash: 128,000,000 delegated executions, 99.85% of
    // everything the kernel ran, and one materialize_lazy_flags call each.
    //
    // The lowering needs no 64-bit variant: the operands are 32-bit either
    // way, and x86-64's rule that a 32-bit write zero-extends into the full
    // register is exactly what emit_store_gpr32 already emits in a 64-bit
    // block. Reuse it rather than duplicating the flag handling, which is
    // subtle (CF from the original destination, ZSP from the result, OF only
    // for count==1, and a pending FlagOp that must be discarded).
    // ---- 8/16-bit MOV-family forms inside a 64-bit block -------------------
    //
    // Same dispatch gap as the mul/div and double-shift forms below: these are
    // lowered in compile_mov, but the 32-bit native chain is gated on
    // `!m_block_is_64bit`, so x64 code fell through to the delegate. Measured
    // with VITREO_JIT_COVERAGE: MOVSX_R16_RM8 alone was 2.7% of all executed
    // instructions on an x64 C loop doing 16-bit arithmetic.
    //
    // Long mode changes the destination-width rule, and the lowerings already
    // follow it: emit_store_gpr16 / emit_store_gpr8 preserve bits 63:16 /
    // 63:8 in a 64-bit block (a 16- or 8-bit write is a partial-register
    // update), while emit_store_gpr32 zero-extends, which is what CWDE needs.
    // The 8-bit helpers also take `has_rex`, so AH/CH/DH/BH versus
    // SPL/BPL/SIL/DIL is already handled. MOFFS addresses go through
    // emit_compute_ea, which forwards to emit_compute_ea64.
    case Opcode::CBW:
    case Opcode::CWD:
    case Opcode::CWDE:
    case Opcode::LFENCE:
    case Opcode::MFENCE:
    case Opcode::MOVSX_R16_RM8:
    case Opcode::MOVZX_R16_RM8:
    case Opcode::MOV_AL_MOFFS8:
    case Opcode::MOV_EAX_MOFFS32:
    case Opcode::MOV_MOFFS32_EAX:
    case Opcode::MOV_MOFFS8_AL:
    case Opcode::SFENCE:
    case Opcode::XCHG_RM8_R8:
        if (route32_disabled()) return false;
        return compile_mov(instr, instr_rip, next_rip, is_last);

    // ---- 8/16-bit ALU forms inside a 64-bit block --------------------------
    // As above, via compile_arithmetic. ADC/SBB read CF, which their lowerings
    // already settle; the partial-register width rules are the same helpers.
    case Opcode::ADC_AL_IMM8:
    case Opcode::ADC_AX_IMM16:
    case Opcode::ADC_R16_RM16:
    case Opcode::ADC_R8_RM8:
    case Opcode::ADC_RM16_IMM16:
    case Opcode::ADC_RM16_IMM8:
    case Opcode::ADC_RM16_R16:
    case Opcode::ADC_RM8_IMM8:
    case Opcode::ADC_RM8_R8:
    case Opcode::ADD_AL_IMM8:
    case Opcode::ADD_R8_RM8:
    case Opcode::ADD_RM8_IMM8:
    case Opcode::ADD_RM8_R8:
    case Opcode::AND_AX_IMM16:
    case Opcode::AND_R16_RM16:
    // AND_RM16_* / OR_RM16_* also appear in compile_locked (memory_ops.cpp),
    // but that compiler only ever runs for a LOCK-prefixed instruction, so it
    // does not serve these non-LOCK forms. Omitting them here left `and ax,
    // imm16` delegating in x64 blocks; XOR has no LOCK-16 group, which is why
    // XOR was routed and AND/OR were not.
    case Opcode::AND_RM16_IMM16:
    case Opcode::AND_RM16_IMM8:
    case Opcode::AND_RM16_R16:
    case Opcode::DEC_RM16:
    case Opcode::INC_RM16:
    case Opcode::NEG_RM16:
    case Opcode::NEG_RM8:
    case Opcode::NOT_RM8:
    case Opcode::OR_AX_IMM16:
    case Opcode::OR_R16_RM16:
    case Opcode::OR_RM16_IMM16:
    case Opcode::OR_RM16_IMM8:
    case Opcode::OR_RM16_R16:
    case Opcode::SBB_AL_IMM8:
    case Opcode::SBB_AX_IMM16:
    case Opcode::SBB_R16_RM16:
    case Opcode::SBB_R8_RM8:
    case Opcode::SBB_RM16_IMM16:
    case Opcode::SBB_RM16_IMM8:
    case Opcode::SBB_RM16_R16:
    case Opcode::SBB_RM8_IMM8:
    case Opcode::SBB_RM8_R8:
    case Opcode::XOR_AL_IMM8:
    case Opcode::XOR_AX_IMM16:
    case Opcode::XOR_R16_RM16:
    case Opcode::XOR_RM16_IMM16:
    case Opcode::XOR_RM16_IMM8:
    case Opcode::XOR_RM16_R16:
    case Opcode::XOR_RM8_IMM8:
    case Opcode::XOR_RM8_R8:
        if (route32_disabled()) return false;
        return compile_arithmetic(instr, instr_rip, next_rip, is_last);

    // ---- 32-bit MUL/DIV inside a 64-bit block ------------------------------
    //
    // Same structural gap as the double shifts below: compile_mul_div lowers
    // these, but the 32-bit native chain is gated on `!m_block_is_64bit`, so
    // every 32-bit integer divide in x64 code was reaching the delegate.
    // Measured with VITREO_JIT_COVERAGE on an x64 C program doing int
    // multiply/divide in a loop: DIV_RM32 and IDIV_RM32 were 2.7% of all
    // executed instructions each.
    //
    // No 64-bit variant is needed: the operands are 32-bit either way, and the
    // lowering already uses the bitness-aware helpers -- emit_compute_ea
    // forwards to emit_compute_ea64, and emit_store_gpr32 supplies x86-64's
    // zero-extension of EAX/EDX. The #DE path calls jit_raise_divide_error
    // with instr_rip exactly as in a 32-bit block.
    // 8/16-bit MUL/DIV, same routing. These touch no pointer register -- the
    // implicit operands are AL/AX/DX -- so the width-correct data helpers
    // cover long mode's partial-register rules on their own.
    case Opcode::DIV_RM16:
    case Opcode::DIV_RM8:
    case Opcode::IDIV_RM16:
    case Opcode::IDIV_RM8:
    case Opcode::IMUL_R16_RM16:
    case Opcode::IMUL_R16_RM16_IMM16:
    case Opcode::IMUL_R16_RM16_IMM8:
    case Opcode::IMUL_RM16:
    case Opcode::IMUL_RM8:
    case Opcode::MUL_RM16:
    case Opcode::MUL_RM8:
        if (route32_disabled()) return false;
        return compile_mul_div(instr, instr_rip, next_rip, is_last);

    case Opcode::MUL_RM32:
    case Opcode::IMUL_RM32:
    case Opcode::DIV_RM32:
    case Opcode::IDIV_RM32:
        if (route32_disabled()) return false;
        return compile_mul_div(instr, instr_rip, next_rip, is_last);

    // ---- LEAVE / LOOP family inside a 64-bit block -------------------------
    // Both are valid in long mode but 64-bit wide: LEAVE moves RBP into RSP
    // and pops an 8-byte slot; LOOP* and JrCXZ use RCX, not ECX.
    // compile_control now selects the width from m_block_is_64bit.
    //
    // JRCXZ_REL8 is the opcode the decoder actually produces for E3 in Mode64
    // and had no lowering at all, so every jrcxz in x64 code was delegating.
    //
    // Deliberately absent: the far ptr16:16/ptr16:32 CALL/JMP forms are #UD in
    // long mode and the decoder rejects them, and CALL_RM32 / JMP_RM32 /
    // CALL_M16_32 / JMP_M16_32 / JCXZ / JECXZ are never produced in Mode64 --
    // a case for any of those would be dead code implying a reachability that
    // does not exist.
    case Opcode::LEAVE:
    case Opcode::LOOP_REL8:
    case Opcode::LOOPE_REL8:
    case Opcode::LOOPNE_REL8:
    case Opcode::JRCXZ_REL8:
        if (route32_disabled()) return false;
        return compile_control(instr, instr_rip, next_rip, is_last);

    // ---- ENTER inside a 64-bit block --------------------------------------
    //
    // LEAVE's counterpart. Long mode forces the operand size to 64 and
    // StackAddrSize to 64, so the frame slots are 8 bytes and the display
    // walk strides by 8. compile_stack's ENTER lowering was 32-bit-only
    // (emit_load_gpr32/emit_store_gpr32 on RSP/RBP would zero the top halves,
    // and the slots were 4 bytes); it now selects the width from
    // m_block_is_64bit via emit_load_gpr_ptr / emit_store_gpr_ptr and the
    // 64-bit guest access helpers, matching interpreter exec_enter's
    // `stack_size = is_64bit() ? 8 : 4`.
    //
    // 66H ENTER still delegates: compile_stack rejects any operand-size
    // override up front, and neither the decoder nor the interpreter models a
    // 16-bit frame.
    case Opcode::ENTER:
        if (route32_disabled()) return false;
        return compile_stack(instr, instr_rip, next_rip, is_last);

    // ---- String ops inside a 64-bit block ----------------------------------
    //
    // These were the last big dispatch gap, but unlike the batches below they
    // needed a real fix first: the inline non-REP paths read and wrote
    // RSI/RDI/RCX with emit_load_gpr32 / emit_store_gpr32, which truncates a
    // 64-bit pointer and then zero-extends the top half away. Those sites now
    // use emit_load_gpr_ptr / emit_store_gpr_ptr, which are address-width.
    // The REP helpers were already 64-bit aware (`wide = cpu->is_64bit()`).
    //
    // A 0x67 address-size prefix in long mode makes the pointers 32-bit again;
    // the inline paths do not model that, so those keep going to the delegate.
    case Opcode::MOVSB:
    case Opcode::MOVSW:
    case Opcode::MOVSD_STR:
    case Opcode::STOSB:
    case Opcode::STOSW:
    case Opcode::STOSD:
    case Opcode::LODSB:
    case Opcode::LODSW:
    case Opcode::LODSD:
    case Opcode::SCASB:
    case Opcode::SCASW:
    case Opcode::SCASD:
    case Opcode::CMPSB:
    case Opcode::CMPSW:
    case Opcode::CMPSD_STR:
        if (route32_disabled()) return false;
        if (instr.prefixes.address_size) return false;
        return compile_string_ops(instr, instr_rip, next_rip, is_last);

    // ---- 8/16-bit shifts and rotates inside a 64-bit block -----------------
    // Same dispatch gap; compile_shifts owns these. None of them references a
    // pointer register -- they act on the data operand, taking memory through
    // emit_compute_ea (which forwards to emit_compute_ea64) and registers
    // through the width-correct emit_load/store_gpr8/16/32 helpers, so long
    // mode's partial-register rules are already applied. RCL/RCR read CF,
    // which their lowerings settle themselves.
    case Opcode::RCL_RM16_1:
    case Opcode::RCL_RM16_CL:
    case Opcode::RCL_RM16_IMM8:
    case Opcode::RCL_RM32_1:
    case Opcode::RCL_RM32_CL:
    case Opcode::RCL_RM32_IMM8:
    case Opcode::RCL_RM8_1:
    case Opcode::RCL_RM8_CL:
    case Opcode::RCL_RM8_IMM8:
    case Opcode::RCR_RM16_1:
    case Opcode::RCR_RM16_CL:
    case Opcode::RCR_RM16_IMM8:
    case Opcode::RCR_RM32_1:
    case Opcode::RCR_RM32_CL:
    case Opcode::RCR_RM32_IMM8:
    case Opcode::RCR_RM8_1:
    case Opcode::RCR_RM8_CL:
    case Opcode::RCR_RM8_IMM8:
    case Opcode::ROL_RM16_1:
    case Opcode::ROL_RM16_CL:
    case Opcode::ROL_RM16_IMM8:
    case Opcode::ROL_RM8_1:
    case Opcode::ROL_RM8_CL:
    case Opcode::ROL_RM8_IMM8:
    case Opcode::ROR_RM16_1:
    case Opcode::ROR_RM16_CL:
    case Opcode::ROR_RM16_IMM8:
    case Opcode::ROR_RM8_1:
    case Opcode::ROR_RM8_CL:
    case Opcode::ROR_RM8_IMM8:
    case Opcode::SAR_RM16_1:
    case Opcode::SAR_RM16_CL:
    case Opcode::SAR_RM16_IMM8:
    case Opcode::SAR_RM8_1:
    case Opcode::SAR_RM8_CL:
    case Opcode::SAR_RM8_IMM8:
    case Opcode::SHLD_RM16_R16_CL:
    case Opcode::SHLD_RM16_R16_IMM8:
    case Opcode::SHL_RM16_1:
    case Opcode::SHL_RM16_CL:
    case Opcode::SHL_RM16_IMM8:
    case Opcode::SHL_RM8_1:
    case Opcode::SHL_RM8_CL:
    case Opcode::SHL_RM8_IMM8:
    case Opcode::SHRD_RM16_R16_CL:
    case Opcode::SHRD_RM16_R16_IMM8:
    case Opcode::SHR_RM8_CL:
    case Opcode::SHR_RM8_IMM8:
        if (route32_disabled()) return false;
        return compile_shifts(instr, instr_rip, next_rip, is_last);

    case Opcode::SHLD_RM32_R32_IMM8:
    case Opcode::SHRD_RM32_R32_IMM8:
    case Opcode::SHLD_RM32_R32_CL:
    case Opcode::SHRD_RM32_R32_CL:
        if (route32_disabled()) return false;
        return compile_shifts(instr, instr_rip, next_rip, is_last);

    // ========================================================================
    // Anything else: fall back to delegate path
    // ========================================================================
    default:
        return false;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

} // namespace vitreo::cpu::jit
