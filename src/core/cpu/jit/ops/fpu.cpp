// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — x87 FPU ops
//
// Every Opcode in the FNCLEX..FDIVR_M64 range (plus EMMS, FFREE_STI,
// FBLD_M80, FBSTP_M80) compiles into compile_fpu. Hot paths lower natively;
// the rest call jit_fpu_op → exec_fpu so x87 stays inside a JIT block instead
// of falling back to jit_interpret_decoded per instruction.
//
// compile_fpu_native was one 3,575-line function. Its native lowerings
// now live one family per ops/fpu_*.cpp (ADR-0006), moved verbatim; this
// file keeps the compile_fpu entry point and its guards, the family
// dispatch in compile_fpu_native, and the jit_fpu_op helper-call fallback
// every family shares.

#include "../jit_helpers.hpp"
#include "fpu_internal.hpp"
#include <optional>
#include <utility>
#include <vector>

#include <cstdlib>

namespace vitreo::cpu::jit {

namespace {

const bool kNativeFpuArith =
    std::getenv("VITREO_JIT_DISABLE_NATIVE_FPU_ARITH") == nullptr;

// DIAGNOSTIC (VITREO_JIT_FPU_FORCE_HELPER=<id[,id...]>): route ONLY these
// opcodes to jit_fpu_op, leaving every other x87 instruction on the native
// ARM64 lowering. The global disable flag changes thousands of instructions at
// once, so it cannot say WHICH one diverges; this bisects a single opcode
// against a known-good baseline. Numeric Opcode enum ids (opcode_name() has no
// case for the x87 memory forms, so names cannot be matched).
const std::vector<u32> kForceHelperIds = [] {
    std::vector<u32> ids;
    const char* v = std::getenv("VITREO_JIT_FPU_FORCE_HELPER");
    if (v == nullptr) return ids;
    const char* p = v;
    while (*p != '\0') {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 0);
        if (end == p) break;
        ids.push_back(static_cast<u32>(id));
        p = (*end == ',') ? end + 1 : end;
    }
    return ids;
}();

} // namespace

// External linkage: the families split out of compile_fpu_native read
// these too (fpu_internal.hpp). They are still defined once, here.
const bool kDisableX87D0Forward =
    std::getenv("VITREO_JIT_DISABLE_X87_D0_FORWARD") != nullptr;

[[nodiscard]] bool native_arith_allowed(Opcode op) noexcept {
    if (!kNativeFpuArith) return false;
    if (kForceHelperIds.empty()) return true;
    return std::find(kForceHelperIds.begin(), kForceHelperIds.end(),
                     static_cast<u32>(op)) == kForceHelperIds.end();
}

namespace {

[[nodiscard]] bool is_fpu_opcode(Opcode op) noexcept {
    const auto u = static_cast<u32>(op);
    return (u >= static_cast<u32>(Opcode::FNCLEX) &&
            u <= static_cast<u32>(Opcode::FDIVR_M64)) ||
           op == Opcode::EMMS ||
           op == Opcode::FFREE_STI ||
           op == Opcode::FBLD_M80 ||
           op == Opcode::FBSTP_M80;
}

// Intel SDM Vol. 1 section 8.3.12: these are the non-waiting FN control
// forms. Every other x87 ESC instruction checks pending unmasked exceptions
// before executing its primary operation. EMMS is an MMX state transition.
[[nodiscard]] bool is_x87_waiting(Opcode op) noexcept {
    switch (op) {
    case Opcode::FNCLEX: case Opcode::FNINIT:
    case Opcode::FNSTSW_AX: case Opcode::FNSTSW_M16:
    case Opcode::FNSTCW_M16: case Opcode::FNSTENV_M28:
    case Opcode::FNSAVE_M108: case Opcode::EMMS:
        return false;
    default:
        return true;
    }
}

} // namespace

namespace {

/// x87 precision control narrows the significand of the ARITHMETIC results
/// only (Intel SDM Vol. 1, 8.1.5.2). FLD/FST/FILD and the control-word ops are
/// unaffected, so only these need the PC guard.
/// Does `op` read a value out of the x87 stack? Those are the instructions a
/// stack UNDERFLOW can hit, so those are the ones whose source tags the JIT
/// has to check before running a native lowering.
///
/// Loads and the no-operand controls are excluded: FLD m32 sources memory, and
/// FNCLEX / FNSTCW / FLDCW touch no register. FLD1 and friends PUSH, and a
/// push faults on overflow rather than underflow -- handled in FpuState::push,
/// which the native path also goes through.
[[nodiscard]] bool sources_stack_register(Opcode op) noexcept {
    const auto u = static_cast<u32>(op);
    if (u < static_cast<u32>(Opcode::FNCLEX) ||
        u > static_cast<u32>(Opcode::FDIVR_M64)) {
        return false;
    }
    switch (op) {
    // No stack source: pure loads, pure control, pure constants.
    case Opcode::FLD_M32: case Opcode::FLD_M64: case Opcode::FLD_M80:
    case Opcode::FILD_M16: case Opcode::FILD_M32: case Opcode::FILD_M64:
    case Opcode::FBLD_M80:
    case Opcode::FLD1: case Opcode::FLDZ: case Opcode::FLDPI:
    case Opcode::FLDL2E: case Opcode::FLDL2T: case Opcode::FLDLG2:
    case Opcode::FLDLN2:
    case Opcode::FNCLEX: case Opcode::FNINIT: case Opcode::FNOP:
    case Opcode::FWAIT:
    case Opcode::FNSTCW_M16: case Opcode::FLDCW_M16:
    case Opcode::FNSTSW_AX: case Opcode::FNSTSW_M16:
    case Opcode::FNSTENV_M28: case Opcode::FLDENV_M28:
    case Opcode::FNSAVE_M108: case Opcode::FRSTOR_M108:
    case Opcode::FINCSTP: case Opcode::FDECSTP:
    case Opcode::FFREE_STI:
        return false;
    // FST/FSTP m32 read ST(0), but their native lowering has to load TOP for
    // the value anyway, so it tests the source tag itself in seven
    // instructions instead of paying this guard's sixteen. If that lowering
    // declines, compile_fpu emits nothing and falls through to the
    // interpreter delegate, which checks the tag in FpuState::read_src.
    case Opcode::FST_M32: case Opcode::FSTP_M32:
    case Opcode::FST_M64: case Opcode::FSTP_M64:
        return false;
    default:
        return true;
    }
}

/// Which ST indices `instr` reads. ST(0) always; the register forms add ST(i).
[[nodiscard]] std::vector<u8> stack_sources(const DecodedInstruction& instr) {
    std::vector<u8> out{0};
    if (instr.operand_count > 0 && !is_memory_operand(instr.operands[0].type)) {
        const u8 i = static_cast<u8>(instr.operands[0].imm) & 7u;
        if (i != 0) out.push_back(i);
    }
    return out;
}

[[nodiscard]] bool pc_affects_result(Opcode op) noexcept {
    switch (op) {
    case Opcode::F2XM1: case Opcode::FADDP_STI:
    case Opcode::FADD_M32: case Opcode::FADD_M64:
    case Opcode::FADD_STI: case Opcode::FADD_TO_STI:
    case Opcode::FCOS: case Opcode::FDIVP_STI:
    case Opcode::FDIVRP_STI: case Opcode::FDIVR_M32:
    case Opcode::FDIVR_M64: case Opcode::FDIVR_STI:
    case Opcode::FDIVR_TO_STI: case Opcode::FDIV_M32:
    case Opcode::FDIV_M64: case Opcode::FDIV_STI:
    case Opcode::FDIV_TO_STI: case Opcode::FIADD_M16:
    case Opcode::FIADD_M32: case Opcode::FIDIVR_M16:
    case Opcode::FIDIVR_M32: case Opcode::FIDIV_M16:
    case Opcode::FIDIV_M32: case Opcode::FIMUL_M16:
    case Opcode::FIMUL_M32: case Opcode::FISUBR_M16:
    case Opcode::FISUBR_M32: case Opcode::FISUB_M16:
    case Opcode::FISUB_M32: case Opcode::FMULP_STI:
    case Opcode::FMUL_M32: case Opcode::FMUL_M64:
    case Opcode::FMUL_STI: case Opcode::FMUL_TO_STI:
    case Opcode::FPATAN: case Opcode::FPREM:
    case Opcode::FPREM1: case Opcode::FPTAN:
    case Opcode::FSCALE: case Opcode::FSIN:
    case Opcode::FSINCOS: case Opcode::FSQRT:
    case Opcode::FSUBP_STI: case Opcode::FSUBRP_STI:
    case Opcode::FSUBR_M32: case Opcode::FSUBR_M64:
    case Opcode::FSUBR_STI: case Opcode::FSUBR_TO_STI:
    case Opcode::FSUB_M32: case Opcode::FSUB_M64:
    case Opcode::FSUB_STI: case Opcode::FSUB_TO_STI:
    case Opcode::FYL2X: case Opcode::FYL2XP1:
        return true;
    // The STORE forms no longer ride this guard. They used to, on the theory
    // that PC == 53 makes the native lowering's truncation of the significand
    // exact -- but PC constrains what ARITHMETIC produces, not what a
    // register holds: FLD m80 and FILD m64 both deposit 64 significant bits
    // at any PC, and the guard also said nothing about RC or about PE. The
    // lowerings now prove exactness from the VALUE instead, which is cheaper
    // and strictly sounder. See compile_fpu_native's FST/FSTP m32 and m64.
    default:
        return false;
    }
}

}  // namespace

namespace {

/// Does this instruction push a new value onto the x87 stack?
///
/// Every member decrements TOP and writes ST(0), so each needs the
/// push-destination tag guard: writing a slot whose tag is not Empty is a
/// stack overflow with an architecturally visible result (SF and C1).
[[nodiscard]] bool pushes_x87_stack(Opcode op) noexcept {
    switch (op) {
    // Constants.
    case Opcode::FLD1:
    case Opcode::FLDZ:
    case Opcode::FLDPI:
    case Opcode::FLDL2T:
    case Opcode::FLDL2E:
    case Opcode::FLDLG2:
    case Opcode::FLDLN2:
    // Loads from memory and from the stack itself.
    case Opcode::FLD_M32:
    case Opcode::FLD_M64:
    case Opcode::FLD_M80:
    case Opcode::FLD_STI:
    case Opcode::FILD_M16:
    case Opcode::FILD_M32:
    case Opcode::FILD_M64:
    case Opcode::FBLD_M80:
    // Transcendentals that push a SECOND result on top of their first.
    case Opcode::FXTRACT:
    case Opcode::FPTAN:
    case Opcode::FSINCOS:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool JitCompiler::compile_fpu(const DecodedInstruction& instr,
                               u64 instr_rip, u64 next_rip,
                               bool is_last) {
    if (!is_fpu_opcode(instr.opcode)) return false;

    const bool waiting = is_x87_waiting(instr.opcode);
    const auto emit_waiting_check = [&](u8 status_reg, bool load_status,
                                        bool reuse_status) {
        if (!waiting) return;
        constexpr u32 sw_off = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        static_assert(sw_off % 2 == 0 && (sw_off >> 1) <= 4095);
        if (load_status) {
            emit(arm64::ldrh_uoff(status_reg, REG_CPU_STATE, sw_off >> 1));
        }
        const usize no_pending = m_code_buf.size();
        emit(0); // TBZ Wstatus, #ES, primary-operation

        const bool hold_nzcv = should_hold_nzcv();
        if (hold_nzcv) emit_nzcv_spill();
        emit_update_rip(instr_rip);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(
                              &jit_fpu_deliver_pending)));
        emit_helper_blr();
        emit_exit_if_pending_raise(hold_nzcv);
        if (hold_nzcv) emit_nzcv_fill();
        // ES may be stale while every recorded flag is masked (for example
        // after FNSTENV/FLDCW). The helper correctly declines delivery; reload
        // FSW only on that rare fall-through when the following guard reuses
        // the value that the helper ABI clobbered.
        if (reuse_status) {
            emit(arm64::ldrh_uoff(status_reg, REG_CPU_STATE, sw_off >> 1));
        }

        m_code_buf[no_pending] = arm64::tbz(
            status_reg, 7,
            static_cast<i32>(m_code_buf.size() - no_pending) * 4);
    };

    // D0 forwarding is restricted to adjacent finite binary64 chains: the
    // m64 binary operations and the exact FLD1 / FADD ST(0),ST(0) sequence,
    // each immediately followed by FSTP m64. The runtime-valid bit is cleared
    // on every helper diversion below; the architectural Float80 value remains
    // authoritative on both paths.
    const bool publish_reg_d0 =
        instr.opcode == Opcode::FADD_STI && instr.operand_count > 0 &&
        (static_cast<u8>(instr.operands[0].imm) & 7u) == 0;
    const bool publish_d0 = (instr.opcode == Opcode::FADD_M64 ||
                             instr.opcode == Opcode::FMUL_M64 ||
                             publish_reg_d0) &&
                            m_next_opcode == Opcode::FSTP_M64 &&
                            !kDisableX87D0Forward &&
                            native_arith_allowed(instr.opcode);
    m_x87_d0_forward_out = publish_d0;

    // Precision control guard.
    //
    // The native arithmetic paths compute in binary64. That naturally matches
    // PC = 53 (double) and is what the extended case is approximated with, but
    // PC = 24 (single) requires the significand to be narrowed to 23 fraction
    // bits — which FpuState::round_result_to_precision does in the interpreter
    // and the native paths do not. Guests that select PC = 24 therefore got
    // interpreter-correct results and JIT-wrong ones, differing in the low
    // bits. (The earlier fp_oracle capture recorded 206 diffs here, every one
    // at PC = 24.)
    //
    // Rather than teach ten native fast paths to narrow, this routes the whole
    // instruction to the generic FPU helper — which runs the interpreter's
    // code, PC handling included — when the control word selects single
    // precision. PC = 24 is rare, so the common path pays one load, a mask and
    // a not-taken branch.
    // The guard also covers STACK FAULTS. Intel SDM Vol. 1 8.5.1.1: sourcing a
    // register the tag word marks Empty raises #IS -- IE and SF set, C1 clear
    // -- and the masked result is the indefinite QNaN. The native lowerings
    // read the register file directly and would compute with whatever stale
    // bits are there, so any instruction that SOURCES a stack register needs
    // the tag checked before the fast path runs.
    //
    // Checking the whole tag word for "any Empty" would be three instructions
    // but useless: real x87 code uses one to three registers and leaves the
    // rest Empty, so it would divert every instruction to the helper. The
    // check has to be per-source, which means going through TOP.
    const bool guard_pc = pc_affects_result(instr.opcode);
    const bool guard_stack = sources_stack_register(instr.opcode);
    // EVERY instruction that pushes, not just the constant loads.
    //
    // This guard used to name only FLD1/FLDZ/FLDPI/FLDL2T/FLDL2E/FLDLG2/FLDLN2
    // — its own comment said "a constant push". But FLD m32/m64/m80, FILD
    // m16/m32/m64, FLD ST(i) and FBLD push as well, and unguarded they wrote
    // the destination natively whatever its tag said. On a full slot that is a
    // stack overflow: the SDM requires SF and C1 in the status word (and, with
    // the invalid-operation exception unmasked, a fault), which the
    // interpreter produces via FpuState::push and the JIT silently did not.
    //
    // Found by the ISA matrix sweep seeding a partially-full x87 stack: 84
    // divergences across FILD_M16/FILD_M32/FLD_M80 alone, every one FSW
    // differing by exactly C1|SF.
    //
    // Adding an opcode here is safe by construction: the guard only diverts to
    // the helper, which owns the overflow contract, so a form that turns out
    // to be helper-lowered anyway loses nothing.
    const bool guard_push = pushes_x87_stack(instr.opcode);
    if (!guard_pc && !guard_stack && !guard_push) {
        const usize entry_start = m_code_buf.size();
        emit_waiting_check(16, true, false);
        if (compile_fpu_native(instr, instr_rip, next_rip, is_last)) {
            return true;
        }
        m_code_buf.resize(entry_start);
        return false;
    }

    const usize guard_start = m_code_buf.size();
    // Stack-source and push-destination guards both need FSW for TOP. Reuse
    // that load for the waiting check, leaving only TBZ on their common path.
    bool waiting_check_emitted = false;
    if (!guard_stack && !guard_push) {
        emit_waiting_check(16, true, false);
        waiting_check_emitted = waiting;
    }
    constexpr u32 cw_off = static_cast<u32>(
        offsetof(CpuState, fpu) + offsetof(FpuState, control_word));
    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CPU_STATE, cw_off));
    emit(arm64::ldrh_reg(REG_SCRATCH3, REG_SCRATCH4, 31));
    emit(arm64::lsr_w_imm(REG_SCRATCH3, REG_SCRATCH3, 8));
    emit(arm64::movz_w(REG_SCRATCH4, 3));
    emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
    // The native body computes in binary64, which is exactly right for PC = 53
    // (both round the infinitely precise result to 53 bits, once) and wrong for
    // the other two: PC = 24 needs a narrower significand, and PC = 64 needs a
    // WIDER one than binary64 can hold -- the low 11 bits of every inexact
    // extended-precision result. So the fast path is PC == 2 alone, and the
    // other encodings go to the helper, which routes through
    // FpuState::arith_x87 and does PC = 64 in the architectural format
    // (core/math/float80.hpp).
    //
    // Windows sets PC = 53 at CRT startup, so the overwhelmingly common case
    // still pays one load, a mask, a compare and a taken branch.
    //
    // Every test below compares with EOR + CBZ/CBNZ rather than SUBS + B.cond.
    // No x87 instruction the JIT lowers here writes EFLAGS (Intel SDM Vol. 1,
    // 8.1.4 -- only the FCOMI family does, and it is not routed through this
    // guard), and the JIT keeps pending lazy flags live in NZCV, so a guard
    // that set NZCV would silently destroy the flags of a preceding
    // arithmetic instruction. `cmp; fld; fstp; sete` is real, compiled code.
    emit(arm64::movz_w(REG_SCRATCH4, 2));
    emit(arm64::eor_w(REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH4));
    // Not-double -> helper. Recorded as a branch to the helper block that
    // follows, so the fall-through continues into the stack-tag test.
    // `true` marks the PC test, whose sense is inverted against the tag tests.
    std::vector<std::pair<usize, bool>> to_helper;
    if (guard_pc) {
        to_helper.emplace_back(m_code_buf.size(), true);
        emit(0);  // CBNZ -> helper
    }

    if (guard_stack) {
        // physical register of ST(k) is (TOP + k) & 7; its tag is the 2-bit
        // field at 2*physical in the tag word, and 3 means Empty.
        constexpr u32 sw_off = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tw_off = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_CPU_STATE, sw_off));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH4, 31));   // FSW
        emit_waiting_check(REG_SCRATCH2, false, true);
        waiting_check_emitted = waiting;
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 11, 3));  // TOP
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_CPU_STATE, tw_off));
        emit(arm64::ldrh_reg(REG_EA_TMP, REG_SCRATCH4, 31));     // tag word
        for (u8 k : stack_sources(instr)) {
            emit(arm64::movz_w(REG_SCRATCH4, k));
            emit(arm64::add_w(REG_SCRATCH3, REG_SCRATCH2, REG_SCRATCH4));
            emit(arm64::movz_w(REG_SCRATCH4, 7));
            emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
            emit(arm64::lsl_w_imm(REG_SCRATCH3, REG_SCRATCH3, 1));
            emit(arm64::lsrv_w(REG_SCRATCH3, REG_EA_TMP, REG_SCRATCH3));
            emit(arm64::movz_w(REG_SCRATCH4, 3));
            emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
            emit(arm64::eor_w(REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH4));
            to_helper.emplace_back(m_code_buf.size(), false);
            emit(0);  // CBZ -> helper (this source is Empty)
        }
    }

    if (guard_push) {
        // A constant push is native only when its prospective destination is
        // Empty. Overflow is deliberately routed through FpuState::push so
        // masked indefinite completion and unmasked pending state have one
        // architectural owner.
        constexpr u32 sw_off = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, status_word));
        constexpr u32 tw_off = static_cast<u32>(
            offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_CPU_STATE, sw_off));
        emit(arm64::ldrh_reg(REG_SCRATCH2, REG_SCRATCH4, 31));
        if (!waiting_check_emitted) {
            emit_waiting_check(REG_SCRATCH2, false, true);
            waiting_check_emitted = waiting;
        }
        emit(arm64::ubfx_w(REG_SCRATCH2, REG_SCRATCH2, 11, 3));
        emit(arm64::movz_w(REG_SCRATCH4, 1));
        emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::movz_w(REG_SCRATCH4, 7));
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit(arm64::lsl_w_imm(REG_SCRATCH2, REG_SCRATCH2, 1));
        emit(arm64::add_x_imm(REG_SCRATCH4, REG_CPU_STATE, tw_off));
        emit(arm64::ldrh_reg(REG_SCRATCH3, REG_SCRATCH4, 31));
        emit(arm64::lsrv_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::movz_w(REG_SCRATCH4, 3));
        emit(arm64::and_w(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));
        emit(arm64::eor_w(REG_SCRATCH4, REG_SCRATCH3, REG_SCRATCH4));
        to_helper.emplace_back(m_code_buf.size(), true);
        emit(0);  // CBNZ -> helper (destination is not Empty)
    }

    const usize to_native = m_code_buf.size();
    emit(0);  // B -> native body: every guard passed

    // Every guard branch lands here: hand the instruction to the
    // interpreter-backed helper, which handles PC = 24 / PC = 64 narrowing and
    // the stack-fault contract in one place.
    const usize helper_pos = m_code_buf.size();
    for (const auto& [br, is_pc_test] : to_helper) {
        // PC and push-destination tests branch on non-zero; source-tag tests
        // branch on zero.
        const i32 delta = static_cast<i32>(helper_pos - br) * 4;
        m_code_buf[br] = is_pc_test ? arm64::cbnz_w(REG_SCRATCH4, delta)
                                    : arm64::cbz_w(REG_SCRATCH4, delta);
    }
    // The helper is a BLR, which destroys NZCV outright. x87 leaves EFLAGS
    // alone, so a pending compare has to survive the whole diversion -- the
    // native body below spills for the same reason.
    const bool helper_hold = should_hold_nzcv();
    if (helper_hold) emit_nzcv_spill();
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
    u32 kind = kFpuKindNone;
    if (instr.operand_count > 0 && is_memory_operand(instr.operands[0].type)) {
        kind = kFpuKindMem;
        emit_compute_ea(REG_CALL_TMP, instr.operands[0].mem, next_rip);
    } else if (instr.operand_count > 0) {
        kind = kFpuKindSti;
        // jit_fpu_op takes the ST(i) index from its 4th argument (`addr & 7`).
        // Only the memory path wrote that register, so an ST(i) form reached
        // the helper with a stale index and operated on the wrong register --
        // `faddp st(1),st` with a leftover 0 became st0+st0 followed by a pop,
        // silently discarding the addend. The native path reads the same field
        // (op0.imm & 7), so both now agree.
        emit_mov_imm32(REG_CALL_TMP,
                       static_cast<u32>(instr.operands[0].imm) & 7u);
    }
    emit_mov_imm32(2, (kind << 28) |
                      (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu));
    emit(arm64::mov_x(3, REG_CALL_TMP));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
    emit_semantic_helper_blr();
    // W6 is caller-saved under AAPCS64. Invalidate the adjacent D0 mirror
    // after the helper returns; clearing it before BLR lets the callee's
    // register allocation manufacture a false-valid forwarding edge.
    if (publish_d0) emit(arm64::movz_w(kX87D0Valid, 0));
    if (helper_hold) emit_nzcv_fill();
    const usize to_end = m_code_buf.size();
    emit(0);  // B -> past the native body

    m_code_buf[to_native] = arm64::b_imm(
        static_cast<i32>(m_code_buf.size() - to_native) * 4);

    if (!compile_fpu_native(instr, instr_rip, next_rip, is_last)) {
        // The body declined; drop the guard so no orphaned code is left and
        // let the caller fall through to the interpreter delegate.
        m_code_buf.resize(guard_start);
        return false;
    }
    m_code_buf[to_end] = arm64::b_imm(
        static_cast<i32>(m_code_buf.size() - to_end) * 4);
    // The native body emits the block terminator itself when this is the last
    // instruction, but the PC==24 helper path above branches *past* that body
    // and so lands here. For the last instruction "here" is the end of the
    // emitted block, and without a terminator execution runs off the end into
    // unwritten arena -- an illegal-instruction fault one byte past the block.
    // Only a guest running in single precision takes that path, which is why
    // it stayed hidden until an x87-heavy block ended on an FPU op.
    if (is_last) {
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

void JitCompiler::emit_mmx_enter() {
    constexpr u32 tag_offset = static_cast<u32>(
        offsetof(CpuState, fpu) + offsetof(FpuState, tag_word));
    constexpr u32 status_offset = static_cast<u32>(
        offsetof(CpuState, fpu) + offsetof(FpuState, status_word));

    // tag_word := 0x0000 (every register Valid).
    emit_mov_imm32(REG_SCRATCH0, 0);
    emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, tag_offset));
    emit(arm64::strh_reg(REG_SCRATCH0, REG_SCRATCH3, 31));

    // TOP := 0, leaving the condition codes and sticky exception bits alone.
    // TOP is status_word bits 11..13, so mask with ~0x3800 = 0xC7FF.
    emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, status_offset));
    emit(arm64::ldrh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
    emit(arm64::movz_w(REG_SCRATCH4, 0xC7FF));
    emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH4));
    emit(arm64::strh_reg(REG_SCRATCH1, REG_SCRATCH3, 31));
}

bool JitCompiler::compile_fpu_native(const DecodedInstruction& instr,
                                      u64 instr_rip, u64 next_rip,
                                      bool is_last) {
    (void)instr_rip;

    const auto& op0 = instr.operands[0];

    // Cold-compile storms hit this per x87 insn. Do not call getenv here.
    // Intel SDM Vol. 2 FLD/FADD/FMUL m32: convert real32 through 80-bit
    // extended precision. Finite normals use the native ARM64 path; Inf,
    // NaN, and subnormals stay on jit_fpu_op. Opt out with
    // VITREO_JIT_DISABLE_NATIVE_FPU_ARITH if a guest needs the helper for
    // every op.
    //
    // The native lowerings, one family per ops/fpu_*.cpp (ADR-0006; the
    // blocks are the old single function's, verbatim). A family returns
    // nullopt for an opcode outside it, and otherwise the lowering's own
    // result, returned from here unchanged. The families' opcode sets are
    // disjoint, so the order of this chain cannot change which one claims
    // an instruction; anything none of them claims takes the jit_fpu_op
    // helper call below.
    if (auto fam = compile_fpu_control(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_stack(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_load(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_store(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_arith_reg(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_arith_mem(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_arith_int(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_math(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }
    if (auto fam = compile_fpu_compare(instr, next_rip, is_last);
        fam.has_value()) {
        return *fam;
    }

    // Intel SDM Vol. 2: FLD/FST/FADD/FMUL/FSUB/FCOM/FNSTSW leave EFLAGS
    // unchanged. The native lowerings spill NZCV for exactly that reason (see
    // the FLD_M32 path above: "the Inf/NaN SUBS gate must not steal a prior CMP
    // for CMP; FLD; FSTP; JNE"), but this helper-call path did not -- and a BLR
    // into C clobbers NZCV, so a pending lazy flag carried in NZCV was silently
    // destroyed by any x87 instruction that took the helper. That is every
    // instruction with no native lowering, plus the Inf/NaN/subnormal and
    // PC=Single fallbacks, so it fires on real operand values at runtime while
    // native x86 leaves the flags alone.
    const bool fpu_helper_hold = should_hold_nzcv();
    if (fpu_helper_hold) emit_nzcv_spill();

    u32 kind = kFpuKindNone;
    if (is_memory_operand(op0.type)) {
        emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
        // The hottest x87 m32 reads can use the JIT's checked linear-memory
        // load before entering the semantic helper. This preserves the x87
        // fault ordering (a bad read faults before changing the FPU stack or
        // status) while avoiding a second GuestMemory/TLB lookup in the
        // helper. The helper still owns Float80 state and rounding behavior.
        const bool preload_m32 =
            instr.opcode == Opcode::FLD_M32 ||
            instr.opcode == Opcode::FADD_M32 ||
            instr.opcode == Opcode::FMUL_M32 ||
            instr.opcode == Opcode::FSUB_M32 ||
            instr.opcode == Opcode::FSUBR_M32 ||
            instr.opcode == Opcode::FDIV_M32 ||
            instr.opcode == Opcode::FDIVR_M32 ||
            instr.opcode == Opcode::FCOMP_M32;
        if (preload_m32) {
            kind = kFpuKindM32Value;
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_w(3, REG_SCRATCH0));
        } else if (instr.opcode == Opcode::FILD_M64) {
            // Intel SDM Vol. 2 FILD: convert the signed m64int to 80-bit
            // extended precision without rounding. Carry the exact loaded
            // bits into the semantic helper; reconstructing the already-
            // computed EA as a disp32 cannot address x64 memory above 4 GiB.
            kind = kFpuKindI64Value;
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::mov_x(3, REG_SCRATCH0));
        } else {
            kind = kFpuKindMem;
            // Move the EA from X2 to X3 BEFORE clobbering X2 with the opcode.
            // This was a 32-bit MOV, which zero-extends: the fpu_op argument
            // was widened to u64 precisely so a >4 GB x87 access would work,
            // but this call site kept truncating it. Reaches FILD_M64,
            // FBLD_M80 and FBSTP_M80 always (they have no native path), and
            // every _M64 arithmetic form under
            // VITREO_JIT_DISABLE_NATIVE_FPU_ARITH.
            emit(arm64::mov_x(3, REG_SCRATCH2));
        }
    } else if (is_immediate_operand(op0.type)) {
        kind = kFpuKindSti;
        emit(arm64::movz_w(3, static_cast<u16>(op0.imm & 7)));
    } else {
        emit(arm64::movz_w(3, 0));
    }

    const u32 packed =
        (kind << 28) | (static_cast<u32>(instr.opcode) & 0x0FFFFFFFu);

    if (instr.opcode == Opcode::FWAIT) {
        // A pending x87 exception is attributed to the waiting instruction,
        // not to the earlier instruction that recorded it. Publish RIP before
        // materialising helper arguments because emit_update_rip may use
        // scratch registers from the helper ABI.
        emit_update_rip(instr_rip);
    }
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
    emit_mov_imm32(2, packed);
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_FPU_OP >> 3));
    emit_semantic_helper_blr(); // blr x16

    if (fpu_helper_hold) emit_nzcv_fill();
    if (instr.opcode == Opcode::FWAIT) emit_exit_if_pending_raise();

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

} // namespace vitreo::cpu::jit
