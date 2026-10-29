// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend_forms.cpp
/// @brief The Tier-1 admission tables shared by both bitnesses: `classify`,
///        `jcc_condition` and the `VITREO_JIT_IR_SSE` form mask
///
/// Split out of ir_frontend.cpp by pure code motion.

#include "ir_frontend_internal.hpp"

#include <cstdlib>

namespace vitreo::cpu::jit::ir {

/// Which instructions this increment can express, by exact decoded form.
///
/// Dispatching on the encoding-specific opcode rather than a coarse "is this
/// an ADD" is deliberate: the enumerator already says which operand shapes the
/// decoder produced, so an unhandled form cannot fall into a handled one. The
/// operand kinds are re-checked below anyway — this table is the first of two
/// gates, not the only one.
[[nodiscard]] Recipe classify(Opcode opcode) noexcept {
    switch (opcode) {
    // --- ADD ---
    case Opcode::ADD_EAX_IMM32:
    case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_RM32_IMM8:
    case Opcode::ADD_RM32_R32:
    case Opcode::ADD_R32_RM32:
        return {Form::AluRmw, Op::Add, FlagOp::Add32};

    // --- SUB ---
    case Opcode::SUB_EAX_IMM32:
    case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_RM32_IMM8:
    case Opcode::SUB_RM32_R32:
    case Opcode::SUB_R32_RM32:
        return {Form::AluRmw, Op::Sub, FlagOp::Sub32};

    // --- AND ---
    case Opcode::AND_EAX_IMM32:
    case Opcode::AND_RM32_IMM32:
    case Opcode::AND_RM32_IMM8:
    case Opcode::AND_RM32_R32:
    case Opcode::AND_R32_RM32:
        return {Form::AluRmw, Op::And, FlagOp::And32};

    // --- OR ---
    case Opcode::OR_EAX_IMM32:
    case Opcode::OR_RM32_IMM32:
    case Opcode::OR_RM32_IMM8:
    case Opcode::OR_RM32_R32:
    case Opcode::OR_R32_RM32:
        return {Form::AluRmw, Op::Or, FlagOp::Or32};

    // --- Shifts by an immediate ---
    case Opcode::SHL_RM32_1:
    case Opcode::SHL_RM32_IMM8:
        return {Form::ShiftImm, Op::Shl, FlagOp::Shl32, Type::I32};
    case Opcode::SHR_RM32_1:
    case Opcode::SHR_RM32_IMM8:
        return {Form::ShiftImm, Op::Shr, FlagOp::Shr32, Type::I32};
    case Opcode::SAR_RM32_1:
    case Opcode::SAR_RM32_IMM8:
        return {Form::ShiftImm, Op::Sar, FlagOp::Sar32, Type::I32};

    // --- 8/16-bit CMP and TEST ---
    //
    // Measured after the SSE tranche: 8- and 16-bit integer
    // forms are ~52% of all declines, and this subset — the ones needing no
    // partial-register WRITE — is about 36%.
    case Opcode::TEST_RM8_R8:
    case Opcode::TEST_RM8_IMM8:
    case Opcode::TEST_AL_IMM8:
        return {Form::NarrowCompare, Op::And, FlagOp::And8, Type::I8};
    case Opcode::TEST_RM16_R16:
    case Opcode::TEST_RM16_IMM16:
    case Opcode::TEST_AX_IMM16:
        return {Form::NarrowCompare, Op::And, FlagOp::And16, Type::I16};
    case Opcode::CMP_RM8_R8:
    case Opcode::CMP_R8_RM8:
    case Opcode::CMP_RM8_IMM8:
    case Opcode::CMP_AL_IMM8:
        return {Form::NarrowCompare, Op::Sub, FlagOp::Sub8, Type::I8};
    case Opcode::CMP_RM16_R16:
    case Opcode::CMP_R16_RM16:
    case Opcode::CMP_RM16_IMM8:
    case Opcode::CMP_RM16_IMM16:
    case Opcode::CMP_AX_IMM16:
        return {Form::NarrowCompare, Op::Sub, FlagOp::Sub16, Type::I16};

    // --- MOV at 8/16 bits (partial-register writes) ---
    case Opcode::MOV_R8_IMM8:
    case Opcode::MOV_RM8_IMM8:
    case Opcode::MOV_RM8_R8:
    case Opcode::MOV_R8_RM8:
        return {Form::MovNarrow, Op::GprWrite, FlagOp::None, Type::I8};
    case Opcode::MOV_R16_IMM16:
    case Opcode::MOV_RM16_IMM16:
    case Opcode::MOV_RM16_R16:
    case Opcode::MOV_R16_RM16:
        return {Form::MovNarrow, Op::GprWrite, FlagOp::None, Type::I16};

    // --- MOVZX / MOVSX into a 32-bit register ---
    case Opcode::MOVZX_R32_RM8:
        return {Form::Widen, Op::Zext, FlagOp::None, Type::I8};
    case Opcode::MOVZX_R32_RM16:
        return {Form::Widen, Op::Zext, FlagOp::None, Type::I16};
    case Opcode::MOVSX_R32_RM8:
        return {Form::Widen, Op::Sext, FlagOp::None, Type::I8};
    case Opcode::MOVSX_R32_RM16:
        return {Form::Widen, Op::Sext, FlagOp::None, Type::I16};

    // --- SSE (M9 first slice) ---
    //
    // These two are not an arbitrary starting point: measured on the load,
    // XORPS and MOVSS are the first blocker for 3664 and 2021 blocks
    // respectively — together 42% of all Tier 1 declines in that guest.
    case Opcode::XORPS_XMM_XMMM128:
        return {Form::XorpsRR, Op::VXor, FlagOp::None};
    case Opcode::MOVSS_XMM_XMMM32:
    case Opcode::MOVSS_XMMM32_XMM:
        return {Form::Movss, Op::VInsertLow32, FlagOp::None};
    // The whole SSE tail after XORPS/MOVSS: 148 declines, rank
    // 18. Register-register needs no new IR at all — read one XMM, write
    // another — which is the only reason it is worth taking at that size.
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPS_XMMM128_XMM:
        return {Form::MovapsRR, Op::XmmWrite, FlagOp::None};
    // `sse_enabled` below turns these two back into declines without giving up
    // Tier 1 entirely: VITREO_JIT_IR_OPS=1 keeps the ALU class and drops SSE.

    // --- XOR ---
    case Opcode::XOR_EAX_IMM32:
    case Opcode::XOR_RM32_IMM32:
    case Opcode::XOR_RM32_IMM8:
    case Opcode::XOR_RM32_R32:
    case Opcode::XOR_R32_RM32:
        return {Form::AluRmw, Op::Xor, FlagOp::Xor32};

    // --- CMP: SUB whose result is discarded ---
    case Opcode::CMP_EAX_IMM32:
    case Opcode::CMP_RM32_IMM32:
    case Opcode::CMP_RM32_IMM8:
    case Opcode::CMP_RM32_R32:
    case Opcode::CMP_R32_RM32:
        return {Form::AluCompare, Op::Sub, FlagOp::Sub32};

    // --- TEST: AND whose result is discarded ---
    case Opcode::TEST_EAX_IMM32:
    case Opcode::TEST_RM32_IMM32:
    case Opcode::TEST_RM32_R32:
        return {Form::AluCompare, Op::And, FlagOp::And32};

    // --- NOT: Intel SDM Vol. 2B, NOT — "Flags Affected: None." ---
    case Opcode::NOT_RM32:
        return {Form::Not, Op::Not, FlagOp::None};

    // --- NEG ---
    case Opcode::NEG_RM32:
        return {Form::Neg, Op::Neg, FlagOp::Neg32};

    // --- INC / DEC. The largest single blocker measured on real GUI code
    // (100 of 266 declines in Solitaire), and not a missing enumerator: both
    // PRESERVE CF, so their flag definition is a MERGE with the incoming flag
    // value rather than a fresh definition. ---
    case Opcode::INC_R32:
    case Opcode::INC_RM32:
        return {Form::IncDec, Op::Add, FlagOp::Inc32};
    case Opcode::DEC_R32:
    case Opcode::DEC_RM32:
        return {Form::IncDec, Op::Sub, FlagOp::Dec32};

    // --- MOV: no flags. The register and memory forms share these opcode
    // enumerators, so `Form::Mov` re-dispatches on the operand shape below.
    case Opcode::MOV_R32_IMM32:
    case Opcode::MOV_RM32_IMM32:
    case Opcode::MOV_RM32_R32:
    case Opcode::MOV_R32_RM32:
    // The A1/A3 absolute forms decode to an ordinary disp-only memory operand
    // in 32-bit mode, so they need no special handling beyond the enumerator.
    case Opcode::MOV_EAX_MOFFS32:
    case Opcode::MOV_MOFFS32_EAX:
        return {Form::Mov, Op::Add, FlagOp::None};

    // --- LEA: address arithmetic, no access, no flags ---
    case Opcode::LEA_R32_M:
        return {Form::Lea, Op::Lea, FlagOp::None};

    // --- Control flow (M7). Terminal forms only; an in-block forward Jcc is
    // NOT accepted, and that is load-bearing rather than incidental: the
    // flag-demand pass finds a definition's killer with a linear scan, which
    // is sound only because a terminator ends its block. See
    // `ir::is_terminator`.
    case Opcode::JMP_REL8:
    case Opcode::JMP_REL32:
        return {Form::Jmp, Op::Add, FlagOp::None};

    // Indirect jump — the single most common reason a real block declined.
    // The terminator already exists (`Op::JmpIndirect`, added for RET); only
    // the frontend had to learn the opcode.
    case Opcode::JMP_RM32:
        return {Form::JmpIndirect, Op::Add, FlagOp::None};

    // --- Stack. None of these touch flags (Intel SDM Vol. 2: PUSH, POP,
    // CALL and RET all list "Flags Affected: None"). ---
    case Opcode::PUSH_R32:
    case Opcode::PUSH_RM32:
    case Opcode::PUSH_IMM8:
    case Opcode::PUSH_IMM32:
        return {Form::Push, Op::Add, FlagOp::None};
    case Opcode::POP_R32:
        return {Form::Pop, Op::Add, FlagOp::None};
    case Opcode::CALL_REL32:
        return {Form::Call, Op::Add, FlagOp::None};
    case Opcode::CALL_RM32:
        return {Form::CallIndirect, Op::Add, FlagOp::None};
    case Opcode::RET_NEAR:
    case Opcode::RET_NEAR_IMM16:
        return {Form::Ret, Op::Add, FlagOp::None};
    case Opcode::LEAVE:
        return {Form::Leave, Op::Add, FlagOp::None};

    case Opcode::JO_REL8:   case Opcode::JNO_REL8:
    case Opcode::JB_REL8:   case Opcode::JNB_REL8:
    case Opcode::JZ_REL8:   case Opcode::JNZ_REL8:
    case Opcode::JBE_REL8:  case Opcode::JNBE_REL8:
    case Opcode::JS_REL8:   case Opcode::JNS_REL8:
    case Opcode::JP_REL8:   case Opcode::JNP_REL8:
    case Opcode::JL_REL8:   case Opcode::JNL_REL8:
    case Opcode::JLE_REL8:  case Opcode::JNLE_REL8:
    case Opcode::JO_REL32:  case Opcode::JNO_REL32:
    case Opcode::JB_REL32:  case Opcode::JNB_REL32:
    case Opcode::JZ_REL32:  case Opcode::JNZ_REL32:
    case Opcode::JBE_REL32: case Opcode::JNBE_REL32:
    case Opcode::JS_REL32:  case Opcode::JNS_REL32:
    case Opcode::JP_REL32:  case Opcode::JNP_REL32:
    case Opcode::JL_REL32:  case Opcode::JNL_REL32:
    case Opcode::JLE_REL32: case Opcode::JNLE_REL32:
        return {Form::Jcc, Op::Add, FlagOp::None};

    default:
        return {};
    }
}

/// x86 condition code for a Jcc opcode, or 0xFF.
///
/// The decoder's enumerators run in condition-code order — the same order as
/// the low nibble of the `70+cc` / `0F 80+cc` encodings — so this is
/// subtraction rather than a 32-case switch. The static_asserts are what make
/// that safe: an insertion or reorder in `opcodes.hpp` becomes a build failure
/// instead of silently branching on the wrong condition, which is a
/// wrong-result bug affecting one condition code out of sixteen.
[[nodiscard]] u8 jcc_condition(Opcode op) noexcept {
    static_assert(static_cast<int>(Opcode::JNO_REL8) ==
                  static_cast<int>(Opcode::JO_REL8) + 1);
    static_assert(static_cast<int>(Opcode::JZ_REL8) ==
                  static_cast<int>(Opcode::JO_REL8) + 4);
    static_assert(static_cast<int>(Opcode::JNLE_REL8) ==
                  static_cast<int>(Opcode::JO_REL8) + 15);
    static_assert(static_cast<int>(Opcode::JNO_REL32) ==
                  static_cast<int>(Opcode::JO_REL32) + 1);
    static_assert(static_cast<int>(Opcode::JZ_REL32) ==
                  static_cast<int>(Opcode::JO_REL32) + 4);
    static_assert(static_cast<int>(Opcode::JNLE_REL32) ==
                  static_cast<int>(Opcode::JO_REL32) + 15);

    const int v = static_cast<int>(op);
    const int rel8 = static_cast<int>(Opcode::JO_REL8);
    const int rel32 = static_cast<int>(Opcode::JO_REL32);
    if (v >= rel8 && v <= rel8 + 15) return static_cast<u8>(v - rel8);
    if (v >= rel32 && v <= rel32 + 15) return static_cast<u8>(v - rel32);
    return 0xFF;
}

namespace {

/// `VITREO_JIT_IR_SSE=0` drops the SSE forms without giving up Tier 1.
///
/// A frontend-local switch with its own variable, rather than a bit in
/// `VITREO_JIT_IR_OPS`: that mask lives on `JitCompiler`, and this file
/// deliberately does not depend on `jit_compiler.hpp` — that independence is
/// what lets the IR be unit-tested without a JitCompiler at all. Duplicating
/// the mask parse here to avoid the include would be the worse trade, since a
/// duplicated parse is the thing that silently disagrees later.
///
/// Cached once: the gate cannot change mid-process, and the frontend must not
/// pay a getenv per instruction.
[[nodiscard]] u32 sse_form_mask() noexcept {
    static const u32 mask = [] {
        const char* v = std::getenv("VITREO_JIT_IR_SSE");
        if (v == nullptr || *v == '\0') return 15u;  // every SSE form
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(v, &end, 0);
        if (end == v || *end != '\0') return 15u;     // fail open, as passes do
        return static_cast<u32>(parsed);
    }();
    return mask;
}

}  // namespace

/// Bit 0 = XORPS, bit 1 = MOVSS, bit 2 = MOVAPS, bit 3 = the long-mode
/// register-form tranche. A mask rather than a boolean
/// because when an
/// SSE lowering is suspected of breaking a guest, "which of the two" is the
/// first question and a 50-second game launch is the measurement.
[[nodiscard]] bool sse_form_enabled(u32 bit) noexcept {
    return (sse_form_mask() & bit) != 0;
}

}  // namespace vitreo::cpu::jit::ir
