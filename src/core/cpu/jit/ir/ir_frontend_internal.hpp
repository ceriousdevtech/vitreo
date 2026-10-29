// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file ir_frontend_internal.hpp
/// @brief Declarations shared by the ir_frontend*.cpp translation units
///
/// ir_frontend.cpp was one 4,839-line file, so everything below was a
/// file-local symbol. Splitting it by pure code motion left these with users
/// in more than one of its parts; every other former file-local stays
/// file-local in the part that now holds all of its users. Private: include
/// it only from the ir_frontend*.cpp parts (not ir_frontend_stats.cpp),
/// never from a header.

#include "ir_frontend.hpp"

#include "core/cpu/decode/decoder.hpp"

#include <optional>

namespace vitreo::cpu::jit::ir {

using FlagOp = FlagState::FlagOp;

/// The decoder's `Operand` and this IR's `Operand` are different types with
/// the same name, and inside `namespace ir` the unqualified name resolves to
/// the IR's. Alias the decoder's rather than qualifying it at every use.
using X86Operand = vitreo::cpu::Operand;

/// The shape of an accepted instruction, as the builder needs to see it.
enum class Form : u8 {
    Unsupported = 0,
    /// dst = dst OP src, flags defined. Two operands.
    AluRmw,
    /// dst OP src computed for flags only, no register write (CMP, TEST).
    AluCompare,
    /// dst = ~dst. NOT is the one x86 ALU op that touches no flag.
    Not,
    /// dst = -dst, flags defined. One operand.
    Neg,
    /// dst = src, no flags.
    Mov,
    /// 128-bit bitwise XOR of two XMM registers. Register-register only: the
    /// memory form requires a 16-byte-aligned operand and raises #GP when it
    /// is not, which this slice does not model.
    XorpsRR,
    /// `MOVSS`, whose three forms disagree about bits 127:32 of the
    /// destination — see `Op::VInsertLow32`.
    Movss,
    /// `MOVAPS` between two XMM registers: a whole-register copy. The memory
    /// forms require a 16-byte-aligned operand and raise #GP otherwise, a
    /// fault this slice does not model, so they decline.
    MovapsRR,
    /// CMP/TEST at 8 or 16 bits. Flags only, no register write.
    ///
    /// This is the cheap half of partial-register support, and the reason it
    /// is a separate form from `AluCompare` rather than a width parameter on
    /// it: the operands are sub-register READS, so x86's merge-on-write rule
    /// — where `mov al,5` leaves EAX[31:8] untouched — never arises. The
    /// 32-bit path carries a lot of measured correctness (RMW fault ordering,
    /// flag publication order) that is not worth reopening to widen.
    NarrowCompare,
    /// MOVZX/MOVSX: read a narrow source, widen it, write a FULL 32-bit
    /// register. Also no partial write, for the same reason.
    Widen,
    /// `SHL`/`SHR`/`SAR r/m32, imm8` and the by-1 forms.
    ///
    /// Immediate counts only. A CL count is a different instruction: the
    /// count is a runtime value that may be ZERO, and a zero-count shift
    /// modifies no flags at all — so a CL shift cannot be a full-flag writer
    /// and needs a runtime branch this form does not emit. `flag_liveness.cpp`
    /// draws the same line, calling immediate shifts FullWriter and CL shifts
    /// Barrier.
    ShiftImm,
    /// `MOV` at 8 or 16 bits — the half that DOES need x86's merge rule.
    ///
    /// `mov al, 5` leaves EAX[31:8] untouched and `mov ah, 5` writes bits
    /// 15:8, unlike a 32-bit write which replaces the register. Lowered as a
    /// bitfield insert into the pin rather than a move.
    MovNarrow,
    /// dst = effective address, no memory access, no flags.
    Lea,
    /// Conditional branch, terminal.
    Jcc,
    /// Unconditional branch, terminal.
    Jmp,
    /// Branch to a runtime target, terminal.
    JmpIndirect,
    /// dst = dst +/- 1, preserving CF.
    IncDec,
    /// dst_reg pushed onto the guest stack.
    Push,
    /// dst_reg popped off the guest stack.
    Pop,
    /// Push the return address, then branch. Terminal.
    Call,
    /// Push the return address, then branch to a runtime target. Terminal.
    CallIndirect,
    /// Pop the return address into EIP. Terminal, indirect.
    Ret,
    /// ESP = EBP; EBP = pop().
    Leave,
    /// dst_reg = [mem], no flags.
    LoadReg,
    /// [mem] = src_reg, no flags.
    StoreReg,
};

struct Recipe {
    Form form{Form::Unsupported};
    Op op{Op::Add};              ///< meaningful for AluRmw/AluCompare
    FlagOp record{FlagOp::None}; ///< meaningful where flags are defined
    /// Operand width for the narrow forms. `I32` everywhere else.
    Type width{Type::I32};
};

/// x86 encoding index of ESP, and the register reference for it.
inline constexpr u8 kEspIndex = 4;

/// Every guest memory access declares `RipAndFlags`.
///
/// It is the stronger of the two, and always legal. `RipOnly` would be a claim
/// that no flag value is live across the access — true for many blocks, and a
/// claim this frontend has no business making, because the thing that decides
/// it is the flag-demand pass running LATER over this output. Declaring the
/// weaker state and being wrong means a fault exposes stale RFLAGS, which is
/// the exact defect the stencil path carries. Declaring the stronger one costs
/// nothing here: the frontend publishes an architectural record for every flag
/// definition anyway, so the state it promises is already there.
inline constexpr FaultState kMemFaultState = FaultState::RipAndFlags;

/// Result of one instruction attempt.
struct InsnVerdict {
    bool ok{false};
    DeclineReason reason{DeclineReason::None};
};

[[nodiscard]] inline bool is_xmm(const X86Operand& op) noexcept {
    return op.type == OperandType::Reg128;
}

/// The flag value live at this point, materializing the block's incoming
/// flags if nothing in the block has defined any yet.
///
/// Emitted lazily so a block that never reads flags carries no `FlagsIn` op.
[[nodiscard]] inline ValueId require_flags(IrBuilder& b, ValueId& live_flags) {
    if (live_flags == kInvalidValue) live_flags = b.flags_in();
    return live_flags;
}

/// Normalize an x86 register operand into this IR's register index space.
///
/// Only `Reg8` is ambiguous: encoding indices 4-7 are AH/CH/DH/BH without a
/// REX prefix and SPL/BPL/SIL/DIL with one. The prefix is in hand here and
/// nowhere downstream, so the high bytes are mapped into their own index
/// space now and every later consumer asks `is_high_byte`.
[[nodiscard]] inline u8 narrow_reg_index(const DecodedInstruction& insn,
                                  const X86Operand& op) noexcept {
    if (op.type == OperandType::Reg8 && !insn.prefixes.has_rex() &&
        op.reg >= 4 && op.reg <= 7)
        return static_cast<u8>(op.reg - 4 + kHighByteBase);
    return op.reg;
}

/// `NOP` (90), `NOP r/m` (0F 1F /0 and the 0F 0D/18-1E hint encodings the
/// decoder folds into it) and ENDBR32/64 with CET off: no state change at all.
/// Intel SDM Vol. 2B NOP: the r/m operand is never accessed, so no address is
/// formed and nothing can fault; operand-size, address-size, segment, REX and
/// F2/F3 prefixes only shape that unused operand. LOCK is #UD and the decoder
/// already rejects it; VEX/EVEX are not these encodings. Both decline here
/// rather than being dropped. Emits no IR. Shared by both frontends.
[[nodiscard]] inline std::optional<InsnVerdict>
try_nop(const DecodedInstruction& insn) noexcept {
    if (insn.opcode != Opcode::NOP && insn.opcode != Opcode::NOP_RM &&
        insn.opcode != Opcode::ENDBR32 && insn.opcode != Opcode::ENDBR64)
        return std::nullopt;
    const auto& px = insn.prefixes;
    if (px.lock || px.vex || px.evex)
        return InsnVerdict{false, DeclineReason::UnsupportedPrefix};
    return InsnVerdict{true, DeclineReason::None};
}

// --- Defined in ir_frontend_forms.cpp ---
[[nodiscard]] Recipe classify(Opcode opcode) noexcept;
[[nodiscard]] u8 jcc_condition(Opcode op) noexcept;
[[nodiscard]] bool sse_form_enabled(u32 bit) noexcept;

// --- Defined in ir_frontend32.cpp ---
[[nodiscard]] InsnVerdict build_insn(IrBuilder& b,
                                     const DecodedInstruction& insn,
                                     u64 next_rip, ValueId& live_flags);

// --- Defined in ir_frontend64.cpp ---
[[nodiscard]] ValueId build_long_mode_ea(IrBuilder& b,
                                            const MemoryOperand& mem,
                                            u64 next_rip, bool address32);
[[nodiscard]] InsnVerdict build_long_mode_insn(IrBuilder& builder,
                                              const DecodedInstruction& insn,
                                              u64 next_rip,
                                              ValueId& live_flags);

// --- Long-mode instruction families ---
//
// Each is one self-contained section of `build_long_mode_insn`, called in
// the original order. `std::nullopt` means "not this family, keep going";
// anything else is the verdict for the instruction.

/// What one long-mode family returns: the instruction's verdict, or
/// `std::nullopt` when the instruction is not in that family.
///
/// A `std::optional<InsnVerdict>` that can also be built from the two
/// verdict fields, so the moved sections' `return verdict;` and
/// `return {true, DeclineReason::None};` compile unchanged: a two-element
/// braced list cannot initialize a plain `std::optional`.
struct FamilyVerdict : std::optional<InsnVerdict> {
    using std::optional<InsnVerdict>::optional;
    FamilyVerdict(bool ok, DeclineReason reason) noexcept
        : std::optional<InsnVerdict>(InsnVerdict{ok, reason}) {}
};

// ir_frontend64_mov.cpp
[[nodiscard]] FamilyVerdict try_long_mode_widen(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);
[[nodiscard]] FamilyVerdict try_long_mode_acc_sign_extend(
    IrBuilder& builder, const DecodedInstruction& insn);
[[nodiscard]] FamilyVerdict try_long_mode_cmov(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_setcc(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_lea(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);
[[nodiscard]] FamilyVerdict try_long_mode_moffs(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);
[[nodiscard]] FamilyVerdict try_long_mode_mov_narrow(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);

// ir_frontend64_alu.cpp
[[nodiscard]] FamilyVerdict try_long_mode_narrow_unary(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_carry_alu(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_cmpxchg(
    IrBuilder& builder, const DecodedInstruction& insn, ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_xchg_xadd(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_narrow_alu(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);

// ir_frontend64_mul_div.cpp
[[nodiscard]] FamilyVerdict try_long_mode_divide(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);
[[nodiscard]] FamilyVerdict try_long_mode_mul_accumulator(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);
[[nodiscard]] FamilyVerdict try_long_mode_imul(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);

// ir_frontend64_shifts.cpp
[[nodiscard]] FamilyVerdict try_long_mode_double_shift(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_shift_rotate64(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_shift_cl(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_shift_imm(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);

// ir_frontend64_rotates.cpp
[[nodiscard]] FamilyVerdict try_long_mode_rcl_rcr_imm(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_rotate_cl(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);
[[nodiscard]] FamilyVerdict try_long_mode_rotate_imm(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);

// ir_frontend64_sse.cpp
[[nodiscard]] FamilyVerdict try_long_mode_sse(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip);

// ir_frontend64_stack.cpp
[[nodiscard]] FamilyVerdict try_long_mode_stack(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip,
    ValueId& live_flags);

}  // namespace vitreo::cpu::jit::ir
