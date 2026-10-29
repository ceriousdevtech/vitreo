// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <array>
#include <cstdlib>
#include <cstdio>
#include "flag_liveness.hpp"
#include <vitreo/common.hpp>

#include "../decode/opcodes.hpp"
#include "../state/flag_state.hpp"

namespace vitreo::cpu::jit {
namespace {

enum class Effect : u8 { Neutral = 0, FullWriter, Barrier };

bool starts_with(const char* s, const char* pfx) noexcept {
    if (!s || !pfx) return false;
    while (*pfx) {
        if (*s++ != *pfx++) return false;
    }
    return true;
}

// Proven-nonzero immediate / by-1 shifts are full-flag writers (CF/OF/SF/ZF/
// AF/PF all redefined). CL-count shifts are barriers: a runtime count of 0
// leaves flags unchanged, so treating them as killers would wrongly omit a
// prior ADD before Jcc when CL happens to be 0.
bool is_shift_full_writer(const DecodedInstruction& instr) noexcept {
    const Opcode op = instr.opcode;
    const u64 imm = instr.operand_count > 1 ? instr.operands[1].imm : 0;

    switch (op) {
    case Opcode::SHL_RM8_1:
    case Opcode::SHL_RM16_1:
    case Opcode::SHL_RM32_1:
    case Opcode::SHL_RM64_1:
    case Opcode::SHR_RM8_1:
    case Opcode::SHR_RM16_1:
    case Opcode::SHR_RM32_1:
    case Opcode::SHR_RM64_1:
    case Opcode::SAR_RM8_1:
    case Opcode::SAR_RM16_1:
    case Opcode::SAR_RM32_1:
    case Opcode::SAR_RM64_1:
        return true;

    case Opcode::SHL_RM8_IMM8:
    case Opcode::SHL_RM16_IMM8:
    case Opcode::SHL_RM32_IMM8:
    case Opcode::SHR_RM8_IMM8:
    case Opcode::SHR_RM16_IMM8:
    case Opcode::SHR_RM32_IMM8:
    case Opcode::SAR_RM8_IMM8:
    case Opcode::SAR_RM16_IMM8:
    case Opcode::SAR_RM32_IMM8:
        // 8/16/32-bit shift counts mask to 5 bits. Imm that masks to 0 does
        // not touch flags — Neutral, not a killer.
        return (imm & 31u) != 0;

    case Opcode::SHL_RM64_IMM8:
    case Opcode::SHR_RM64_IMM8:
    case Opcode::SAR_RM64_IMM8:
        return (imm & 63u) != 0;

    default:
        return false;
    }
}

bool is_shift_cl(Opcode op) noexcept {
    switch (op) {
    case Opcode::SHL_RM8_CL:
    case Opcode::SHL_RM16_CL:
    case Opcode::SHL_RM32_CL:
    case Opcode::SHL_RM64_CL:
    case Opcode::SHR_RM8_CL:
    case Opcode::SHR_RM16_CL:
    case Opcode::SHR_RM32_CL:
    case Opcode::SHR_RM64_CL:
    case Opcode::SAR_RM8_CL:
    case Opcode::SAR_RM16_CL:
    case Opcode::SAR_RM32_CL:
    case Opcode::SAR_RM64_CL:
        return true;
    default:
        return false;
    }
}

// opcode_name() returns human-readable strings ("ADD r/m32, r32"), not enum
// spellings. Match on those forms. Trailing space / digit boundaries keep
// ADC from matching ADD and SBB from matching SUB.
//
// Everything here is a function of the opcode alone; the one operand-
// dependent rule (a masked-nonzero shift immediate) is applied by classify()
// before it consults the per-opcode table built from this.
Effect classify_opcode(Opcode op) noexcept {
    if (is_shift_cl(op)) return Effect::Barrier;

    // Enum-range classify for ALU full-flag writers. Prefer this over
    // opcode_name() string matching: several logical opcodes historically
    // returned "?" from opcode_name(), which made AND a silent Barrier and
    // defeated dead-flag omission on SHR;AND;Jcc style blocks.
    if ((op >= Opcode::ADD_AL_IMM8 && op <= Opcode::ADD_R64_RM64) ||
        (op >= Opcode::SUB_AL_IMM8 && op <= Opcode::SUB_R64_RM64) ||
        (op >= Opcode::CMP_AL_IMM8 && op <= Opcode::CMP_R64_RM64) ||
        (op >= Opcode::AND_AL_IMM8 && op <= Opcode::AND_R64_RM64) ||
        (op >= Opcode::OR_AL_IMM8 && op <= Opcode::OR_R64_RM64) ||
        (op >= Opcode::XOR_AL_IMM8 && op <= Opcode::XOR_R64_RM64) ||
        (op >= Opcode::TEST_AL_IMM8 && op <= Opcode::TEST_RM64_R64)) {
        return Effect::FullWriter;
    }

    // COMISS/UCOMISS/COMISD/UCOMISD (and their VEX forms, which share the
    // opcode) read no flag and write all six: ZF/PF/CF from the compare,
    // OF/SF/AF cleared (Intel SDM Vol. 2). A memory form can still fault
    // first; the scan treats every memory-operand killer that way.
    if (op == Opcode::COMISS_XMM_XMMM32 || op == Opcode::UCOMISS_XMM_XMMM32 ||
        op == Opcode::COMISD_XMM_XMMM64 || op == Opcode::UCOMISD_XMM_XMMM64) {
        return Effect::FullWriter;
    }

    const char* n = opcode_name(op);
    if (!n || n[0] == '?') return Effect::Barrier;

    // Full-flag writers — string fallback for any ALU forms outside the
    // contiguous ranges above (and for readability of older call sites).
    if (starts_with(n, "ADD ") || starts_with(n, "SUB ") ||
        starts_with(n, "AND ") || starts_with(n, "OR ") ||
        starts_with(n, "XOR ") || starts_with(n, "CMP ") ||
        starts_with(n, "TEST ")) {
        return Effect::FullWriter;
    }

    // Flag readers / partial-flag ops / control flow — barriers.
    // Jcc names are "JO"/"JZ"/… without a space; JMP is "JMP …".
    // Imm-0 shifts fall through past the SHL/SHR/SAR name match below into
    // Neutral only when is_shift_full_writer returned false AND we do not
    // treat them as barriers here — they are handled by returning Neutral
    // for known shift opcodes after the barrier list, via the default path
    // that still matches "SHL "/"SHR "/"SAR " as Barrier for rotates etc.
    // Imm-0 SHL/SHR/SAR must stay Neutral (flags unchanged). Detect them
    // before the generic shift barrier names.
    if (op == Opcode::SHL_RM8_IMM8 || op == Opcode::SHL_RM16_IMM8 ||
        op == Opcode::SHL_RM32_IMM8 || op == Opcode::SHL_RM64_IMM8 ||
        op == Opcode::SHR_RM8_IMM8 || op == Opcode::SHR_RM16_IMM8 ||
        op == Opcode::SHR_RM32_IMM8 || op == Opcode::SHR_RM64_IMM8 ||
        op == Opcode::SAR_RM8_IMM8 || op == Opcode::SAR_RM16_IMM8 ||
        op == Opcode::SAR_RM32_IMM8 || op == Opcode::SAR_RM64_IMM8) {
        // Imm masked to 0: flag-neutral.
        return Effect::Neutral;
    }

    if (starts_with(n, "J") ||  // Jcc, JMP, JCXZ, …
        starts_with(n, "SET") ||
        starts_with(n, "CMOV") ||
        starts_with(n, "ADC ") ||
        starts_with(n, "SBB ") ||
        starts_with(n, "INC ") ||
        starts_with(n, "DEC ") ||
        starts_with(n, "BT ") || starts_with(n, "BTS ") ||
        starts_with(n, "BTR ") || starts_with(n, "BTC ") ||
        starts_with(n, "SHL ") || starts_with(n, "SHR ") ||
        starts_with(n, "SAR ") || starts_with(n, "SAL ") ||
        starts_with(n, "ROL ") || starts_with(n, "ROR ") ||
        starts_with(n, "RCL ") || starts_with(n, "RCR ") ||
        starts_with(n, "MUL ") || starts_with(n, "IMUL ") ||
        starts_with(n, "DIV ") || starts_with(n, "IDIV ") ||
        starts_with(n, "NEG ") ||
        starts_with(n, "PUSHF") || starts_with(n, "POPF") ||
        starts_with(n, "LAHF") || starts_with(n, "SAHF") ||
        starts_with(n, "CLC") || starts_with(n, "STC") ||
        starts_with(n, "CMC") ||
        starts_with(n, "LOOP") ||
        starts_with(n, "CALL") || starts_with(n, "RET") ||
        starts_with(n, "INT") || starts_with(n, "IRET") ||
        starts_with(n, "SYSCALL") || starts_with(n, "SYSENTER") ||
        starts_with(n, "HLT") || starts_with(n, "UD2") ||
        starts_with(n, "CPUID") ||
        starts_with(n, "CMPS") || starts_with(n, "SCAS")) {
        return Effect::Barrier;
    }

    // Flag-neutral data movement / SSE compute that does not touch EFLAGS.
    // FLD/FST/FSTP (and FSTSW/FSTCW) do not write EFLAGS (Intel SDM Vol. 2).
    if (starts_with(n, "FLD") || starts_with(n, "FST")) {
        return Effect::Neutral;
    }
    if (starts_with(n, "MOV") || starts_with(n, "LEA") ||
        starts_with(n, "NOP") || starts_with(n, "PAUSE") ||
        starts_with(n, "PUSH ") || starts_with(n, "POP ") ||
        starts_with(n, "XCHG") || starts_with(n, "CDQ") ||
        starts_with(n, "CDQE") || starts_with(n, "CBW") ||
        starts_with(n, "CWD") || starts_with(n, "CWDE") ||
        starts_with(n, "CQO") ||
        starts_with(n, "BSWAP") ||
        starts_with(n, "PUNPCK") || starts_with(n, "PSHU") ||
        starts_with(n, "PSLL") || starts_with(n, "PSRL") ||
        starts_with(n, "PSRA") || starts_with(n, "PADD") ||
        starts_with(n, "PSUB") || starts_with(n, "PMUL") ||
        starts_with(n, "PAND") || starts_with(n, "POR") ||
        starts_with(n, "PXOR") || starts_with(n, "PCMPEQ") ||
        starts_with(n, "PCMPGT") || starts_with(n, "PEXTR") ||
        starts_with(n, "PINSR") ||
        starts_with(n, "ADDSS") || starts_with(n, "ADDSD") ||
        starts_with(n, "ADDPS") || starts_with(n, "ADDPD") ||
        starts_with(n, "SUBSS") || starts_with(n, "SUBSD") ||
        starts_with(n, "SUBPS") || starts_with(n, "SUBPD") ||
        starts_with(n, "MULSS") || starts_with(n, "MULSD") ||
        starts_with(n, "MULPS") || starts_with(n, "MULPD") ||
        starts_with(n, "DIVSS") || starts_with(n, "DIVSD") ||
        starts_with(n, "DIVPS") || starts_with(n, "DIVPD") ||
        starts_with(n, "MINSS") || starts_with(n, "MINSD") ||
        starts_with(n, "MINPS") || starts_with(n, "MINPD") ||
        starts_with(n, "MAXSS") || starts_with(n, "MAXSD") ||
        starts_with(n, "MAXPS") || starts_with(n, "MAXPD") ||
        starts_with(n, "RSQRT") || starts_with(n, "RCPPS") ||
        starts_with(n, "RCPSS") ||
        starts_with(n, "SHUFPS") || starts_with(n, "SHUFPD") ||
        starts_with(n, "CMPPS") || starts_with(n, "CMPPD") ||
        starts_with(n, "MOVMSK") ||
        starts_with(n, "HADD") || starts_with(n, "HSUB") ||
        starts_with(n, "UNPCK") ||
        starts_with(n, "CVT") || starts_with(n, "SQRT") ||
        starts_with(n, "ANDPS") || starts_with(n, "ANDPD") ||
        starts_with(n, "ANDNPS") || starts_with(n, "ANDNPD") ||
        starts_with(n, "ORPS") || starts_with(n, "XORPS") ||
        starts_with(n, "XORPD")) {
        return Effect::Neutral;
    }

    return Effect::Barrier;
}

// The string matching above costs about as much as interpreting a simple
// SIMD instruction, and jit_interpret_decoded asks on every delegated
// execution with flags pending. Build the per-opcode answer once.
Effect classify(const DecodedInstruction& instr) noexcept {
    if (is_shift_full_writer(instr)) return Effect::FullWriter;
    static const auto table = [] {
        std::array<Effect, opcode_count()> t{};
        for (usize i = 0; i < t.size(); ++i) {
            t[i] = classify_opcode(static_cast<Opcode>(i));
        }
        return t;
    }();
    const auto index = static_cast<usize>(instr.opcode);
    return index < table.size() ? table[index] : Effect::Barrier;
}

// Writer kinds that publish host NZCV (m_flags.nzcv != None). OR/XOR 32 and
// CMP/SUB 16 do not — omitting those would starve a reconstruct.
enum class PubKind : u8 { None, Sub, And, Add };

PubKind publisher_kind(Opcode op) noexcept {
    if ((op >= Opcode::CMP_AL_IMM8 && op <= Opcode::CMP_R64_RM64) ||
        (op >= Opcode::SUB_AL_IMM8 && op <= Opcode::SUB_R64_RM64)) {
        // 8/16-bit CMP/SUB often emit SUB (not SUBS) and leave m_flags.nzcv None.
        if (op == Opcode::CMP_AL_IMM8 || op == Opcode::CMP_AX_IMM16 ||
            op == Opcode::CMP_RM8_IMM8 || op == Opcode::CMP_RM16_IMM16 ||
            op == Opcode::CMP_RM16_IMM8 || op == Opcode::CMP_RM8_R8 ||
            op == Opcode::CMP_RM16_R16 || op == Opcode::CMP_R8_RM8 ||
            op == Opcode::CMP_R16_RM16 ||
            op == Opcode::SUB_AL_IMM8 || op == Opcode::SUB_AX_IMM16 ||
            op == Opcode::SUB_RM8_IMM8 || op == Opcode::SUB_RM16_IMM16 ||
            op == Opcode::SUB_RM16_IMM8 || op == Opcode::SUB_RM8_R8 ||
            op == Opcode::SUB_RM16_R16 || op == Opcode::SUB_R8_RM8 ||
            op == Opcode::SUB_R16_RM16) {
            return PubKind::None;
        }
        return PubKind::Sub;
    }
    if (op >= Opcode::TEST_AL_IMM8 && op <= Opcode::TEST_RM64_R64) {
        return PubKind::And;
    }
    if (op >= Opcode::AND_AL_IMM8 && op <= Opcode::AND_R64_RM64) {
        if (op == Opcode::AND_AL_IMM8 || op == Opcode::AND_AX_IMM16 ||
            op == Opcode::AND_RM8_IMM8 || op == Opcode::AND_RM16_IMM16 ||
            op == Opcode::AND_RM16_IMM8 || op == Opcode::AND_RM8_R8 ||
            op == Opcode::AND_RM16_R16 || op == Opcode::AND_R8_RM8 ||
            op == Opcode::AND_R16_RM16) {
            return PubKind::None;
        }
        return PubKind::And;
    }
    if (op >= Opcode::ADD_AL_IMM8 && op <= Opcode::ADD_R64_RM64) {
        if (op == Opcode::ADD_AL_IMM8 || op == Opcode::ADD_AX_IMM16 ||
            op == Opcode::ADD_RM8_IMM8 || op == Opcode::ADD_RM16_IMM16 ||
            op == Opcode::ADD_RM16_IMM8 || op == Opcode::ADD_RM8_R8 ||
            op == Opcode::ADD_RM16_R16 || op == Opcode::ADD_R8_RM8 ||
            op == Opcode::ADD_R16_RM16) {
            return PubKind::None;
        }
        return PubKind::Add;
    }
    return PubKind::None;
}

/// True when a forward liveness scan may cross `instr` without giving up on
/// omitting an earlier publisher's lazy record.
///
/// `Effect::Barrier` is one bucket holding three unrelated things: genuine
/// flag READERS (ADC/SBB/RCL/RCR/Jcc/SETcc/CMOV/LAHF/PUSHF/CMC/LOOP), partial
/// writers that read nothing (ROL/ROR, CL-count shifts, BT*, MUL/IMUL), and
/// control flow or anything unrecognised. Only the readers have to stop the
/// scan. Lumping the second group in with them is why a straight-line block of
/// mixed ALU work omits nothing at all: in tools/stress codefoot, XOR's scan
/// died on the following IMUL and SUB's on the following ROL, so every one of
/// the block's six full-flag writers published a record no one reads.
///
/// What makes crossing sound for this group, in the order it matters:
///
///  * They do not READ flags, so they cannot observe the record being omitted.
///  * They cannot FAULT, so no exception record can observe it either. This is
///    only true of register/immediate forms -- which is already guaranteed
///    here, because the scan breaks on instruction_touches_memory() before it
///    ever reaches this predicate. DIV/IDIV are excluded regardless: #DE makes
///    them faulting even register-only.
///  * They may PRESERVE some of the publisher's flags (ROL/ROR keep SF/ZF/AF/
///    PF; MUL/IMUL leave them undefined), and after an omission those become
///    stale. That is unobservable only because the scan still requires a later
///    full writer that redefines everything, with no reader in between -- the
///    same condition that justified omitting the record in the first place.
///
/// INC/DEC are deliberately NOT here even though they read no flag a Jcc
/// would: they preserve CF, and the emitter materialises that CF at publish
/// time (emit_set_lazy_flags_preserve_cf packs it as `(CF << 32) | src`). That
/// makes them consumers of the pending record in a way the list above is not.
///
/// Matched on the opcode ENUM, never on opcode_name(). This used to test name
/// prefixes ("IMUL ", "ROL ", "BT "), but opcode_name() falls back to the enum
/// identifier ("IMUL_R32_RM32") for every opcode without a hand-written
/// spelling, which is all of MUL/IMUL/BT* and all but two rotates -- so the
/// rule silently matched almost nothing, and `shr ecx,11; imul ecx,edx;
/// cmp eax,ecx` (an LZMA range decoder's bit step) published the SHR record
/// on every bit.
bool crossable_partial_writer(const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    // Rotates: write CF (and OF for count 1), preserve SF/ZF/AF/PF; a masked
    // count of zero writes nothing. RCL/RCR are excluded -- they rotate
    // THROUGH carry, so they read CF. Intel SDM Vol. 2B RCL/RCR/ROL/ROR.
    case Opcode::ROL_RM8_1:   case Opcode::ROL_RM16_1:
    case Opcode::ROL_RM32_1:  case Opcode::ROL_RM64_1:
    case Opcode::ROL_RM8_CL:  case Opcode::ROL_RM16_CL:
    case Opcode::ROL_RM32_CL: case Opcode::ROL_RM64_CL:
    case Opcode::ROL_RM8_IMM8:  case Opcode::ROL_RM16_IMM8:
    case Opcode::ROL_RM32_IMM8: case Opcode::ROL_RM64_IMM8:
    case Opcode::ROR_RM8_1:   case Opcode::ROR_RM16_1:
    case Opcode::ROR_RM32_1:  case Opcode::ROR_RM64_1:
    case Opcode::ROR_RM8_CL:  case Opcode::ROR_RM16_CL:
    case Opcode::ROR_RM32_CL: case Opcode::ROR_RM64_CL:
    case Opcode::ROR_RM8_IMM8:  case Opcode::ROR_RM16_IMM8:
    case Opcode::ROR_RM32_IMM8: case Opcode::ROR_RM64_IMM8:
    // BT/BTS/BTR/BTC write CF from the selected bit, leave OF/SF/AF/PF
    // undefined and read no flag. Intel SDM Vol. 2A BT/BTC/BTR/BTS.
    case Opcode::BT_RM16_R16:  case Opcode::BT_RM32_R32:
    case Opcode::BT_RM64_R64:  case Opcode::BT_RM16_IMM8:
    case Opcode::BT_RM32_IMM8: case Opcode::BT_RM64_IMM8:
    case Opcode::BTS_RM16_R16:  case Opcode::BTS_RM32_R32:
    case Opcode::BTS_RM64_R64:  case Opcode::BTS_RM16_IMM8:
    case Opcode::BTS_RM32_IMM8: case Opcode::BTS_RM64_IMM8:
    case Opcode::BTR_RM16_R16:  case Opcode::BTR_RM32_R32:
    case Opcode::BTR_RM64_R64:  case Opcode::BTR_RM16_IMM8:
    case Opcode::BTR_RM32_IMM8: case Opcode::BTR_RM64_IMM8:
    case Opcode::BTC_RM16_R16:  case Opcode::BTC_RM32_R32:
    case Opcode::BTC_RM64_R64:  case Opcode::BTC_RM16_IMM8:
    case Opcode::BTC_RM32_IMM8: case Opcode::BTC_RM64_IMM8:
    // MUL/IMUL write CF/OF and leave SF/ZF/AF/PF undefined; they read none.
    // DIV/IDIV are NOT crossable: #DE. Intel SDM Vol. 2A MUL/IMUL.
    case Opcode::MUL_RM8:  case Opcode::MUL_RM16:
    case Opcode::MUL_RM32: case Opcode::MUL_RM64:
    case Opcode::IMUL_RM8:  case Opcode::IMUL_RM16:
    case Opcode::IMUL_RM32: case Opcode::IMUL_RM64:
    case Opcode::IMUL_R16_RM16: case Opcode::IMUL_R32_RM32:
    case Opcode::IMUL_R64_RM64:
    case Opcode::IMUL_R16_RM16_IMM8:  case Opcode::IMUL_R32_RM32_IMM8:
    case Opcode::IMUL_R64_RM64_IMM8:
    case Opcode::IMUL_R16_RM16_IMM16: case Opcode::IMUL_R32_RM32_IMM32:
    case Opcode::IMUL_R64_RM64_IMM32:
        return true;
    default:
        // CL-count shifts: a runtime count of 0 preserves every flag, which
        // is why they are not full writers. They still read none.
        return is_shift_cl(instr.opcode);
    }
}

/// The INC/DEC forms whose long-mode lowering (unary64.cpp) skips both the
/// carry capture and the record when within-block liveness proved the record
/// dead. Only these may be crossed by a scan that elides an earlier record.
bool long_mode_incdec(Opcode op) noexcept {
    return op == Opcode::INC_RM32 || op == Opcode::DEC_RM32 ||
           op == Opcode::INC_RM64 || op == Opcode::DEC_RM64;
}

/// A register-to-register or immediate-to-register MOV/MOVZX/MOVSX/MOVSXD:
/// writes no flag, reads none, and with no memory operand cannot fault or
/// leave the block (Intel SDM Vol. 2B MOV/MOVZX/MOVSX/MOVSXD: Flags Affected
/// None). The INC/DEC killer scan crosses exactly these.
bool plain_register_move(const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::MOV_R8_IMM8:   case Opcode::MOV_R16_IMM16:
    case Opcode::MOV_R32_IMM32: case Opcode::MOV_R64_IMM64:
    case Opcode::MOV_RM8_R8:    case Opcode::MOV_RM16_R16:
    case Opcode::MOV_RM32_R32:  case Opcode::MOV_RM64_R64:
    case Opcode::MOV_R8_RM8:    case Opcode::MOV_R16_RM16:
    case Opcode::MOV_R32_RM32:  case Opcode::MOV_R64_RM64:
    case Opcode::MOV_RM8_IMM8:  case Opcode::MOV_RM16_IMM16:
    case Opcode::MOV_RM32_IMM32: case Opcode::MOV_RM64_IMM32:
    case Opcode::MOVZX_R16_RM8:  case Opcode::MOVZX_R32_RM8:
    case Opcode::MOVZX_R64_RM8:  case Opcode::MOVZX_R32_RM16:
    case Opcode::MOVZX_R64_RM16:
    case Opcode::MOVSX_R16_RM8:  case Opcode::MOVSX_R32_RM8:
    case Opcode::MOVSX_R64_RM8:  case Opcode::MOVSX_R32_RM16:
    case Opcode::MOVSX_R64_RM16: case Opcode::MOVSXD_R64_RM32:
        break;
    default:
        return false;
    }
    for (u8 i = 0; i < instr.operand_count; ++i) {
        if (!is_register_operand(instr.operands[i].type) &&
            !is_immediate_operand(instr.operands[i].type)) {
            return false;
        }
    }
    return true;
}

/// The IMUL forms whose long-mode lowering (mul_div64.cpp) publishes a lazy
/// record of their own and skips it when within-block liveness proved it dead.
bool long_mode_imul_publisher(Opcode op) noexcept {
    return op == Opcode::IMUL_R32_RM32 || op == Opcode::IMUL_R32_RM32_IMM8 ||
           op == Opcode::IMUL_R32_RM32_IMM32 || op == Opcode::IMUL_R64_RM64 ||
           op == Opcode::IMUL_R64_RM64_IMM8 || op == Opcode::IMUL_R64_RM64_IMM32;
}

/// True when the instruction at `j` is a long-mode register INC/DEC whose own
/// record the INC/DEC rule of compute_omit_lazy_flags proved dead.
bool incdec_record_dead(const BasicBlock& block, const std::vector<u8>& omit,
                        u32 j) noexcept {
    const DecodedInstruction& in = block.instructions[j];
    return block.is_64bit && long_mode_incdec(in.opcode) &&
           in.operand_count == 1 && is_register_operand(in.operands[0].type) &&
           !in.prefixes.lock && j < omit.size() && (omit[j] & 3u) != 0;
}

int jcc_cc(Opcode op) noexcept {
    switch (op) {
    case Opcode::JO_REL8: case Opcode::JO_REL32: return 0x0;
    case Opcode::JNO_REL8: case Opcode::JNO_REL32: return 0x1;
    case Opcode::JB_REL8: case Opcode::JB_REL32: return 0x2;
    case Opcode::JNB_REL8: case Opcode::JNB_REL32: return 0x3;
    case Opcode::JZ_REL8: case Opcode::JZ_REL32: return 0x4;
    case Opcode::JNZ_REL8: case Opcode::JNZ_REL32: return 0x5;
    case Opcode::JBE_REL8: case Opcode::JBE_REL32: return 0x6;
    case Opcode::JNBE_REL8: case Opcode::JNBE_REL32: return 0x7;
    case Opcode::JS_REL8: case Opcode::JS_REL32: return 0x8;
    case Opcode::JNS_REL8: case Opcode::JNS_REL32: return 0x9;
    case Opcode::JP_REL8: case Opcode::JP_REL32: return 0xA;
    case Opcode::JNP_REL8: case Opcode::JNP_REL32: return 0xB;
    case Opcode::JL_REL8: case Opcode::JL_REL32: return 0xC;
    case Opcode::JNL_REL8: case Opcode::JNL_REL32: return 0xD;
    case Opcode::JLE_REL8: case Opcode::JLE_REL32: return 0xE;
    case Opcode::JNLE_REL8: case Opcode::JNLE_REL32: return 0xF;
    default: return -1;
    }
}

/// The x86 condition a register-form SETcc or CMOVcc reads, or -1. Both are
/// flag READERS that write no flag and, with no memory operand, cannot fault
/// (Intel SDM Vol. 2 SETcc / CMOVcc: Flags Affected = None). When the
/// condition is one the publisher's host NZCV answers (jcc_fuses_from_publisher)
/// the lowering evaluates it from NZCV -- CSET / CSEL -- and never reads the
/// lazy record, exactly like a fused Jcc, but with no second control-flow
/// edge. A lowering that does fall back to the record while the record is
/// elided is caught at emission (elided spans) and the block is recompiled
/// without omission, so the analysis may cross these optimistically.
int nzcv_reader_cc(const DecodedInstruction& instr) noexcept {
    static_assert(static_cast<int>(Opcode::SETNLE_RM8) -
                      static_cast<int>(Opcode::SETO_RM8) == 15);
    static_assert(static_cast<int>(Opcode::CMOVNLE_R64_RM64) -
                      static_cast<int>(Opcode::CMOVO_R16_RM16) == 47);
    static_assert(static_cast<int>(Opcode::CMOVZ_R32_RM32) -
                      static_cast<int>(Opcode::CMOVO_R16_RM16) == 4 * 3 + 1);
    const int op = static_cast<int>(instr.opcode);
    if (instr.operand_count < 1) return -1;
    if (op >= static_cast<int>(Opcode::SETO_RM8) &&
        op <= static_cast<int>(Opcode::SETNLE_RM8)) {
        if (instr.operands[0].type != OperandType::Reg8) return -1;
        return op - static_cast<int>(Opcode::SETO_RM8);
    }
    if (op >= static_cast<int>(Opcode::CMOVO_R16_RM16) &&
        op <= static_cast<int>(Opcode::CMOVNLE_R64_RM64)) {
        if (instr.operand_count < 2 ||
            !is_register_operand(instr.operands[1].type)) {
            return -1;
        }
        return (op - static_cast<int>(Opcode::CMOVO_R16_RM16)) / 3;
    }
    return -1;
}

/// An 8- or 16-bit TEST is an And-kind publisher for the Jcc rules above,
/// but its lowering leaves no host NZCV (a byte ANDS would put x86 SF in
/// bit 31, not bit 7), so a SETcc / CMOVcc after it reads the record.
bool narrow_test(Opcode op) noexcept {
    return op == Opcode::TEST_AL_IMM8 || op == Opcode::TEST_RM8_IMM8 ||
           op == Opcode::TEST_RM8_R8 || op == Opcode::TEST_AX_IMM16 ||
           op == Opcode::TEST_RM16_IMM16 || op == Opcode::TEST_RM16_R16;
}

bool jcc_fuses_from_publisher(PubKind kind, int cc) noexcept {
    if (cc < 0 || cc == 0xA || cc == 0xB) return false;
    if (kind == PubKind::And) {
        return cc != 0x2 && cc != 0x3 && cc != 0x6 && cc != 0x7;
    }
    if (kind == PubKind::Add) {
        return cc != 0x6 && cc != 0x7;
    }
    return kind == PubKind::Sub;
}

}  // namespace

/// True for any ADC/SBB form. Both ranges are contiguous in Opcode.
bool is_adc_sbb(Opcode op) noexcept {
    return (op >= Opcode::ADC_AL_IMM8 && op <= Opcode::ADC_R64_RM64) ||
           (op >= Opcode::SBB_AL_IMM8 && op <= Opcode::SBB_R64_RM64);
}

bool has_memory_operand(const DecodedInstruction& instr) noexcept {
    for (u8 i = 0; i < instr.operand_count; ++i) {
        if (is_memory_operand(instr.operands[i].type)) return true;
    }
    return false;
}

/// An ADC/SBB immediately after an Add/Sub-kind publisher retires that
/// publisher's lazy record, even though ADC/SBB read CF and are Barriers
/// everywhere else in this file.
///
/// Why it is sound, and why ONLY in this exact shape:
///   * ADC/SBB write all six arithmetic flags, so the publisher's AF and PF
///     -- the two the NZCV kind bits cannot carry -- are dead.
///   * The CF they read comes from PSTATE, not from the record. Both the
///     chain fast path (ops64.cpp, `in_add`/`in_sub`) and the shared
///     emit_lazy_cf_to_call_tmp take `m_flags.incoming_nzcv` before ever consulting
///     FlagOp, and adjacency is what makes `m_flags.incoming_nzcv` that publisher's
///     kind. One instruction further away and that is no longer true.
///   * Neither may touch memory: a faulting access would expose the flags we
///     are discarding, and a memory-destination publisher is not reliably an
///     NZCV publisher either.
///
/// It is also SELF-CORRECTING in the one place liveness cannot see: whether
/// the consumer takes the chain fast path depends on the destination being
/// pinned, which is not known here. When it does not, the ADC/SBB slow path
/// publishes its own record unconditionally (ops64.cpp) -- so a downstream
/// FlagOp dispatch still finds a valid record and the omission simply buys
/// nothing on that path.
///
/// The omission is marked 2, not 1: the publisher must still clear the CF/OF
/// overlay it would have cleared as part of the record, or a stale CLC/STC
/// overlay outlives its owner and wins over the PSTATE carry.
/// See the recorded overlay-ordering bug for why that is not theoretical.
bool adc_sbb_retires_publisher(const DecodedInstruction& pub, PubKind kind,
                               const DecodedInstruction& consumer) noexcept {
    if (kind != PubKind::Add && kind != PubKind::Sub) return false;
    if (!is_adc_sbb(consumer.opcode)) return false;
    return !has_memory_operand(pub) && !has_memory_operand(consumer);
}

bool pending_flags_dead_across(const DecodedInstruction& instr) noexcept {
    if (classify(instr) != Effect::FullWriter) return false;
    // A faulting access would expose the flags we are about to discard.
    for (u8 i = 0; i < instr.operand_count; ++i) {
        if (is_memory_operand(instr.operands[i].type)) return false;
    }
    return true;
}

bool is_jcc_opcode(Opcode op) noexcept { return jcc_cc(op) >= 0; }

FlagEffect classify_flag_effect(const DecodedInstruction& instr) noexcept {
    switch (classify(instr)) {
    case Effect::Neutral:    return FlagEffect::Neutral;
    case Effect::FullWriter: return FlagEffect::FullWriter;
    case Effect::Barrier:    break;
    }
    return FlagEffect::Barrier;
}

bool instruction_touches_memory(const DecodedInstruction& instr) noexcept {
    for (u8 i = 0; i < instr.operand_count; ++i) {
        if (is_memory_operand(instr.operands[i].type)) return true;
    }
    return false;
}

/// **DELIBERATELY UNSOUND MEASUREMENT PROBE.** Off unless
/// `VITREO_JIT_OMIT_LIVEOUT` names a terminator class.
///
/// `VITREO_JIT_OMIT_ALL` answers "what is every lazy-flag publication worth"
/// (measured -33% on sc_blocksize bsz1). It cannot say which publications to
/// attack, because a sound analysis can only omit a record whose successors it
/// can see. This splits the ceiling by exactly that line:
///
///   indirect  the block ends in RET / JMP r/m / CALL r/m, so the successor
///             has no compile-time identity and no scan can ever reach it.
///             A sound analysis needs something other than a scan here.
///   direct    the block ends in Jcc / JMP rel / CALL rel, whose successors
///             ARE known. This is the share a cross-block scan could pursue.
///
/// Both arms omit only the record that is still live at the terminator -- the
/// last surviving definition -- so the intra-block analysis above is left
/// exactly as it is and the arms differ only in the bucket they empty.
void probe_omit_liveout(const BasicBlock& block,
                        const std::vector<Effect>& fx,
                        std::vector<u8>& omit) noexcept {
    enum class Bucket { Off, Indirect, Direct, Both };
    static const Bucket bucket = [] {
        const char* v = std::getenv("VITREO_JIT_OMIT_LIVEOUT");
        if (v == nullptr || *v == '\0') return Bucket::Off;
        const std::string_view s{v};
        if (s == "indirect") return Bucket::Indirect;
        if (s == "direct") return Bucket::Direct;
        if (s == "both" || s == "1") return Bucket::Both;
        return Bucket::Off;
    }();
    if (bucket == Bucket::Off) return;

    const u32 n = block.instruction_count;
    if (n == 0) return;
    bool indirect = false;
    switch (block.instructions[n - 1].opcode) {
    case Opcode::RET_NEAR:   case Opcode::RET_NEAR_IMM16:
    case Opcode::RET_FAR:    case Opcode::RET_FAR_IMM16:
    case Opcode::JMP_RM32:   case Opcode::JMP_RM64:
    case Opcode::CALL_RM32:  case Opcode::CALL_RM64:
    case Opcode::JMP_M16_16: case Opcode::JMP_M16_32:
    case Opcode::JMP_M16_64:
    case Opcode::CALL_M16_16: case Opcode::CALL_M16_32:
    case Opcode::CALL_M16_64:
        indirect = true;
        break;
    default:
        break;
    }
    if (bucket == Bucket::Indirect && !indirect) return;
    if (bucket == Bucket::Direct && indirect) return;

    // The live-out record is the last definition the analysis above kept.
    for (u32 i = n; i-- > 0;) {
        if (fx[i] == Effect::FullWriter) {
            if ((omit[i] & 3u) == 0) omit[i] |= 1u;
            return;
        }
    }
}

/// Map a register-only 32- or 64-bit full-flag publisher to a recoverable
/// FlagOp. Kept in lockstep with `ir_passes.cpp::recoverable_record` and the
/// rebuild in `jit_rebuild_flag_record` (linear_fault.cpp).
bool try_decode_recoverable_publisher(const DecodedInstruction& def,
                                      bool mode64,
                                      StencilFlagRecovery& out) noexcept {
    using FO = FlagState::FlagOp;
    bool recompute = false;
    bool wide = false;
    FO op = FO::None;
    const Opcode oc = def.opcode;
    switch (oc) {
    case Opcode::ADD_RM32_R32: case Opcode::ADD_R32_RM32:
    case Opcode::ADD_RM32_IMM8: case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_EAX_IMM32:
        op = FO::Add32; break;
    case Opcode::SUB_RM32_R32: case Opcode::SUB_R32_RM32:
    case Opcode::SUB_RM32_IMM8: case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_EAX_IMM32:
        op = FO::Sub32; break;
    case Opcode::CMP_RM32_R32: case Opcode::CMP_R32_RM32:
    case Opcode::CMP_RM32_IMM8: case Opcode::CMP_RM32_IMM32:
    case Opcode::CMP_EAX_IMM32:
        op = FO::Sub32; recompute = true; break;
    case Opcode::AND_RM32_R32: case Opcode::AND_R32_RM32:
    case Opcode::AND_RM32_IMM8: case Opcode::AND_RM32_IMM32:
    case Opcode::AND_EAX_IMM32:
        op = FO::And32; break;
    case Opcode::TEST_RM32_R32: case Opcode::TEST_RM32_IMM32:
    case Opcode::TEST_EAX_IMM32:
        op = FO::And32; recompute = true; break;
    case Opcode::OR_RM32_R32: case Opcode::OR_R32_RM32:
    case Opcode::OR_RM32_IMM8: case Opcode::OR_RM32_IMM32:
    case Opcode::OR_EAX_IMM32:
        op = FO::Or32; break;
    case Opcode::XOR_RM32_R32: case Opcode::XOR_R32_RM32:
    case Opcode::XOR_RM32_IMM8: case Opcode::XOR_RM32_IMM32:
    case Opcode::XOR_EAX_IMM32:
        op = FO::Xor32; break;
    // REX.W forms (64-bit mode only). Their immediates are imm8/imm32
    // sign-extended to 64 bits (Intel SDM Vol. 2 ADD/SUB/CMP/AND/OR/XOR/TEST).
    case Opcode::ADD_RM64_R64: case Opcode::ADD_R64_RM64:
    case Opcode::ADD_RM64_IMM8: case Opcode::ADD_RM64_IMM32:
    case Opcode::ADD_RAX_IMM32:
        op = FO::Add64; wide = true; break;
    case Opcode::SUB_RM64_R64: case Opcode::SUB_R64_RM64:
    case Opcode::SUB_RM64_IMM8: case Opcode::SUB_RM64_IMM32:
    case Opcode::SUB_RAX_IMM32:
        op = FO::Sub64; wide = true; break;
    case Opcode::CMP_RM64_R64: case Opcode::CMP_R64_RM64:
    case Opcode::CMP_RM64_IMM8: case Opcode::CMP_RM64_IMM32:
    case Opcode::CMP_RAX_IMM32:
        op = FO::Sub64; wide = true; recompute = true; break;
    case Opcode::AND_RM64_R64: case Opcode::AND_R64_RM64:
    case Opcode::AND_RM64_IMM8: case Opcode::AND_RM64_IMM32:
    case Opcode::AND_RAX_IMM32:
        op = FO::And64; wide = true; break;
    case Opcode::TEST_RM64_R64: case Opcode::TEST_RM64_IMM32:
    case Opcode::TEST_RAX_IMM32:
        op = FO::And64; wide = true; recompute = true; break;
    case Opcode::OR_RM64_R64: case Opcode::OR_R64_RM64:
    case Opcode::OR_RM64_IMM8: case Opcode::OR_RM64_IMM32:
    case Opcode::OR_RAX_IMM32:
        op = FO::Or64; wide = true; break;
    case Opcode::XOR_RM64_R64: case Opcode::XOR_R64_RM64:
    case Opcode::XOR_RM64_IMM8: case Opcode::XOR_RM64_IMM32:
    case Opcode::XOR_RAX_IMM32:
        op = FO::Xor64; wide = true; break;
    default:
        return false;
    }
    if (wide && !mode64) return false;
    if (has_memory_operand(def) || def.operand_count < 1) return false;

    const OperandType gpr_type = wide ? OperandType::Reg64 : OperandType::Reg32;
    const u8 reg_limit = mode64 ? 16 : 8;
    auto take = [&](const Operand& o, bool& is_imm, u8& reg, u64& imm) {
        if (o.type == gpr_type) {
            if (o.reg >= reg_limit) return false;
            is_imm = false; reg = o.reg; imm = 0; return true;
        }
        if (is_immediate_operand(o.type)) {
            is_imm = true; reg = 0xFF;
            imm = wide ? static_cast<u64>(o.imm)
                       : static_cast<u64>(static_cast<u32>(o.imm));
            return true;
        }
        return false;
    };

    Operand dst_op = def.operands[0];
    Operand src_op{};
    if (def.operand_count >= 2) {
        src_op = def.operands[1];
    } else if (oc == Opcode::ADD_EAX_IMM32 || oc == Opcode::SUB_EAX_IMM32 ||
               oc == Opcode::CMP_EAX_IMM32 || oc == Opcode::AND_EAX_IMM32 ||
               oc == Opcode::OR_EAX_IMM32 || oc == Opcode::XOR_EAX_IMM32 ||
               oc == Opcode::TEST_EAX_IMM32 || oc == Opcode::ADD_RAX_IMM32 ||
               oc == Opcode::SUB_RAX_IMM32 || oc == Opcode::CMP_RAX_IMM32 ||
               oc == Opcode::AND_RAX_IMM32 || oc == Opcode::OR_RAX_IMM32 ||
               oc == Opcode::XOR_RAX_IMM32 || oc == Opcode::TEST_RAX_IMM32) {
        if (!is_immediate_operand(def.operands[0].type)) return false;
        dst_op = Operand::make_reg(gpr_type, 0);
        src_op = def.operands[0];
    } else {
        return false;
    }

    out = StencilFlagRecovery{};
    out.flag_op = static_cast<u8>(op);
    out.recompute_dst = recompute;
    out.width = wide ? 64 : 32;
    if (!take(src_op, out.src_is_imm, out.src_reg, out.src_imm)) return false;
    if (!take(dst_op, out.dst_is_imm, out.dst_reg, out.dst_imm)) return false;
    if (!recompute && out.dst_is_imm) return false;
    // `add eax, eax`: after the publisher the one register holds the RESULT,
    // so a record rebuilt from it would name the result as its own source and
    // lose CF/OF/AF (Add derives the first addend as dst - src). SUB r,r is
    // exact either way (its flags are those of 0 - 0) and the logic ops read
    // only the result, so only ADD is refused.
    if (!recompute && !out.src_is_imm && out.src_reg == out.dst_reg &&
        (op == FO::Add32 || op == FO::Add64)) {
        return false;
    }
    return true;
}

namespace {

/// True when operand `o` is a general register that aliases guest GPR `reg`.
/// A byte register numbered 4-7 is AH/CH/DH/BH without REX and SPL..DIL with
/// it (Intel SDM Vol. 1 3.4.1.1); both readings are reported.
bool operand_names_gpr(const Operand& o, u8 reg) noexcept {
    switch (o.type) {
    case OperandType::Reg8:
        return o.reg == reg || (o.reg >= 4 && o.reg < 8 && o.reg - 4 == reg);
    case OperandType::Reg16:
    case OperandType::Reg32:
    case OperandType::Reg64:
        return o.reg == reg;
    default:
        return false;
    }
}

}  // namespace

/// Whether `instr` may write guest GPR `reg` (0-15). A sound
/// over-approximation: false only when every GPR the instruction writes is
/// known and none is `reg`.
///
/// The omission scan crosses Jcc, the flag-neutral class, full-flag writers
/// and crossable partial writers (classify / crossable_partial_writer). Among
/// those an explicit GPR destination is always operand 0 (Intel SDM Vol. 2);
/// the implicit destinations are listed. String instructions advance
/// RSI/RDI (and RCX when repeated), and a REP form can fault after some
/// iterations with that progress architectural, so they report every
/// register. Every other Barrier reports true.
bool may_write_gpr(const DecodedInstruction& instr, u8 reg) noexcept {
    if (reg == 0xFF) return false;
    const Opcode op = instr.opcode;
    if (is_jcc_opcode(op)) return false;
    if (op >= Opcode::MOVSB && op <= Opcode::SCASQ) return true;
    constexpr u8 kRax = 0, kRdx = 2, kRsp = 4, kRbp = 5;
    const bool op0 = instr.operand_count >= 1 &&
                     operand_names_gpr(instr.operands[0], reg);
    const bool op1 = instr.operand_count >= 2 &&
                     operand_names_gpr(instr.operands[1], reg);
    switch (op) {
    case Opcode::CBW: case Opcode::CWDE: case Opcode::CDQE:
    case Opcode::FNSTSW_AX: case Opcode::XLAT:
        return reg == kRax;
    case Opcode::CWD: case Opcode::CDQ: case Opcode::CQO:
        return reg == kRdx;
    case Opcode::MUL_RM8: case Opcode::MUL_RM16:
    case Opcode::MUL_RM32: case Opcode::MUL_RM64:
    case Opcode::IMUL_RM8: case Opcode::IMUL_RM16:
    case Opcode::IMUL_RM32: case Opcode::IMUL_RM64:
        return reg == kRax || reg == kRdx;
    case Opcode::PUSH_R16: case Opcode::PUSH_R32: case Opcode::PUSH_R64:
    case Opcode::PUSH_RM16: case Opcode::PUSH_RM32: case Opcode::PUSH_RM64:
    case Opcode::PUSH_IMM8: case Opcode::PUSH_IMM16: case Opcode::PUSH_IMM32:
    case Opcode::PUSH_CS: case Opcode::PUSH_SS: case Opcode::PUSH_DS:
    case Opcode::PUSH_ES: case Opcode::PUSH_FS: case Opcode::PUSH_GS:
    case Opcode::PUSHF: case Opcode::PUSHFQ: case Opcode::PUSHA:
    case Opcode::POPF: case Opcode::POPFQ:
        return reg == kRsp;
    case Opcode::POP_R16: case Opcode::POP_R32: case Opcode::POP_R64:
    case Opcode::POP_RM16: case Opcode::POP_RM32: case Opcode::POP_RM64:
    case Opcode::POP_DS: case Opcode::POP_ES: case Opcode::POP_SS:
    case Opcode::POP_FS: case Opcode::POP_GS:
        return reg == kRsp || op0;
    case Opcode::POPA:
        return true;
    case Opcode::LEAVE: case Opcode::ENTER:
        return reg == kRsp || reg == kRbp;
    case Opcode::XCHG_AX_R16: case Opcode::XCHG_EAX_R32:
    case Opcode::XCHG_RAX_R64:
        return reg == kRax || op0 || op1;
    case Opcode::XCHG_RM8_R8: case Opcode::XCHG_RM16_R16:
    case Opcode::XCHG_RM32_R32: case Opcode::XCHG_RM64_R64:
        return op0 || op1;
    default:
        break;
    }
    // CMP/TEST/COMISS-class writers and BT write no GPR; reporting operand 0
    // for BT is merely conservative.
    if ((op >= Opcode::CMP_AL_IMM8 && op <= Opcode::CMP_R64_RM64) ||
        (op >= Opcode::TEST_AL_IMM8 && op <= Opcode::TEST_RM64_R64)) {
        return false;
    }
    if (classify(instr) == Effect::Barrier && !crossable_partial_writer(instr)) {
        return true;
    }
    return op0;
}

bool try_plan_stencil_flag_recovery(const BasicBlock& block, u32 def_i,
                                    u32 killer_i, const std::vector<u64>& addr,
                                    StencilFlagRecovery& plan) noexcept {
    if (def_i >= block.instruction_count || killer_i > block.instruction_count) {
        return false;
    }
    if (killer_i <= def_i) return false;

    // VITREO_JIT_NO_X64_RECOVERY=1: no x64 plans (the rule before
    // 2026-10-09), for bisecting.
    static const bool no_x64_recovery =
        std::getenv("VITREO_JIT_NO_X64_RECOVERY") != nullptr;
    if (block.is_64bit && no_x64_recovery) return false;
    StencilFlagRecovery built{};
    if (!try_decode_recoverable_publisher(block.instructions[def_i],
                                          block.is_64bit, built)) {
        return false;
    }

    u64 first_rip = 0;
    u64 last_rip = 0;
    bool any_fault = false;
    u32 last_fault_i = def_i;
    for (u32 j = def_i + 1; j < killer_i; ++j) {
        if (!instruction_touches_memory(block.instructions[j])) continue;
        any_fault = true;
        if (first_rip == 0) first_rip = addr[j];
        last_rip = addr[j];
        last_fault_i = j;
    }
    if (killer_i < block.instruction_count &&
        instruction_touches_memory(block.instructions[killer_i])) {
        any_fault = true;
        if (first_rip == 0) first_rip = addr[killer_i];
        last_rip = addr[killer_i];
        last_fault_i = killer_i;
    }
    if (!any_fault) return false;

    // The record is rebuilt from the operand registers at the fault, so none
    // may be written before the last instruction that can fault. That
    // instruction itself writes nothing before it faults (a fault is precise,
    // Intel SDM Vol. 3A 6.6) -- except a REP string instruction, which
    // may_write_gpr reports for every register.
    for (u32 j = def_i + 1; j <= last_fault_i; ++j) {
        const auto& mid = block.instructions[j];
        if (j == last_fault_i &&
            !(mid.opcode >= Opcode::MOVSB && mid.opcode <= Opcode::SCASQ)) {
            break;
        }
        if (!built.src_is_imm && may_write_gpr(mid, built.src_reg)) {
            return false;
        }
        if (!built.dst_is_imm && may_write_gpr(mid, built.dst_reg)) {
            return false;
        }
    }

    built.def_rip = addr[def_i];
    built.first_rip = first_rip;
    built.last_rip = last_rip;
    plan = built;
    return true;
}

void compute_omit_lazy_flags(
    const BasicBlock& block, std::vector<u8>& omit,
    std::vector<StencilFlagRecovery>* recovery,
    std::vector<u32>* elided_span,
    bool cross_nzcv_readers) noexcept {
    const u32 n = block.instruction_count;
    omit.assign(n, 0);
    if (recovery != nullptr) recovery->clear();
    if (elided_span != nullptr) elided_span->assign(n, 0);
    if (n == 0) return;
    // `instruction_count` is the loop bound everywhere in the JIT, but the
    // decoded array behind it is separately owned. If they ever disagree,
    // indexing past the end is a host segfault; leaving `omit` all-zero simply
    // omits no lazy-flag stores, which is always semantically safe — the
    // optimisation is dropped, the block still compiles correctly.
    if (block.instructions.size() < static_cast<usize>(n)) return;
    // VITREO_JIT_NO_OMIT=1: never omit a lazy-flag store. Always semantically
    // safe (the optimisation is simply dropped), so it isolates every
    // liveness-driven omission in one switch.
    static const bool no_omit = std::getenv("VITREO_JIT_NO_OMIT") != nullptr;
    if (no_omit) return;

    // VITREO_JIT_OMIT_ALL=1: **DELIBERATELY UNSOUND MEASUREMENT PROBE.** Omit
    // every lazy-flag store, as though a perfect cross-block oracle had proved
    // all of them dead. Guest flags are then wrong wherever a later block
    // actually reads them, so this is only ever a ceiling: it answers "what is
    // the absolute most that any cross-block flag-liveness analysis could be
    // worth" before any of that analysis is designed.
    //
    // Never on by default and never a correctness path. Read the corpus's
    // checksum column: a MISMATCH row means flags DID matter there, and its
    // time is void as a target -- but its existence is itself the finding.
    static const bool omit_all =
        std::getenv("VITREO_JIT_OMIT_ALL") != nullptr;
    if (omit_all) {
        omit.assign(n, 1);
        return;
    }

    // In x64 traces, retain records across every control-flow edge and
    // intervening instruction. An adjacent non-faulting full writer is the
    // narrow case with no edge or fault on which the old flags can be read.
    //
    // Full liveness (default; VITREO_JIT_FLAG_LIVENESS64=0 restores the
    // narrow peephole below): run the SAME full dataflow analysis 32-bit
    // blocks get. The narrow bail-out was added in
    // fe8d8901 when x64 trace formation was turned on, and it is expensive:
    // a `CMP r/m64,r64` whose only consumer is the next `Jcc` still publishes
    // a seven-instruction lazy-flag record nobody reads, because the narrow
    // peephole below only fires when the NEXT instruction kills the flags,
    // and a Jcc reads them. Measured on sc_blocksize, an x64 block transition
    // costs ~49 cycles against x86's ~12 on the same source.
    //
    // The analysis below already carries x64-specific handling (the
    // `block.is_64bit` arms in the escaping-Jcc walk), which this early
    // return orphaned -- so this is a gate over existing code, not new
    // analysis.
    //
    // History: turned on 2026-09-08 and reverted the same day, because the
    // x64 flag-block fuzzer diverged from Rosetta on ~0.5% of blocks (seeds
    // 0xB2000, 0xC3000, 0xD4000). Both recorded repros end in SBB r64
    // (tools/fuzz/repros/*seed_729195.json, *seed_729288.json): the SBB64
    // helper recomputed CF from the lazy record, which the analysis had
    // correctly proved dead because ADC/SBB take a chained carry from live
    // NZCV. SBB r64 is now lowered natively from NZCV like ADC r64, and both
    // repros are pinned as FlagLivenessTest.X64SbbAfter*MatchesOracle. The
    // same seeds plus nine fresh ones (3000 blocks) are clean with this on.
    //
    // The smoke gates passing is NOT sufficient evidence for changes here:
    // run fresh fuzzer seeds.
    static const bool full_liveness64 =
        environment_flag_enabled("VITREO_JIT_FLAG_LIVENESS64", true);

    // INC/DEC preserve CF, so they remain barriers to every other analysis.
    // Their own publication is unnecessary only when the immediately following
    // register-only full writer replaces all flags before any observer/fault.
    // Intel SDM Vol. 2A INC/DEC. Do not cross memory, LOCK (#UD on registers),
    // a flag consumer, or an edge; there is no recovery/deferred-store contract
    // for these partial writers.
    //
    // In a long-mode block the killer may also sit further on, past register-
    // only moves and the crossable partial writers (crossable_partial_writer):
    // none of them reads a flag, faults, or leaves the block, so the INC/DEC
    // record is just as unobservable. `shl r9d,8; inc rbp; mov ecx,r9d;
    // add r8d,r8d` (a range decoder's normalization) is the shape. Only for
    // the r/m32 and r/m64 forms, whose 64-bit lowering (unary64.cpp) then
    // skips the carry capture as well as the record -- which is what lets the
    // main scan below cross such an INC/DEC (incdec_record_dead).
    for (u32 i = 0; i + 1 < n; ++i) {
        const auto& cur = block.instructions[i];
        const bool incdec =
            (cur.opcode >= Opcode::INC_R16 && cur.opcode <= Opcode::DEC_RM64);
        if (!incdec || cur.operand_count != 1 ||
            !is_register_operand(cur.operands[0].type) || cur.prefixes.lock) {
            continue;
        }
        const bool scan_far = block.is_64bit && long_mode_incdec(cur.opcode);
        for (u32 j = i + 1; j < n; ++j) {
            const auto& next = block.instructions[j];
            if (next.prefixes.lock) break;
            if (pending_flags_dead_across(next)) {
                omit[i] = 1;
                if (elided_span != nullptr && j > i + 1) {
                    for (u32 k = i + 1; k <= j; ++k) (*elided_span)[k] = i + 1;
                }
                break;
            }
            if (!scan_far || instruction_touches_memory(next)) break;
            if (plain_register_move(next) || crossable_partial_writer(next)) {
                continue;
            }
            break;
        }
    }

    // A long-mode two- and three-operand IMUL publishes a record of its own
    // (CF/OF from the product; SF/ZF/AF/PF undefined, Intel SDM Vol. 2A IMUL)
    // and reads no flag. That record is dead under the same rule as INC/DEC's
    // far case: a register-only full writer follows across nothing but
    // register moves and crossable partial writers. A memory SOURCE does not
    // matter here: its fault precedes the IMUL's own flag write, so it can
    // only expose the flags before the IMUL, which this rule does not touch.
    // `imul ecx,edx; cmp eax,ecx` is the range decoder's bit step.
    // mul_div64.cpp skips the publication for these forms when omitted.
    if (block.is_64bit) {
        for (u32 i = 0; i + 1 < n; ++i) {
            const auto& cur = block.instructions[i];
            if (!long_mode_imul_publisher(cur.opcode) || cur.prefixes.lock) continue;
            for (u32 j = i + 1; j < n; ++j) {
                const auto& next = block.instructions[j];
                if (next.prefixes.lock) break;
                if (pending_flags_dead_across(next)) {
                    omit[i] = 1;
                    if (elided_span != nullptr) {
                        for (u32 k = i + 1; k <= j; ++k) (*elided_span)[k] = i + 1;
                    }
                    break;
                }
                if (instruction_touches_memory(next)) break;
                if (plain_register_move(next) ||
                    crossable_partial_writer(next)) {
                    continue;
                }
                break;
            }
        }
    }


    std::vector<Effect> fx(n, Effect::Barrier);
    for (u32 i = 0; i < n; ++i) {
        fx[i] = classify(block.instructions[i]);
    }

    // Address of each instruction, and a map back from address to index, so a
    // branch target can be resolved to a position in this block.
    std::vector<u64> addr(n, 0);
    {
        u64 a = block.start_addr;
        for (u32 i = 0; i < n; ++i) {
            addr[i] = a;
            a += block.instructions[i].length;
        }
    }
    auto index_of_addr = [&](u64 target) -> int {
        for (u32 i = 0; i < n; ++i) {
            if (addr[i] == target) return static_cast<int>(i);
        }
        return -1;  // outside this block (or mid-instruction)
    };

    // Instructions some direct in-block branch lands on. Such an instruction
    // has a second predecessor, so the NZCV live on entry is not necessarily
    // the one its textual predecessor left.
    std::vector<bool> join(n, false);
    for (u32 j = 0; j < n; ++j) {
        const DecodedInstruction& br = block.instructions[j];
        if (br.operand_count < 1) continue;
        const OperandType t = br.operands[0].type;
        if (t != OperandType::Rel8 && t != OperandType::Rel16 &&
            t != OperandType::Rel32) {
            continue;
        }
        const int ti = index_of_addr(addr[j] + br.length +
                                     static_cast<u64>(br.operands[0].imm));
        if (ti >= 0) join[static_cast<u32>(ti)] = true;
    }

    // Kind of NZCV each instruction leaves live, extended along an ADC/SBB
    // carry chain: `add; adc; adc; sbb` keeps the carry in PSTATE the whole
    // way, so every link is a publisher for the link after it. Computed as a
    // forward pass because link k's status depends on link k-1's.
    std::vector<PubKind> chain(n, PubKind::None);
    for (u32 i = 0; i < n; ++i) {
        const DecodedInstruction& in = block.instructions[i];
        const PubKind own = (fx[i] == Effect::FullWriter)
                                ? publisher_kind(in.opcode)
                                : PubKind::None;
        if (own != PubKind::None && !has_memory_operand(in)) {
            chain[i] = own;
            continue;
        }
        if (i > 0 && !join[i] && is_adc_sbb(in.opcode) &&
            !has_memory_operand(in) &&
            (chain[i - 1] == PubKind::Add || chain[i - 1] == PubKind::Sub)) {
            // Mirrors what the emitter publishes on the chain fast path:
            // ADC leaves Add-kind NZCV, SBB leaves Sub-kind.
            chain[i] = (in.opcode >= Opcode::SBB_AL_IMM8 &&
                        in.opcode <= Opcode::SBB_R64_RM64)
                           ? PubKind::Sub
                           : PubKind::Add;
        }
    }

    // The adjacent-pair rules: the record's only possible reader is the very
    // next instruction, which is register-only and redefines all six flags.
    // No faulting access lies between. The next instruction must not be a
    // branch target: reached from elsewhere, an ADC/SBB there reads CF from
    // the lazy record rather than from this publisher's NZCV.
    auto omit_adjacent_carry_link = [&](u32 i) -> bool {
        if (i + 1 >= n || join[i + 1]) return false;
        const DecodedInstruction& cur = block.instructions[i];
        const DecodedInstruction& next = block.instructions[i + 1];
        if (fx[i] != Effect::FullWriter) {
            // An ADC/SBB link retired by the next link or by an adjacent
            // register-only full writer.
            if (chain[i] == PubKind::None || !is_adc_sbb(cur.opcode) ||
                has_memory_operand(cur)) {
                return false;
            }
            const bool killer =
                (is_adc_sbb(next.opcode) || fx[i + 1] == Effect::FullWriter) &&
                !has_memory_operand(next);
            if (killer) omit[i] = 2;
            return killer;
        }
        // A plain Add/Sub publisher retired by an adjacent ADC/SBB, checked
        // before the general scan because that scan breaks on an ADC/SBB
        // Barrier immediately.
        if (adc_sbb_retires_publisher(cur, chain[i], next)) {
            omit[i] = 2;
            return true;
        }
        return false;
    };

    if (block.is_64bit && !full_liveness64) {
        for (u32 i = 0; i + 1 < n; ++i) {
            if (omit_adjacent_carry_link(i)) continue;
            if (fx[i] == Effect::FullWriter &&
                !instruction_touches_memory(block.instructions[i]) &&
                pending_flags_dead_across(block.instructions[i + 1])) {
                omit[i] = 1;
            }
        }
        return;
    }


    for (u32 i = 0; i < n; ++i) {
        // An ADC/SBB link may have its own record retired by the NEXT link,
        // and nothing else: it is a Barrier for every other rule in this file,
        // so it never enters the general killer scan below.
        if (omit_adjacent_carry_link(i)) continue;
        if (fx[i] != Effect::FullWriter) continue;
        const PubKind kind = publisher_kind(block.instructions[i].opcode);

        // Highest in-block index reached by a taken branch we scanned past.
        // The x64 JIT can keep an adjacent, register-only publisher + fused
        // forward Jcc inside one trace. A later writer retires this record only
        // when it lies at or beyond every such target, so both predecessors
        // reach it. Other branch shapes still leave through a cold exit and
        // retain the existing one-edge deferred-store rule below.
        u32 max_taken_target = 0;
        // Index of the adjacent Jcc that established max_taken_target, i.e.
        // the one edge the internal-join proof below is allowed to discharge.
        // Only j == i + 1 can set it, so there is at most one.
        u32 internal_jcc_idx = 0;
        // True once an escaping Jcc has been crossed on the strength of the
        // cold-stub re-emit. At most one may be crossed.
        bool crossed_escaping = false;
        // True when a Neutral may-fault op was crossed; omit then requires a
        // JitFlagRecovery plan before it is sound.
        bool needs_recovery = false;

        for (u32 j = i + 1; j < n; ++j) {
            // The signal fault path reconstructs guest RIP but cannot recover
            // live host NZCV. Even a flag-neutral load/store may fault (page
            // protection, lazy mapping, or a genuine guest exception), so a
            // publisher's architectural lazy record must exist before any
            // intervening memory access — unless a recovery plan can rebuild
            // it (ADR-0004 / stencil M5). Full writers with memory operands
            // are already rejected by pending_flags_dead_across(); this closes
            // the corresponding hole for Neutral-classified instructions.
            if (instruction_touches_memory(block.instructions[j])) {
                // Keep the internal-join proof narrow (no mem). Barriers that
                // touch memory still stop the scan.
                if (max_taken_target != 0) break;
                if (fx[j] == Effect::Neutral) {
                    needs_recovery = true;
                    continue;
                }
                if (fx[j] != Effect::FullWriter) break;
                // FullWriter with memory: a potential killer, but its access
                // can fault before it writes a flag (Intel SDM Vol. 3A 6.6),
                // and the exception context must then hold this publisher's
                // flags. Retiring the record needs a plan that covers it.
                needs_recovery = true;
            }
            // VITREO_JIT_NO_CROSS_PARTIAL=1 bisects the partial-writer
            // crossing onto one switch, the way NO_SCAN_PAST does for the
            // forward-Jcc rescan.
            static const bool no_cross_partial =
                std::getenv("VITREO_JIT_NO_CROSS_PARTIAL") != nullptr;
            if (!no_cross_partial && fx[j] == Effect::Barrier &&
                crossable_partial_writer(block.instructions[j])) {
                // Writes some flags, reads none, cannot fault: it can neither
                // observe the omitted record nor let anything else observe it.
                // See crossable_partial_writer() for why the flags it
                // PRESERVES going stale is not observable either.
                continue;
            }
            // A long-mode register INC/DEC whose own record the rule above
            // proved dead captures no carry and publishes nothing (unary64.cpp),
            // so it reads nothing of this record either. Not while proving an
            // internal join, which crosses only ALU writers.
            if (fx[j] == Effect::Barrier && max_taken_target == 0 &&
                incdec_record_dead(block, omit, j)) {
                continue;
            }
            // A register SETcc / CMOVcc whose condition the publisher's NZCV
            // answers reads NZCV, not the record (nzcv_reader_cc). Crossing it
            // needs no edge reasoning: it writes no flag, cannot fault and
            // falls through. Not while proving an internal join, which crosses
            // only ALU writers (see below). VITREO_JIT_NO_CROSS_READERS=1
            // bisects this rule onto one switch.
            static const bool no_cross_readers =
                std::getenv("VITREO_JIT_NO_CROSS_READERS") != nullptr;
            // A flag reader some in-block branch lands on has a second
            // predecessor, so its flags are not this publisher's NZCV on every
            // path, and the emitter evaluates it from the lazy record (Intel
            // SDM Vol. 2 Jcc/SETcc/CMOVcc read the EFLAGS of whichever path
            // ran). `cmp [r9],eax; jne L; mov; cmp eax,[rsi+4]; L: jb` is the
            // shape: crossing the JB elided the second CMP's record it reads.
            if (fx[j] == Effect::Barrier && join[j] &&
                (nzcv_reader_cc(block.instructions[j]) >= 0 ||
                 jcc_cc(block.instructions[j].opcode) >= 0)) {
                break;
            }
            if (fx[j] == Effect::Barrier) {
                const int rcc = nzcv_reader_cc(block.instructions[j]);
                if (rcc >= 0) {
                    if (no_cross_readers || !cross_nzcv_readers ||
                        max_taken_target != 0 ||
                        narrow_test(block.instructions[i].opcode) ||
                        !jcc_fuses_from_publisher(kind, rcc)) {
                        break;
                    }
                    continue;
                }
            }
            if (fx[j] == Effect::Barrier) {
                // A Jcc that fuses from this writer's NZCV reads NZCV, not
                // FlagOp, so it does not by itself keep the store alive — and
                // we may keep scanning for a killer.
                //
                // But a Jcc has TWO edges and this scan only walks
                // fall-through. Crossing one is sound only when the taken edge
                // rejoins *inside* this block, so both paths still reach the
                // killer. If the target leaves the block, control escapes with
                // this publisher's flags architecturally live and nothing here
                // can kill them.
                //
                // Getting this wrong is invisible in the common case: with an
                // unconditional `continue` here, `TEST edi,edi; JE end;
                // ADD [esi],1` let the ADD kill the TEST. When the branch was
                // taken the ADD never ran and the block exited with no lazy
                // FlagOp and no NZCV kind — EFLAGS silently lost to any
                // observer that read them before the next writer (an exception
                // record, GetThreadContext, PUSHF). Caught by
                // OracleInteger32.TestEdiEdiJeSkipAdd against real x86.
                const int cc = jcc_cc(block.instructions[j].opcode);
                if (!jcc_fuses_from_publisher(kind, cc)) break;

                // A Jcc's displacement is Rel8/Rel16/Rel32, which
                // is_immediate_operand() does NOT cover — it spans Imm8..Imm64
                // only. Testing for "immediate" here silently rejected every
                // branch and disabled the optimization wholesale.
                const DecodedInstruction& br = block.instructions[j];
                const bool rel = br.operand_count >= 1 &&
                    (br.operands[0].type == OperandType::Rel8 ||
                     br.operands[0].type == OperandType::Rel16 ||
                     br.operands[0].type == OperandType::Rel32);
                if (!rel) {
                    break;  // indirect / unknown target: assume it escapes
                }
                const u64 target = addr[j] + br.length +
                    static_cast<u64>(br.operands[0].imm);
                const int ti = index_of_addr(target);
                if (ti <= static_cast<int>(j)) {
                    // Leaves the block, or branches backwards (a loop edge
                    // re-executes this publisher, so a forward killer does not
                    // dominate the back edge either).
                    //
                    // A Jcc immediately after the publisher is still
                    // crossable: nothing has run in between, so the publisher's
                    // operand registers are intact at the branch and
                    // queue_mid_block_jcc_exit re-emits the store on the taken
                    // edge -- into the cold stub when the branch fuses, inline
                    // otherwise. The one thing that does run in between is a
                    // memory-destination publisher's own store, whose slow
                    // path calls out; that store publishes the skipped record
                    // first (publish_pending_stub_store), so the Jcc finds
                    // nothing left to replay. That keeps the store out of the hot
                    // fall-through body, which is where the ~2.5x on
                    // WalkDumpLooped256WithJcc came from. Only one such Jcc may
                    // be crossed; past it the registers are no longer known.
                    //
                    // Both bitnesses now honour the re-emit contract:
                    // emit_set_lazy_flags (32-bit) and emit_set_lazy_flags64
                    // (64-bit, wide=true) both record the skipped store in
                    // m_pending_stub_store, and queue_mid_block_jcc_exit
                    // replays it on the taken edge at the recorded width.
                    // Without that recording a 64-bit taken edge leaves AF/PF
                    // stale, since NZCV kind bits carry only N/Z/C/V — see
                    // JitParity64Test.MidBlockTakenJccKeepsAfPfOfOmittedLazyStore,
                    // which is the guard on this whole contract.
                    static const bool no_defer64 =
                        std::getenv("VITREO_JIT_NO_DEFER64") != nullptr;
                    if (j != i + 1 || crossed_escaping ||
                        (no_defer64 && block.is_64bit)) {
                        break;
                    }
                    crossed_escaping = true;
                    continue;
                }
                if (block.is_64bit && j == i + 1 &&
                    !has_memory_operand(block.instructions[i])) {
                    max_taken_target = std::max(
                        max_taken_target, static_cast<u32>(ti));
                    internal_jcc_idx = j;
                    continue;
                }
                // A forward branch whose target is INSIDE this block does
                // not necessarily rejoin the translation. Non-adjacent,
                // memory-publisher, and non-fused shapes remain cold exits, so
                // their taken edge leaves with this publisher's flags live.
                //
                // Treating it as a rejoin is what broke trace formation:
                // before traces a Jcc's target was never in-block, so this
                // path never ran; with traces it runs constantly and omitted
                // stores whose AF/PF the taken edge still needed. Measured as
                // a 64-bit launcher dying 16,368 instructions in at RIP=0x14009ea3b, an
                // SSO string test reading a corrupted pointer. The same
                // mismatch is the recorded 32-bit trace hazard.
                //
                // Fall through to the escaping rule: crossable only
                // immediately after the publisher, where the operands are
                // still intact and queue_mid_block_jcc_exit re-emits the
                // store on the taken edge.
                if (j != i + 1 || crossed_escaping) break;
                crossed_escaping = true;
                continue;
            }
            if (fx[j] == Effect::FullWriter) {
                if (max_taken_target != 0 &&
                    !pending_flags_dead_across(block.instructions[j])) {
                    // A fault before this writer publishes its replacement
                    // flags exposes the predecessor's architectural record.
                    // The internal-join proof therefore accepts only
                    // register/immediate writers on every path to the killer.
                    break;
                }
                // Only a killer that every path reaches may omit the store.
                if (j >= max_taken_target) {
                    if (needs_recovery) {
                        StencilFlagRecovery plan{};
                        if (!try_plan_stencil_flag_recovery(block, i, j, addr,
                                                            plan)) {
                            break;
                        }
                        if (recovery != nullptr) recovery->push_back(plan);
                    }
                    omit[i] = 1;
                    // Between the publisher and its killer the record is not
                    // in CpuState; the killer itself is included because a
                    // block truncated there hands the killer to the
                    // interpreter with the stale record.
                    if (elided_span != nullptr) {
                        for (u32 k = i + 1; k <= j && k < n; ++k) {
                            (*elided_span)[k] = i + 1;
                        }
                    }
                    if (max_taken_target != 0 && internal_jcc_idx != 0) {
                        // This record was discharged by the internal-join
                        // proof, not by a cold-stub re-emit. Publish that on
                        // the Jcc itself: only an edge carrying kInternalJoin
                        // may be compiled as a real intra-block branch, which
                        // has no cold stub to replay the store on.
                        omit[internal_jcc_idx] |= kInternalJoinJcc;
                    }
                    break;
                }
                static const bool no_scan_past =
                    std::getenv("VITREO_JIT_NO_SCAN_PAST") != nullptr;
                if (no_scan_past) break;
                // This killer retires the record on the FALL-THROUGH path
                // only: a crossed forward Jcc jumps over it, so the taken path
                // still carries the publisher's flags. Keep scanning for a
                // killer at or past that branch target, which retires it on
                // both. Stopping here instead was why `cmp; jcc; alu` — the
                // single most common shape in x86 — never omitted, measured at
                // 11 emitted ARM per `CMP EAX, imm32`.
                //
                // Everything that could make continuing unsafe still breaks
                // below on its own terms: a Reader breaks (it may observe the
                // record), and a second escaping Jcc breaks on the
                // crossed_escaping guard, because past one branch the
                // publisher's operand registers are no longer known to be
                // intact for the re-emit.
                continue;
            }
            if (max_taken_target != 0) {
                // Keep this first rollout deliberately narrow: no neutral
                // instruction (including implicit-stack and memory forms) is
                // crossed while proving an internal join. Dense CMP/TEST-Jcc
                // traces consist only of non-faulting ALU writers here.
                break;
            }
        }
    }

    probe_omit_liveout(block, fx, omit);
}

}  // namespace vitreo::cpu::jit
