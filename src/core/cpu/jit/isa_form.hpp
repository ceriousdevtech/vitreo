// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file isa_form.hpp
/// @brief The FORM of a decoded instruction — the unit the ISA matrix scores.
///
/// An opcode is not the unit of JIT
/// coverage, because a single `case Opcode::X` arm routinely lowers some
/// operand shapes natively and bails to the interpreter delegate on the rest.
/// Measured on this tree, the single most common bail (88 sites) is an XMM
/// register index whose CpuState byte offset does not fit an LDR immediate —
/// i.e. `PADDB xmm1, xmm2` is native while `PADDB xmm9, xmm10` delegates,
/// under one opcode and one `case` arm.
///
/// So coverage is keyed by (Opcode, FormKey, bitness). FormKey is packed from
/// the decoded instruction and captures exactly the properties that lowerings
/// have been observed to switch on. It is deliberately NOT a hash of the
/// encoding: it must be small, enumerable, and readable in a report.

#pragma once

#include "../decode/decoder.hpp"

#include <vitreo/types.hpp>

namespace vitreo::cpu::jit {

/// Packed operand/prefix shape. 16 bits, stable across builds: the bit
/// positions are part of the committed matrix baseline
/// (`tools/isa_matrix.json`), so insert new properties at the top rather than
/// renumbering.
struct FormKey {
    u16 bits{0};

    enum : u16 {
        /// Some operand is a memory reference.
        kMem        = 1u << 0,
        /// Operand 0 (the destination) is the memory reference.
        kMemDst     = 1u << 1,
        /// Some operand is an immediate.
        kImm        = 1u << 2,
        /// The immediate is wider than 8 bits.
        kImmWide    = 1u << 3,
        /// REX.W — 64-bit operand size.
        kRexW       = 1u << 4,
        /// Any register operand (or the memory base/index) has index >= 8.
        /// The XMM8-15 / R8-R15 axis: the single biggest source of partial
        /// lowerings on this tree.
        kHighReg    = 1u << 5,
        /// 66H operand-size override.
        kOpSize16   = 1u << 6,
        /// 67H address-size override.
        kAddrSize   = 1u << 7,
        /// FS or GS segment override.
        kSegFsGs    = 1u << 8,
        /// Any other segment override.
        kSegOther   = 1u << 9,
        /// VEX-encoded.
        kVex        = 1u << 10,
        /// VEX.L = 1 (256-bit).
        kVexL       = 1u << 11,
        /// VEX.vvvv selects a register other than the destination — the
        /// non-destructive three-operand shape whose vvvv was ignored when
        /// VEX was lowered as legacy SSE.
        kVexNds     = 1u << 12,
        /// LOCK prefix.
        kLock       = 1u << 13,
        /// REP/REPNE prefix.
        kRep        = 1u << 14,
        /// RIP-relative or absolute (moffs) memory operand.
        kRipOrAbs   = 1u << 15,
    };

    constexpr bool operator==(const FormKey& o) const noexcept {
        return bits == o.bits;
    }
};

/// Derive the form of a decoded instruction.
[[nodiscard]] constexpr FormKey form_key(const DecodedInstruction& in) noexcept {
    u16 b = 0;
    for (u8 i = 0; i < in.operand_count; ++i) {
        const Operand& op = in.operands[i];
        if (is_memory_operand(op.type)) {
            b |= FormKey::kMem;
            if (i == 0) b |= FormKey::kMemDst;
            if (op.mem.base == 16 || op.mem.has_absolute) {
                b |= FormKey::kRipOrAbs;
            }
            if ((op.mem.base != 255 && op.mem.base != 16 && op.mem.base >= 8) ||
                (op.mem.index != 255 && op.mem.index >= 8)) {
                b |= FormKey::kHighReg;
            }
        } else if (is_register_operand(op.type)) {
            if (op.reg >= 8) b |= FormKey::kHighReg;
        } else {
            switch (op.type) {
            case OperandType::Imm8:
                b |= FormKey::kImm;
                break;
            case OperandType::Imm16:
            case OperandType::Imm32:
            case OperandType::Imm64:
                b |= static_cast<u16>(FormKey::kImm | FormKey::kImmWide);
                break;
            default:
                break;
            }
        }
    }

    const Prefixes& p = in.prefixes;
    if (p.rex_w()) b |= FormKey::kRexW;
    if (p.operand_size) b |= FormKey::kOpSize16;
    if (p.address_size) b |= FormKey::kAddrSize;
    if (p.segment == SegReg::FS || p.segment == SegReg::GS) {
        b |= FormKey::kSegFsGs;
    } else if (p.segment != SegReg::Default) {
        b |= FormKey::kSegOther;
    }
    if (p.vex) {
        b |= FormKey::kVex;
        if (p.vex_l) b |= FormKey::kVexL;
        if (in.operand_count > 0 && is_register_operand(in.operands[0].type) &&
            p.vex_vvvv != 0 && p.vex_vvvv != in.operands[0].reg) {
            b |= FormKey::kVexNds;
        }
        if (p.vex_vvvv >= 8) b |= FormKey::kHighReg;
    }
    if (p.lock) b |= FormKey::kLock;
    if (p.rep || p.repne) b |= FormKey::kRep;

    return FormKey{b};
}

/// How the JIT actually translated one instruction.
///
/// `Helper` exists because `Native` used to mean "did not take the interpreter
/// delegate", which counted a `BLR` into `jit_sse_op` — a flat C++ switch that
/// runs the interpreter's own semantics — as a native lowering.
enum class LowerKind : u8 {
    /// Emitted ARM64 computes the architectural result.
    Native = 0,
    /// A C++ helper computes the architectural result.
    Helper = 1,
    /// The per-instruction interpreter delegate.
    Delegate = 2,
};

[[nodiscard]] constexpr const char* lower_kind_name(LowerKind k) noexcept {
    switch (k) {
    case LowerKind::Native:   return "native";
    case LowerKind::Helper:   return "helper";
    case LowerKind::Delegate: return "delegate";
    }
    return "?";
}

/// Render a FormKey as a short, stable, sorted tag list ("mem,high,vex.l").
/// Writes at most 96 bytes including the terminator.
void format_form_key(FormKey key, char* out, usize cap) noexcept;

}  // namespace vitreo::cpu::jit
