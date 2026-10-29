// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_ir.hpp
/// @brief Stage 1 optimizing block IR for the x86 -> ARM64 JIT: data model.
///
/// Canonical typed block IR for integer, memory, control-flow and selected
/// vector operations, with explicit flag values and fault-state obligations.
/// Frontend coverage and ARM64 backend feasibility are separate gates; the
/// existence of an IR opcode does not imply every guest form is lowerable.
///
/// Why an IR at all, in one paragraph. The stencil compiler carries every
/// cross-instruction dataflow fact as a mutable member of `JitCompiler`
/// (plan §2 lists them: twelve fields for flags alone, `m_flags.nzcv`,
/// `m_flags.incoming_nzcv`, `m_flags.hold_nzcv`, `m_flags.stub_spill_nzcv`, `m_flags.nzcv_hold_depth`,
/// `m_flags.pred_nzcv_live`, `m_flags.preserved_pred_nzcv`, `m_flags.pending_carry_source` /
/// `m_flags.incoming_carry_source`, `m_pending_stub_store` / `m_incoming_stub_store`,
/// `m_pending_alu32_nz_result` / `m_incoming_alu32_nz_result`, and the
/// `m_omit_lazy*` cluster). They exist because there is nowhere to put a flag
/// *value*. Here there is: a flag definition is an SSA value whose
/// representation is an element of a small lattice
/// (`{Dead, InPstate(kind), InCpuStateRecord(FlagOp), InRflags}`), and the
/// facts those twelve fields smear across the emit loop become properties of
/// one node that a verifier can check.
///
/// Three constraints from plan §3 (fault-state reconstruction) are baked into
/// the node layout rather than left to convention:
///
///  1. **`guest_rip` is mandatory on every op.** It feeds the host-PC ->
///     guest-RIP side table the hardware fault handler reads
///     (`linear_fault.cpp:432-500` via `jit_fault_metadata_for_pc`). An op
///     with no owning guest instruction cannot be attributed at a fault, so
///     the verifier rejects it rather than letting codegen invent one.
///  2. **`may_fault` is explicit**, and the verifier enforces that reordering
///     stays inside a single guest instruction's RIP interval (ops must be
///     grouped in ascending, contiguous runs of `guest_rip`).
///  3. **Flags at a fault.** The signal handler does not persist host NZCV.
///     A flag value that is live across a `may_fault` op therefore may not be
///     `Dead` and may not live only `InPstate`; it must be architecturally
///     visible (`InCpuStateRecord` or `InRflags`), or covered by a verified
///     recovery plan. This obligation applies independently of the stencil
///     compiler's own fault-aware flag-liveness checks.
///
/// Width discipline. There is **no implicit width coercion anywhere**. Every
/// operand of a binary op has exactly the op's result type; narrowing and
/// widening are the explicit `Zext`/`Sext` opcodes. x86 partial-register
/// writes are a recorded correctness minefield (see the zero-count shift
/// zero-extension bug, where JIT and interpreter were wrong *identically* so
/// parity tests stayed green); making the width explicit at every edge is what
/// lets the verifier see the mistake instead of the tests missing it.
///
/// Dependencies: this header knows nothing about `JitCompiler` and pulls in no
/// ARM64 codegen. That is why its tests run without one, and why `jit_ir.cpp`
/// builds on the x86_64 host too. Its only Vitreo dependency is
/// `CpuState::FlagState::FlagOp`, which *is* the lazy-record lattice element —
/// mirroring that enum here would only invite it to drift.

#pragma once

#include "vitreo/types.hpp"
#include "core/cpu/state/flag_state.hpp"

#include <string>
#include <vector>

namespace vitreo::cpu::jit::ir {

// =============================================================================
// Value and type model
// =============================================================================

/// SSA value handle. A plain dense index into the block's value numbering, not
/// a pointer: see the allocation note on `IrBlock`.
using ValueId = u32;

/// "No value" — an op that defines nothing, or an absent optional operand.
inline constexpr ValueId kInvalidValue = 0xFFFF'FFFFu;

/// An op with `guest_rip == kInvalidRip` is malformed. Zero is a safe sentinel:
/// no guest instruction the JIT compiles lives at guest linear address 0.
inline constexpr u64 kInvalidRip = 0;

/// SSA value types. Integer widths only in Stage 1; `Flags` is the type of a
/// flag definition (see `FlagDef`). Vector/float types get added here.
///
/// `Void` is the type of an op that defines no value (`GprWrite`). It is never
/// a legal operand type.
enum class Type : u8 { Void = 0, I8, I16, I32, I64, Flags, V128 };

/// Index space for the legacy high bytes AH/CH/DH/BH, bits 15:8 of GPRs 0-3.
///
/// x86 spells them as `Reg8` encoding indices 4-7 — but only WITHOUT a REX
/// prefix. With REX those same indices mean SPL/BPL/SIL/DIL, four completely
/// different architectural registers. An IR that stored the raw encoding index
/// could not tell the two apart, so the high bytes get an index space of their
/// own and the frontend normalizes on the way in (`narrow_reg_index`), where
/// the prefix is still in hand.
///
/// This is deliberately not a flag on the operand: three places need the same
/// answer and disagreeing is silent — the verifier, the emitter's
/// extract/insert, and the pin tombstone. Getting the tombstone wrong in
/// particular means `mov ah, 5` invalidates RSP's pin instead of RAX's, a
/// wrong register arbitrarily later.
inline constexpr u8 kHighByteBase = 16;

/// True when a `(type, index)` operand pair names a legacy HIGH byte.
[[nodiscard]] constexpr bool is_high_byte(Type t, u8 index) noexcept {
    return t == Type::I8 && index >= kHighByteBase &&
           index < static_cast<u8>(kHighByteBase + 4);
}

/// The architectural GPR a `(type, index)` operand pair touches.
[[nodiscard]] constexpr u8 architectural_gpr(Type t, u8 index) noexcept {
    return is_high_byte(t, index) ? static_cast<u8>(index - kHighByteBase) : index;
}

/// Bit position of a narrow field within its architectural GPR.
[[nodiscard]] constexpr u8 subregister_lsb(Type t, u8 index) noexcept {
    return is_high_byte(t, index) ? 8 : 0;
}

/// Width in bits, or 0 for `Void`/`Flags` (which have no integer width).
[[nodiscard]] constexpr u32 type_bits(Type t) noexcept {
    switch (t) {
        case Type::I8:  return 8;
        case Type::I16: return 16;
        case Type::I32: return 32;
        case Type::I64: return 64;
        case Type::V128: return 128;
        default:        return 0;
    }
}

/// True for the integer types, i.e. the ones with a defined width.
[[nodiscard]] constexpr bool is_integer_type(Type t) noexcept {
    return type_bits(t) != 0;
}

[[nodiscard]] const char* type_name(Type t) noexcept;

// =============================================================================
// Guest register references
// =============================================================================

/// A reference to an architectural guest register.
///
/// Stage 1 covers 32-bit guests, where all eight GPRs are already pinned to
/// host registers (`jit_compiler.cpp:4921-4928`), so plan §3.2 Option A holds
/// for free: **guest registers keep the existing fixed pin ABI as their only
/// home, and the IR allocates temporaries only.** A guest register in a host
/// register outside `JitPinMap` is invisible to the fault handler and silently
/// lost, surfacing as a corrupted register inside an SEH handler arbitrarily
/// far from the cause. `GprRead`/`GprWrite` therefore stay explicit ops rather
/// than becoming ordinary SSA values with a register allocator behind them:
/// the read/write is where the pin ABI is honoured.
///
/// `index` uses x86 encoding order (0=A, 1=C, 2=D, 3=B, 4=SP, 5=BP, 6=SI,
/// 7=DI, 8-15 = R8-R15). `width` is the *access* width, and it must equal the
/// value type at the op — a 32-bit guest writing AL is a read-modify-write the
/// builder must spell out, never an implicit narrowing.
struct GuestReg {
    u8 index{0};
    Type width{Type::I32};

    [[nodiscard]] friend constexpr bool operator==(GuestReg a,
                                                   GuestReg b) noexcept {
        return a.index == b.index && a.width == b.width;
    }
};

/// Highest guest GPR index this IR will accept. 16 so x64 needs no reshape;
/// `IrBlock::is_64bit` is what restricts a 32-bit block to 0-7.
inline constexpr u8 kMaxGuestGprIndex = 15;

/// Highest index a `GuestReg` OPERAND may carry. Above `kMaxGuestGprIndex`
/// only the four high-byte slots exist, and only at `I8`. A memory base or
/// index register is a real register and still bounded by `kMaxGuestGprIndex`.
inline constexpr u8 kMaxGuestRegOperandIndex =
    static_cast<u8>(kHighByteBase + 3);

[[nodiscard]] const char* gpr_name(GuestReg reg, bool is_64bit) noexcept;

// =============================================================================
// Operands
// =============================================================================

enum class OperandKind : u8 {
    None = 0,   ///< Absent optional operand (e.g. `Lea` with no index).
    Value,      ///< An SSA value defined earlier in this block.
    Imm,        ///< A literal, already sign- or zero-extended to 64 bits.
    GuestReg,   ///< An architectural register reference (GprRead/GprWrite).
};

/// A single operand. Deliberately POD and 16 bytes: operands live inline in
/// the op node, so a block's whole IR is one contiguous array with no pointer
/// chasing and no per-node allocation.
struct Operand {
    OperandKind kind{OperandKind::None};
    /// For `Value`: the type the *user* expects. The verifier checks it
    /// against the definition's actual type, so a width mismatch is caught at
    /// the edge that made it rather than wherever it later goes wrong.
    /// For `GuestReg`: the access width (duplicated in `reg.width`).
    Type type{Type::Void};
    u8 reg_index{0};
    u8 pad{0};
    /// `Value` -> ValueId; `Imm` -> the literal bits; otherwise 0.
    u64 payload{0};

    [[nodiscard]] static Operand value(ValueId v, Type t) noexcept {
        Operand o;
        o.kind = OperandKind::Value;
        o.type = t;
        o.payload = v;
        return o;
    }
    [[nodiscard]] static Operand imm(u64 bits, Type t) noexcept {
        Operand o;
        o.kind = OperandKind::Imm;
        o.type = t;
        o.payload = bits;
        return o;
    }
    [[nodiscard]] static Operand gpr(GuestReg r) noexcept {
        Operand o;
        o.kind = OperandKind::GuestReg;
        o.type = r.width;
        o.reg_index = r.index;
        return o;
    }

    [[nodiscard]] ValueId as_value() const noexcept {
        return static_cast<ValueId>(payload);
    }
    [[nodiscard]] GuestReg as_gpr() const noexcept {
        return GuestReg{reg_index, type};
    }
};

// =============================================================================
// Opcodes
// =============================================================================

/// Stage 1 op set. Register-only integer ALU, plus the two ops that touch the
/// guest register file and the one that defines flags. Memory, compares/Jcc,
/// calls/stack, LOCK, and SIMD add enumerators here.
enum class Op : u8 {
    /// Read an architectural GPR out of its pin. One value operand slot used:
    /// operand 0 is the `GuestReg`. Result type must equal the access width.
    GprRead = 0,
    /// Write an architectural GPR back to its pin. Operand 0 is the
    /// `GuestReg`, operand 1 the value. Defines no SSA value.
    GprWrite,
    /// Materialize a literal. Operand 0 is `Imm`, which must fit the result
    /// type either zero- or sign-extended (no silent truncation).
    Const,

    /// The architectural flags as they exist at block ENTRY. Result type is
    /// `Flags`; no operands.
    ///
    /// The first genuine block INPUT value in this IR, and it exists because
    /// of one instruction family. Everything else a block reads is a
    /// `GprRead`, which is an op that reads a known home (a pin). Flags have
    /// no single home — they may be a lazy `FlagOp` record, a CF/OF overlay,
    /// or materialized RFLAGS, and which one is a property of whatever ran
    /// before this block. `FlagsIn` names "the architectural flags here,
    /// however they are currently represented".
    ///
    /// What needs it: `INC` and `DEC` PRESERVE CF. Their flag definition is
    /// therefore a MERGE of a new computation with the incoming flag value,
    /// not a fresh definition — plan M3's "INC/DEC CF preservation as a merge,
    /// not mutable overlay state". The stencil compiler expresses that merge
    /// as a register handed to `emit_set_lazy_flags_preserve_cf`; here it is a
    /// value edge the verifier can check.
    FlagsIn,

    // Binary integer ALU. Two operands, both exactly the result type.
    Add,
    Sub,
    And,
    Or,
    Xor,

    // Unary integer ALU. One operand, exactly the result type.
    Not,
    Neg,

    // Shifts. Operand 0 is the value (result type); operand 1 is the count,
    // either an `Imm` in [0, width) or an I8 value. The count is I8 and not
    // "whatever the source is" on purpose: a 32-bit shift by a 64-bit value
    // is a category error, and a zero count still zero-extends bits 63:32 of
    // a 32-bit destination (a recorded bug where JIT and interpreter were
    // wrong identically). Zero-extension is `Zext`, never a shift side effect.
    Shl,
    Shr,
    Sar,

    /// Low half of an integer product. Two operands, both the result type.
    ///
    /// Only the LOW half, at the result's width: that is what the destination
    /// of every multiply form receives. Whether the product OVERFLOWED that
    /// width is a separate question, and asking it needs the high half --
    /// `SMulH` below for a 64-bit signed product, and for the narrower widths
    /// a `Mul` at I64 on sign- or zero-extended operands, where the full
    /// product fits and the high half is just its upper bits.
    Mul,
    /// High 64 bits of a SIGNED 64x64 product. I64 only, two I64 operands.
    /// There is no I64 in which the full 128-bit product fits, so IMUL at 64
    /// bits cannot ask the overflow question any other way.
    SMulH,

    /// A segment SELECTOR, zero-extended, for `PUSH FS` / `PUSH GS`.
    ///
    /// Operand 0 is an `Imm` naming the segment. Deliberately not `SegBase`:
    /// the base is a cached linear address and the selector is the 16-bit
    /// value the guest expects on the stack, so pushing one for the other
    /// would store a plausible number that means something else entirely.
    SegSelector,

    /// DIV / IDIV at any width, writing the implicit accumulator pair.
    ///
    /// Operand 0 is the divisor; operand 1 is an `Imm` naming which of the
    /// four forms this is. It MAY FAULT: a zero divisor, or a quotient that
    /// does not fit the destination, raises #DE and **commits nothing** --
    /// which is why it is a single op with a helper lowering rather than
    /// arithmetic the frontend could build. The decision and the writes have
    /// to be indivisible, and the IR has no control flow inside an
    /// instruction to make them so.
    Divide,

    /// The whole architectural EFLAGS as an I64 value, for `PUSHF`.
    ///
    /// Operand 0 is the block's live `Flags` value, which is what ORDERS this
    /// after any pending definition -- PUSHF must observe the flags the
    /// preceding arithmetic produced, not a stale RFLAGS image. The lowering
    /// is a helper call that materializes the lazy record and applies the
    /// architectural read mask, so the masking rule lives in one place shared
    /// with the interpreter rather than being re-derived in emitted code.
    FlagsImage,

    /// Install an I64 value into the architectural EFLAGS, for `POPF`.
    ///
    /// Defines no value. The helper applies the writable-bit mask and clears
    /// the pending lazy record -- a POPF is a full flag write, so anything the
    /// block still owed is dead.
    FlagsInstall,

    /// Force CF or OF, leaving every other flag alone.
    ///
    /// This is what a PARTIAL flag writer needs and `DefFlags` cannot give it.
    /// A lazy record replaces the whole flag state, so publishing one for a
    /// rotate would discard the pending record a preceding instruction owed
    /// and ZF/SF/PF/AF would materialize from stale RFLAGS. `CpuState` already
    /// models this: `write_cf_only` sets the overlay byte AND RFLAGS, and
    /// `set_flag_op` clears the overlays because a full-flag writer owns CF
    /// and OF again.
    ///
    /// Operand 0 is a value whose BIT 0 is the flag; the lowering masks it.
    /// Defines no value, so it is `Void`-typed like `GprWrite`.
    SetCf,
    SetOf,

    /// Evaluate an x86 condition code against a flag value, yielding 0 or 1.
    ///
    /// The complement of `Jcc`: that one CONSUMES a condition to pick an edge,
    /// this one turns it into data. SETcc needs it directly; CMOVcc needs it to
    /// build a select; and ADC/SBB need it to get CF as an integer, because
    /// their records carry the carry-in INSIDE the operand
    /// (`flag_src = (cf_in << 32) | src`).
    ///
    /// Operand 0 is the flag value, operand 1 the x86 condition as an `Imm`
    /// in 0x0-0xF. The result is `I8`, always 0 or 1. Lowering it may call the
    /// condition helper, so it clobbers the scratch pool like a memory access.
    EvalCc,

    /// `cond ? a : b`, where `cond` is a value and NOT host NZCV.
    ///
    /// CMOVcc needs this, and needs it value-based: the condition comes from
    /// `EvalCc`, which walks the lazy record, so there is no NZCV to select
    /// on. Operands: 0 = condition (nonzero picks `a`), 1 = `a`, 2 = `b`.
    /// Operands 1 and 2 and the result all share the op's type.
    ///
    /// Both arms are evaluated, which is what CMOV wants: Intel SDM Vol. 2A
    /// says the memory source is read whether or not the condition holds.
    Select,

    /// Narrow a value to a smaller integer type, keeping the low bits.
    ///
    /// The counterpart of `Zext`/`Sext`, and missing until a lowering needed
    /// to compute wider than it writes: RCL/RCR rotate a (width+1)-bit
    /// quantity because CF rides on top of the operand, so the arithmetic
    /// happens at I64 and the destination takes the low bits back. The width
    /// discipline makes that explicit rather than letting a consumer quietly
    /// read a narrower slice of a wider value.
    Trunc,

    /// Rotates. Same operand shape as the shifts, but the count is the
    /// ALREADY-REDUCED rotation amount in [0, width): x86 masks to 5 bits and
    /// then rotates by that modulo the operand width, and doing the reduction
    /// in the lowering keeps the guest rule where the guest rule belongs.
    /// A rotate touches only CF and OF; ZF/SF/PF/AF are unaffected.
    Rol,
    Ror,

    /// base + index*scale + disp, computed without touching memory. Operands:
    /// 0 = base value (result type), 1 = index value or `None`, 2 = scale
    /// `Imm` in {1,2,4,8}, 3 = displacement `Imm`. Kept distinct from
    /// Add/Shl so Stage 2's EA-commoning pass has a single node shape to CSE —
    /// the thing `m_esp_frame_live` (plan §2) is a one-entry hand-rolled
    /// version of.
    Lea,

    /// Read a segment's base address out of `CpuState`. Operand 0 carries the
    /// `SegReg` as an immediate; the result is `I32`.
    ///
    /// An explicit VALUE rather than a hidden field on `Lea`, so an EA that
    /// includes a segment base is an ordinary expression the same passes can
    /// see. FS is what makes this worth having: `mov eax, fs:[0x18]` is how a
    /// Win32 guest reaches its TEB, and on RollerCoaster Tycoon 3 the FS
    /// override alone was 3014 declines — 35% of everything left.
    SegBase,

    /// Read an architectural XMM register out of `CpuState`. Operand 0 is the
    /// register index as a `GuestReg`; the result is `V128`.
    ///
    /// Unlike `GprRead` this is always a real load. There is no vector pin
    /// map, and there cannot usefully be one: the fault handler recovers GPRs
    /// from `ucontext` using `JitPinMap` and saves NO vector state at all, so
    /// an XMM value held in a NEON register across a faultable access would be
    /// unrecoverable. Tier 0 reaches the same conclusion by construction —
    /// every one of its SSE lowerings is load-compute-store within a single
    /// guest instruction.
    XmmRead,

    /// Write an architectural XMM register. Operand 0 is the register index,
    /// operand 1 the `V128` value.
    XmmWrite,

    /// 128-bit bitwise XOR. `XORPS`/`XORPD` differ only in the exception class
    /// they would raise on a misaligned memory operand, which the
    /// register-register form this admits cannot reach.
    VXor,

    /// Element-wise packed integer operations on a 128-bit vector.
    ///
    /// The element WIDTH is a field (`Inst::vec.elem`) rather than a separate
    /// opcode per size, because the lowering differs only in the NEON size
    /// selector and duplicating eight ops four ways would be four chances to
    /// get one of them wrong. The guest opcode already names the width; the
    /// frontend translates it once.
    ///
    /// Signed and unsigned MIN/MAX are distinct ops rather than a flag: x86
    /// gives them different mnemonics (PMINUB versus PMINSW) because they are
    /// different operations, and a flag would let a frontend silently pick the
    /// wrong one.
    VAdd,
    VSub,
    VCmpEq,
    VCmpGt,     ///< SIGNED greater-than, which is what PCMPGT* means.
    VMinU,
    VMaxU,
    VMinS,
    VMaxS,
    VMulLo,     ///< PMULLW: the LOW half of each element product.
    VAvgU,      ///< PAVGB/PAVGW: unsigned rounding average.
    /// Saturating packed add and subtract, signed and unsigned:
    /// PADDSB/PADDSW, PADDUSB/PADDUSW, PSUBSB/PSUBSW, PSUBUSB/PSUBUSW.
    /// x86 clamps to the element's own limits rather than wrapping, which is
    /// exactly ARM64 SQADD/UQADD/SQSUB/UQSUB. Unlike the min/max family these
    /// DO have a 64-bit element form, so they take the ordinary VSize rule.
    VQAddS,
    VQAddU,
    VQSubS,
    VQSubU,
    /// PABSB/PABSW/PABSD: per-element absolute value. x86 and ARM64 agree on
    /// the one interesting case -- the most negative element is its own
    /// absolute value, saturating nowhere and wrapping to itself.
    VAbs,
    VZipLo,     ///< PUNPCKL*: interleave the LOW halves, destination first.
    VZipHi,     ///< PUNPCKH*: the same from the high halves.
    /// PACKSSWB/PACKSSDW and PACKUSWB: narrow both sources one element size
    /// with saturation, destination into the low half and source into the
    /// high half. `vec_elem` is the SOURCE element size.
    VPackS,
    VPackU,
    /// Element-wise shift by a compile-time count. Operand 1 is an `Imm`.
    /// x86 saturates an out-of-range count to "all bits shifted out" rather
    /// than masking it, which the frontend resolves before building these.
    VShlImm,
    VShrImm,    ///< logical
    VSarImm,    ///< arithmetic

    /// 128-bit bitwise AND and OR. Like `VXor`, the single- and
    /// double-precision spellings differ only in the exception class a
    /// MISALIGNED memory operand would raise, which the register-register
    /// forms admitted here cannot reach, and the integer spellings (`PAND`,
    /// `POR`, `PXOR`) compute the same bits again.
    VAnd,
    VOr,

    /// Insert operand 1's low 32 bits into operand 0's lane 0, keeping bits
    /// 127:32 of operand 0. This is `MOVSS xmm, xmm` — and the reason it is a
    /// distinct op from `VZext32` is that the two MOVSS forms disagree here.
    /// Measured against the interpreter: register-to-register PRESERVES bits
    /// 127:32, a load ZEROES them. The difference is invisible whenever the
    /// destination's upper lanes are already zero, which right after the
    /// `xorps xmm,xmm` idiom they usually are.
    VInsertLow32,

    /// Widen an `I32` to `V128`, zeroing bits 127:32. This is `MOVSS xmm, m32`
    /// once the load itself has been done as an ordinary 32-bit guest load.
    VZext32,

    /// Extract bits 31:0 of a `V128` as an `I32`, for `MOVSS m32, xmm`.
    VLow32,

    /// Explicit widening. Operand 0's type must be strictly narrower than the
    /// result type. There is no implicit coercion anywhere in this IR.
    Zext,
    Sext,

    /// Define the architectural flags. Result type is `Flags`; the value
    /// sources are operands 0..n (all the same integer type). The
    /// representation lattice lives in `Inst::flags` — see `FlagDef`.
    DefFlags,

    /// Read guest memory. Operand 0 is the ADDRESS value; the result type is
    /// the access width. Always `may_fault`.
    ///
    /// The address is a value, not a `MemoryOperand`, because address
    /// arithmetic is `Lea` — a separate node the EA-commoning pass can CSE.
    /// Keeping them apart is the point of plan §C: "represent LEA/effective
    /// address construction independently from loads and stores".
    Load,

    /// Write guest memory. Operand 0 is the ADDRESS value, operand 1 the value
    /// stored (its type is the access width). Defines no SSA value. Always
    /// `may_fault`.
    Store,

    /// Conditional branch on the architectural flags. Operand 0 is the `Flags`
    /// value it tests; the condition code and both edge targets live in
    /// `Inst::branch`. Defines no SSA value.
    ///
    /// **TERMINATOR.** See `is_terminator` for why that is enforced rather
    /// than merely expected.
    Jcc,

    /// Unconditional branch. No operands; the target is in `Inst::branch`.
    /// **TERMINATOR.**
    Jmp,

    /// Branch to a RUNTIME address. Operand 0 is the target value (the guest's
    /// pointer width). `Inst::branch` is unused — there is no static successor
    /// to name, which is the whole difference from `Jmp`.
    ///
    /// This is `RET`, `JMP r/m` and `CALL r/m`. **TERMINATOR.**
    JmpIndirect,

    /// A conditional branch that is NOT the block's last instruction: a long-
    /// mode trace continues along the fall-through, so the taken edge leaves
    /// the block and the not-taken edge simply continues with the next op.
    /// Operand 0 is the `Flags` value it tests -- the block's LIVE flags at
    /// that point, which is what keeps `eliminate_dead_flags` sound: the exit
    /// edge leaves with every flag live, and naming the live definition as an
    /// operand makes it a use, so its record is published before the exit --
    /// or, for a definition dead on the fall-through, on the taken edge only
    /// (`FlagDef::exit_deferred`).
    /// `Inst::branch` holds the condition, the taken target and the
    /// fall-through (the next instruction). Defines no value. Every guest
    /// register write before it is already committed, so the exit needs no
    /// state of its own. NOT a terminator.
    CondExit,

    /// ADC's and SBB's arithmetic: `a + b + cf` and `a - b - cf`, where
    /// operand 2 is CF as an `I8` 0/1 (an `EvalCc` of condition B). Operands 0
    /// and 1 and the result share the op's type, I8 through I32 (there is no
    /// 64-bit ADC/SBB record to publish).
    ///
    /// One op rather than two adds so the backend can lower it to ADCS/SBCS
    /// and take the carry straight from host C when the producer of the flags
    /// it reads left it there: Intel SDM Vol. 2A ADC/SBB define every flag
    /// from the three-way sum, which is exactly what the ARM S-forms compute
    /// (C inverted for the subtraction).
    AddCarry,
    SubBorrow,
};

/// True for the ops that end a block.
///
/// The verifier requires a terminator to be the LAST op in its block, and that
/// rule is load-bearing well beyond tidiness: it is what keeps
/// `eliminate_dead_flags` sound. That pass finds a flag definition's killer
/// with a linear forward scan, which would be wrong if a branch could sit
/// between a definition and its killer — an escaping edge would leave the
/// block with the elided record still owed, which is exactly why the stencil
/// path replays it from `m_pending_stub_store` on the taken edge.
///
/// Because a terminator must be last, nothing can redefine flags after a
/// branch, so no definition is ever killed across one. The linear scan stays
/// correct by construction rather than by a reviewer noticing.
[[nodiscard]] constexpr bool is_terminator(Op op) noexcept {
    return op == Op::Jcc || op == Op::Jmp || op == Op::JmpIndirect;
}

[[nodiscard]] const char* op_name(Op op) noexcept;

/// Number of operand slots an op node carries inline. Four is what `Lea`
/// needs (base, index, scale, disp) and nothing needs more.
inline constexpr u32 kMaxOperands = 4;

/// True for the lazy records that PRESERVE CF rather than defining it.
///
/// These are exactly the records whose `DefFlags` must carry a merge operand:
/// the flag value they inherit CF from. The verifier enforces the pairing in
/// both directions, so a CF-preserving record with no merge edge — or a
/// merge edge on a record that defines CF — is a malformed block rather than
/// a silently wrong carry.
[[nodiscard]] constexpr bool preserves_cf(FlagState::FlagOp op) noexcept {
    using F = FlagState::FlagOp;
    return op == F::Inc8 || op == F::Inc16 || op == F::Inc32 ||
           op == F::Inc64 || op == F::Dec8 || op == F::Dec16 ||
           op == F::Dec32 || op == F::Dec64;
}

// =============================================================================
// The flag representation lattice
// =============================================================================

/// Where a flag definition physically lives. This is the whole point of
/// Stage 1: plan §2 observes that the twelve mutable flag fields in
/// `JitCompiler` are one concept — *a flag value with a representation
/// lattice* — spread across an emit loop because there was no value to hang
/// it on.
///
/// The lattice is ordered by how observable the flags are, which is exactly
/// the order the fault rule cares about:
///
///   Dead              no consumer before the next full-flag writer, so
///                     nothing is emitted at all. This is what the
///                     `m_omit_lazy*` cluster and `compute_omit_lazy_flags`
///                     compute today, as a side table indexed by instruction.
///   InPstate(kind)    the value is in host NZCV, tagged with the kind that
///                     says how to read x86 semantics out of it. Mirrors
///                     `NzcvState` / `m_flags.nzcv` / `m_flags.incoming_nzcv`.
///                     **Not visible to a fault**: the signal handler does not
///                     persist NZCV.
///   InCpuStateRecord  a lazy `CpuState::FlagOp` record is published. This is
///                     the architecturally authoritative form wherever a fault
///                     can happen, which is the reason for the may_fault rule.
///   InRflags          RFLAGS bits fully materialized in CpuState.
///
/// A definition has exactly one representation in this IR. A pass that wants
/// two homes (NZCV *and* a published record, which the stencil compiler does
/// routinely for a fusable compare) expresses that as
/// `InCpuStateRecord` + `nzcv != NzcvKind::None`: the record is the
/// architectural home, the NZCV kind is an additional fast-path fact.
enum class FlagRepr : u8 { Dead = 0, InPstate, InCpuStateRecord, InRflags };

/// Mirrors `NzcvState`. Deliberately a copy rather than an
/// include: this header must not depend on `jit_compiler.hpp` (plan §1,
/// Seam A — the IR has to be independently testable, and its tests must run
/// without a `JitCompiler`). The enumerators and their order are kept
/// identical so the backend's conversion is a cast; that conversion, and the
/// `static_assert` pinning the two enums together, belong in the backend
/// translation unit that can see both.
///
/// `NzOnly` is the subtle one and the comment is worth repeating: a
/// nonzero-count shift republished its result with TST, so only N and Z carry
/// the result's SF/ZF. x86 CF (the shifted-out bit) and OF live solely in the
/// lazy record, and TST cleared ARM C/V. Only Z/S conditions may be fused off
/// it.
enum class NzcvKind : u8 {
    None = 0, Sub, And, Add, IncDec, Sub8, Carried, NzOnly
};

[[nodiscard]] const char* flag_repr_name(FlagRepr r) noexcept;
[[nodiscard]] const char* nzcv_kind_name(NzcvKind k) noexcept;

/// The NZCV kind a record's ARM flag-setting form would leave in PSTATE, or
/// `NzcvKind::None` for a record with no such form.
///
/// This is a SEMANTIC property of the record — "if you computed this with the
/// S-form, host flags would mean K" — so it lives with the IR. Whether a
/// particular x86 condition can actually be read out of kind K is a BACKEND
/// question (ARM and x86 disagree about the carry sense, ANDS clears C and V,
/// and parity has no NZCV equivalent at all), and stays in the backend.
[[nodiscard]] constexpr NzcvKind nzcv_kind_for(FlagState::FlagOp op) noexcept {
    using F = FlagState::FlagOp;
    switch (op) {
        case F::Sub32: return NzcvKind::Sub;
        case F::Add32: return NzcvKind::Add;
        case F::And32: return NzcvKind::And;
        case F::Inc32: case F::Dec32: return NzcvKind::IncDec;
        // The X-register S-forms leave the same meanings for the 64-bit
        // records: an operand-width SUBS/ADDS/ANDS is the claim, whatever
        // the width (the stencil compiler fuses x64 compares the same way).
        case F::Sub64: return NzcvKind::Sub;
        case F::Add64: return NzcvKind::Add;
        case F::And64: return NzcvKind::And;
        case F::Inc64: case F::Dec64: return NzcvKind::IncDec;
        // ADCS/SBCS define all four flags from the three-way sum exactly as
        // ADDS/SUBS do from the two-way one (Intel SDM Vol. 2A ADC/SBB): SF,
        // ZF and OF of the result, CF the carry out / ARM C the inverted
        // borrow. So they leave the Add and Sub meanings.
        case F::Adc32: return NzcvKind::Add;
        case F::Sbb32: return NzcvKind::Sub;
        // A zero-extended 8-bit SUBS leaves Z and C matching x86 ZF/CF (the
        // borrow is identical at 8 and 32 bits once the high 24 bits are
        // zero). N and V are 32-bit and do NOT match SF/OF, which is why this
        // is its own kind rather than `Sub`: the backend's `fused_arm_cond`
        // serves only the unsigned/equality conditions from `Sub8` and leaves
        // signed and overflow branches on the lazy path.
        //
        // Worth the special case because it is not a corner: a byte compare
        // feeding a JE/JNE is the shape of every string and buffer scan. Left
        // unmapped, the branch could not fuse and fell back to
        // `emit_eval_lazy_condition`, a ~147-instruction runtime dispatch over
        // every FlagOp kind -- 147 ARM words against Tier 0's 7 on the hottest
        // block of the lzss workload, which alone made Tier 1 1.79x SLOWER
        // there despite emitting equal or better code for every other
        // instruction in the same block.
        //
        // Deliberately NOT extended to Sub16/And8/And16: those have no
        // `NzcvState` counterpart, so `fused_arm_cond` would apply its
        // unrestricted filter and let a signed branch read 32-bit N/V that do
        // not describe the guest's narrow result.
        case F::Sub8: return NzcvKind::Sub8;
        // Or32/Xor32 have no ARM flag-setting form (there is no ORRS or EORS),
        // so fusing them would need a separate TST and buys nothing.
        default: return NzcvKind::None;
    }
}

/// The representation of one flag definition. Carried inline on the `DefFlags`
/// op node.
struct FlagDef {
    FlagRepr repr{FlagRepr::InCpuStateRecord};

    /// Valid when `repr == InPstate`, and additionally usable as a fast-path
    /// annotation when `repr == InCpuStateRecord` (the compare published both).
    /// Must be `None` for `Dead` and `InRflags`.
    NzcvKind nzcv{NzcvKind::None};

    /// Valid when `repr == InCpuStateRecord`: which lazy record is published.
    /// `CarrySource` (plan §2, `m_flags.pending_carry_source`) is not a separate
    /// field because it is a projection of this one — Add32/Sub32/Logic32/...
    /// are all recoverable from the `FlagOp`, so a carry consumer reads the
    /// producing `DefFlags` instead of a parallel mutable variable that has to
    /// be invalidated by hand.
    FlagState::FlagOp record{FlagState::FlagOp::None};

    /// Shift count, 1-31, for the `Shl32`/`Shr32`/`Sar32` records ONLY.
    ///
    /// A field rather than a third operand because the count is a compile-time
    /// constant here: this IR admits only the immediate and by-1 shift forms.
    /// The record packs it as `flag_dst = (count << 32) | result`, so the
    /// backend must have it, and the verifier requires 1-31.
    ///
    /// **Zero is not representable, deliberately.** `shl eax, 0` modifies NO
    /// flags at all — not even ZF — so it must produce no `DefFlags` rather
    /// than one carrying a zero count. A count of 0 that reached here would
    /// publish a record whose materializer silently does nothing, leaving the
    /// PREVIOUS definition's flags claimed by this instruction's record.
    u8 shift_count{0};

    /// A CF/OF overlay (`CLC`/`STC`/`CMC`/`BT`) is live over this definition.
    ///
    /// Recorded landmine, twice: the overlay gate must precede *every*
    /// lazy-flag fast path, because "a live full-flag FlagOp implies no
    /// overlay" is false — CLC/STC/CMC/BT install an overlay over a live
    /// FlagOp by design. Stage 1 has no op that sets this, but the field
    /// exists now so the Stage 3 flag consumers cannot forget to look.
    bool overlay_live{false};

    /// `repr == Dead`, but the CF/OF overlay clear that publishing the record
    /// would have performed is still required — mirrors
    /// `m_omit_lazy_keep_overlay` and the "marked 2, not 1" rule in
    /// `flag_liveness.cpp`. Omitting the record does not retire a stale
    /// CLC/STC, and the consumer reading CF from PSTATE would lose to it.
    bool keep_overlay{false};

    /// `repr == InCpuStateRecord`, and the record must STAY published: a
    /// later INC/DEC's elided record is rebuilt at a fault from the carry
    /// this record leaves in CpuState (JitFlagRecovery::MergeCf). Passes that
    /// would move or drop a record (PSTATE-only demotion, exit deferral)
    /// leave a pinned one alone.
    bool record_pinned{false};

    /// `repr == InCpuStateRecord`, but the record is published only on the
    /// taken edge of the one `CondExit` that reads it (defer_records_to_exits):
    /// on the fall-through it is dead, its killer later in the block and
    /// every fault before that covered by a recovery plan.
    bool exit_deferred{false};
};

// =============================================================================
// Branches
// =============================================================================

/// The edges of a branch. Both are guest addresses, never IR block indices:
/// this IR still compiles one guest basic block at a time, so a successor is
/// named the way the dispatcher and the block cache name it.
struct BranchInfo {
    /// x86 condition code, 0x0-0xF (the low nibble of a Jcc opcode).
    /// Meaningless for `Jmp`.
    u8 cond{0};
    /// Where the branch goes when taken (or unconditionally, for `Jmp`).
    u64 target{kInvalidRip};
    /// Where execution continues when a `Jcc` is not taken. Unused by `Jmp`.
    u64 fallthrough{kInvalidRip};

    /// This branch is a CALL, and control is expected to come back here.
    ///
    /// The architectural push already happened — it is an ordinary `Store` in
    /// the IR — so this is not semantics being smuggled into the branch. It is
    /// the CONTROL-FLOW fact that the branch is a call, which the backend
    /// needs in order to prime the return-address predictor. Without it every
    /// matching `RET` misses and pays a dispatcher round trip.
    u64 return_site{kInvalidRip};

    /// This indirect branch is a RET, so its target is expected to be the top
    /// of the return-address predictor rather than an arbitrary pointer.
    ///
    /// `RET` and `JMP r/m` are the same instruction to an emitter that only
    /// looks at the target value, and lowering them identically costs a
    /// measured 7.7x on a call/ret loop.
    bool is_return{false};
};

// =============================================================================
// Memory accesses
// =============================================================================

/// What guarantee a memory access carries, i.e. which lowering the backend
/// owes it. This names the REQUIREMENT, not the instruction chosen — the
/// backend picks a form that satisfies it.
///
/// One class today. Stronger classes (direct, validated, watched, code,
/// atomic, slow) arrive with the analyses that can prove them; adding the
/// enumerators before anything can establish them would name guarantees
/// nobody checks.
enum class MemClass : u8 {
    /// A plain guest access with nothing proven about it. The backend must
    /// emit the fully guarded form: bounds check, code-page and write-watch
    /// handling, and a fault path that reconstructs architectural state.
    Guest = 0,
};

[[nodiscard]] const char* mem_class_name(MemClass c) noexcept;

/// The memory metadata carried on a `Load`/`Store` node.
/// Which `Op::Divide` form an op is: the pair index is log2(width / 8), so
/// `kind >> 1` names the width and `kind & 1` the signedness. The order is
/// the encoding the helper switches on; keep them in step.
enum class DivideKind : u8 {
    Div8 = 0, Idiv8 = 1, Div16 = 2, Idiv16 = 3,
    Div32 = 4, Idiv32 = 5, Div64 = 6, Idiv64 = 7,
};

struct MemAccess {
    MemClass klass{MemClass::Guest};

    /// Access width. Redundant with the result type on a `Load`, but a `Store`
    /// defines no value, so the width has to live somewhere both share.
    Type width{Type::I32};

    /// The 16-byte ALIGNED vector forms raise #GP when the address is not a
    /// multiple of 16 (Intel SDM Vol. 2 MOVAPS/MOVAPD/MOVDQA; the reference
    /// checks it with `require_align16`). The unaligned spellings of the very
    /// same copies do not, and that is the ONLY thing separating them -- so it
    /// is a property of the access, not a different operation.
    ///
    /// The check costs two words in the hot stream, a UBFX of the low bits and
    /// a CBNZ to an out-of-line stub; the stub raises. It is never the reason
    /// a form declines.
    bool align16{false};
};

// =============================================================================
// Fault state
// =============================================================================

/// What the fault handler must be able to reconstruct at this op.
///
/// Plan §3: a software raise needs `cpu->rip` already published; a hardware
/// fault recovers RIP from the host-PC side table and pins from `ucontext`.
/// Neither persists host NZCV.
enum class FaultState : u8 {
    /// Cannot fault. Must be paired with `may_fault == false`.
    None = 0,
    /// Guest RIP must be correct at this point; no flag value is live across
    /// it, so nothing has to be said about flags.
    RipOnly,
    /// Guest RIP must be correct *and* every live flag value must be
    /// architecturally visible (`InCpuStateRecord` or `InRflags`). This is the
    /// state a faulting op needs whenever a flag definition spans it — the
    /// invariant `compute_omit_lazy_flags` violates today.
    RipAndFlags,
};

[[nodiscard]] const char* fault_state_name(FaultState s) noexcept;

// =============================================================================
// Op node
// =============================================================================

/// One IR operation. Trivially copyable POD; the block owns a flat vector of
/// these and nothing points at anything.
struct Inst {
    Op op{Op::Const};

    /// Result type, or `Type::Void` for an op that defines no value.
    Type type{Type::Void};

    /// The SSA value this op defines, or `kInvalidValue`.
    ValueId result{kInvalidValue};

    u32 operand_count{0};
    Operand operands[kMaxOperands]{};

    /// The guest instruction that owns this op. **Mandatory** — it is the
    /// host-PC -> guest-RIP side table entry the hardware fault handler reads.
    /// `kInvalidRip` is rejected by the verifier.
    u64 guest_rip{kInvalidRip};

    /// True if this op can raise a guest-visible fault.
    bool may_fault{false};

    /// What must be reconstructible if it does. Must be `None` exactly when
    /// `may_fault` is false.
    FaultState fault_state{FaultState::None};

    /// Meaningful only for `Op::DefFlags`.
    FlagDef flags{};

    /// Meaningful only for `Op::Load` / `Op::Store`.
    MemAccess mem{};

    /// Meaningful only for `Op::Jcc` / `Op::Jmp`.
    BranchInfo branch{};

    /// Meaningful only for the element-wise vector ops. The value is an ARM64
    /// `VSize`: 0 = byte, 1 = halfword, 2 = word, 3 = doubleword.
    u8 vec_elem{2};

    [[nodiscard]] bool defines_value() const noexcept {
        return result != kInvalidValue;
    }
};

// =============================================================================
// Block
// =============================================================================

/// A straight-line IR block: one guest basic block's worth of ops.
///
/// **Allocation strategy.** One `std::vector<Inst>` of trivially-copyable
/// nodes, operands inline, `ValueId` and `InstId` as dense `u32` indices. No
/// pointer graph, no per-node allocation, no interning side tables. This runs
/// inside block compilation on the hot path, so the intended lifecycle is a
/// single long-lived `IrBlock` owned by the compiler, `clear()`ed per block —
/// `clear()` keeps capacity, so the steady state is zero allocations. Index
/// handles also survive the vector reallocating during construction, which
/// pointers into it would not.
/// How to rebuild a flag record the pass elided, expressed in GUEST terms.
///
/// The pass decides an elision but knows nothing about ARM registers, and the
/// backend knows the pin map but not the dataflow that justified the elision.
/// This is the handoff: guest register indices and immediates here, translated
/// to host pins in `ir_emit_block`, published as a `JitFlagRecovery` per
/// covered guest-instruction interval.
///
/// `first_rip`/`last_rip` are inclusive and name the guest instructions where
/// a fault could observe the elided record — that is, those strictly between
/// the definition's own instruction and its killer's. Neither endpoint's own
/// instruction is covered, which is what lets the covered set be exactly the
/// instructions whose faults the pass would otherwise have had to respect.
struct FlagRecoveryPlan {
    /// `FlagState::FlagOp` byte of the elided record. Zero means no plan.
    u8 flag_op{0};
    /// True when the result was discarded (CMP/TEST) and must be recomputed
    /// from both surviving inputs rather than read back from a destination.
    bool recompute_dst{false};
    /// x86 SOURCE operand: a guest register index, or an immediate.
    bool src_is_imm{false};
    u8 src_reg{0xFF};
    u64 src_imm{0};
    /// `recompute_dst` false: where the RESULT lives (a guest register).
    /// `recompute_dst` true: the FIRST x86 operand.
    bool dst_is_imm{false};
    u8 dst_reg{0xFF};
    u64 dst_imm{0};
    /// Guest instruction owning the elided definition. Carried so the
    /// verifier can match a plan to the definition it justifies, rather than
    /// accepting a Dead definition because SOME plan exists somewhere in the
    /// block — which would make the check vacuous on any block with two.
    u64 def_rip{kInvalidRip};
    /// Inclusive guest-instruction range this plan covers.
    u64 first_rip{kInvalidRip};
    u64 last_rip{kInvalidRip};
    /// Operand width of the record: 64 for the REX.W records, else 32 (a
    /// 32-bit operation in a long-mode block reads bits 31:0 of its pins).
    u8 width{32};
    /// An INC/DEC carry-merge record (JitFlagRecovery::MergeCf): `dst_reg`
    /// holds the RESULT, and the inherited CF is still CpuState's at every
    /// covered instruction (merge_carry_source_live proves it).
    bool merge_cf{false};

    [[nodiscard]] bool active() const noexcept { return flag_op != 0; }

    /// True when this plan justifies eliding `owner_rip`'s record against a
    /// fault at `fault_rip`.
    [[nodiscard]] bool covers(u64 owner_rip, u64 fault_rip) const noexcept {
        return active() && def_rip == owner_rip && fault_rip >= first_rip &&
               fault_rip <= last_rip;
    }
};

struct IrBlock {
    /// Guest RIP of the block entry. Diagnostic/printing only.
    u64 entry_rip{kInvalidRip};

    /// 32-bit guest blocks are Stage 1's scope; the flag is here so the
    /// verifier can reject guest GPR indices 8-15 in a 32-bit block and the
    /// printer can name registers correctly.
    bool is_64bit{false};

    std::vector<Inst> insts;

    /// Next SSA value number to hand out. Values are numbered independently of
    /// instruction indices so a malformed block (two defs of one value) is
    /// *representable* and therefore testable — an SSA invariant that cannot
    /// be violated cannot be verified either, and the verifier is the artifact
    /// under test.
    ValueId next_value{0};

    /// Flag records the demand pass elided ACROSS a may-fault op, each with
    /// the guest-level recipe for rebuilding it at a fault. Empty unless
    /// `Pass::FlagRecovery` ran and found a candidate; at most one per killed
    /// definition, and a block rarely has more than one.
    std::vector<FlagRecoveryPlan> flag_recovery;

    /// Guest GPRs (bit = index) pinned to a host register for the whole
    /// block, so a recovery plan may name them: the fault handler reads the
    /// interrupted register file. A 32-bit block pins all eight; a long-mode
    /// block's pin layout is chosen per block, and its caller says which.
    u16 recovery_gprs{0x00FF};

    void clear() noexcept {
        insts.clear();  // keeps capacity: see the allocation note above
        flag_recovery.clear();
        recovery_gprs = 0x00FF;
        entry_rip = kInvalidRip;
        next_value = 0;
    }

    void reserve(usize n) { insts.reserve(n); }

    [[nodiscard]] usize size() const noexcept { return insts.size(); }
    [[nodiscard]] bool empty() const noexcept { return insts.empty(); }

    /// The type a value was defined with, or `Type::Void` if never defined.
    /// Linear scan: blocks are tens of ops and this is verifier/printer-only,
    /// never on the emit path.
    [[nodiscard]] Type type_of(ValueId v) const noexcept;
};

/// Index of an op within `IrBlock::insts`.
using InstId = u32;
inline constexpr InstId kInvalidInst = 0xFFFF'FFFFu;

// =============================================================================
// Builder
// =============================================================================

/// Convenience construction over an `IrBlock`.
///
/// The builder holds a *reference*, not a copy, and appends. It owns no
/// storage of its own, so building into a compiler-owned reused block costs
/// nothing. It performs no validation: constructing a malformed block must
/// stay possible, because `verify()` is what the tests exercise.
///
/// `set_rip()` opens a guest instruction's interval. Every op emitted after it
/// belongs to that guest instruction, which is how plan §3.1's "may reorder
/// freely within one guest instruction's interval, never across a boundary"
/// stays checkable: the verifier only has to confirm the runs are contiguous
/// and ascending.
class IrBuilder {
public:
    explicit IrBuilder(IrBlock& block) noexcept : m_block(block) {}

    /// Open the interval owned by guest instruction at `rip`.
    void set_rip(u64 rip) noexcept { m_rip = rip; }
    [[nodiscard]] u64 rip() const noexcept { return m_rip; }

    ValueId gpr_read(GuestReg reg);
    void    gpr_write(GuestReg reg, ValueId value);
    ValueId constant(Type type, u64 bits);

    /// Read/write an architectural XMM register. `index` is 0-15; the
    /// verifier rejects 8-15 in a 32-bit block.
    /// Base address of `segment`; long-mode FS/GS require the full I64 base.
    ValueId seg_base(u8 segment, Type width = Type::I32);

    ValueId xmm_read(u8 index);
    void    xmm_write(u8 index, ValueId value);

    /// `MOVSS xmm, xmm`: bits 31:0 from `low32`, bits 127:32 from `dst`.
    ValueId v_insert_low32(ValueId dst, ValueId low32);
    /// `MOVSS xmm, m32`: bits 31:0 from `low32`, bits 127:32 zero.
    ValueId v_zext32(ValueId low32);
    /// `MOVSS m32, xmm`: bits 31:0 of `src`.
    ValueId v_low32(ValueId src);

    ValueId binary(Op op, Type type, ValueId lhs, ValueId rhs);
    ValueId unary(Op op, Type type, ValueId src);

    /// Shift by a literal count.
    /// Narrow `src` to `to`, keeping the low bits.
    [[nodiscard]] ValueId trunc(Type to, ValueId src);
    /// High 64 bits of the signed 64x64 product of `a` and `b`.
    [[nodiscard]] ValueId smulh(ValueId a, ValueId b);
    /// `cond ? a : b`. Both arms are evaluated.
    [[nodiscard]] ValueId select(Type type, ValueId cond, ValueId a, ValueId b);
    /// Evaluate an x86 condition against `flags`, yielding an I8 0 or 1.
    [[nodiscard]] ValueId eval_cc(ValueId flags, u8 cond);
    /// `AddCarry` / `SubBorrow`: `a +/- b +/- cf`, `cf` an I8 0 or 1.
    [[nodiscard]] ValueId carry_binary(Op op, Type type, ValueId a, ValueId b,
                                       ValueId cf);
    /// An element-wise shift by a compile-time count.
    [[nodiscard]] ValueId vec_shift_imm(Op op, u8 elem, ValueId a, u8 count);
    /// A segment selector as a zero-extended I64.
    [[nodiscard]] ValueId seg_selector(u8 seg);
    /// DIV/IDIV at any width. `kind` is `DivideKind`.
    void divide(ValueId divisor, u8 kind, FaultState state);
    /// An element-wise packed integer op. `elem` is an ARM64 `VSize`.
    [[nodiscard]] ValueId vec_binary(Op op, u8 elem, ValueId a, ValueId b);
    [[nodiscard]] ValueId vec_unary(Op op, u8 elem, ValueId a);
    /// The architectural EFLAGS as an I64 value (`PUSHF`). `flags` orders it
    /// after any pending definition.
    [[nodiscard]] ValueId flags_image(ValueId flags);
    /// Install `value` into the architectural EFLAGS (`POPF`).
    void flags_install(ValueId value);
    /// Force CF (`Op::SetCf`) or OF (`Op::SetOf`) from bit 0 of `bit`,
    /// leaving every other flag as the pending record left it.
    void set_flag_overlay(Op op, ValueId bit, Type type);
    /// A shift record whose count is a runtime value, for the by-CL forms.
    /// A masked count of zero modifies no flags, which the LOWERING handles
    /// with a runtime skip -- the IR has no in-instruction control flow.
    ValueId def_flags_shift_reg(const FlagDef& def, ValueId original,
                                ValueId result, ValueId count, Type type);
    ValueId shift_imm(Op op, Type type, ValueId src, u64 count);
    /// Shift by a runtime count. `count` must be an I8 value.
    ValueId shift_reg(Op op, Type type, ValueId src, ValueId count);

    /// `index` may be `kInvalidValue` for base+disp.
    ValueId lea(Type type, ValueId base, ValueId index, u64 scale, i64 disp);

    ValueId zext(Type to, ValueId src);
    ValueId sext(Type to, ValueId src);

    /// The architectural flags at block entry.
    ValueId flags_in();

    /// Define flags from up to two value sources (e.g. src and result of a
    /// SUB). `def` carries the representation.
    ///
    /// `merge_from` is the flag value whose CF this definition INHERITS, and
    /// is required exactly when `def.record` preserves CF (INC/DEC). Passing
    /// it for any other record, or omitting it for those, is rejected by the
    /// verifier.
    ValueId def_flags(const FlagDef& def, ValueId a,
                      ValueId b = kInvalidValue,
                      ValueId merge_from = kInvalidValue);

    /// A 16-byte guest access that must be 16-byte ALIGNED, for the aligned
    /// vector copies. Same fault state as any other access, plus the #GP.
    [[nodiscard]] ValueId load_aligned128(ValueId addr, FaultState state);
    void store_aligned128(ValueId addr, ValueId value, FaultState state);

    /// Read `width` bits of guest memory from `addr`.
    ///
    /// The op is marked `may_fault` with `state` by construction: a load that
    /// forgot to say it can fault is the exact shape the verifier exists to
    /// reject, and making the builder incapable of producing one is cheaper
    /// than catching it later.
    ValueId load(Type width, ValueId addr, FaultState state);

    /// Write `value` (whose type is the access width) to guest memory.
    void store(Type width, ValueId addr, ValueId value, FaultState state);

    /// Conditional branch on `flags`, testing x86 condition code `cond`.
    void jcc(u8 cond, ValueId flags, u64 target, u64 fallthrough);

    /// Mid-block conditional exit (`Op::CondExit`); `flags` must be the
    /// block's live flags value.
    void cond_exit(u8 cond, ValueId flags, u64 target, u64 fallthrough);

    /// Unconditional branch.
    void jmp(u64 target);

    /// Branch to a runtime address held in `target`. `is_return` marks a RET,
    /// whose target the return-address predictor is expected to hold.
    void jmp_indirect(ValueId target, bool is_return = false);

    /// Mark the branch just emitted as a CALL returning to `return_site`.
    void set_call_return_site(u64 return_site) noexcept;

    /// Mark the most recently appended op as potentially faulting.
    ///
    /// Stage 1 has no faulting opcode — every op here is register-only. The
    /// hook exists so Stage 2's memory ops need no new mechanism, and so the
    /// tests can build the exact shape plan §3 forbids
    /// (`CMP eax,imm ; MOV [mem],eax ; ADD eax,1`) without waiting for loads
    /// and stores to be implemented.
    void mark_may_fault(FaultState state) noexcept;

    /// The op appended last, for tests and for passes that need to annotate.
    [[nodiscard]] Inst& last() noexcept { return m_block.insts.back(); }
    [[nodiscard]] InstId last_id() const noexcept {
        return static_cast<InstId>(m_block.insts.size() - 1);
    }

    [[nodiscard]] IrBlock& block() noexcept { return m_block; }

private:
    /// Append `inst` with the current RIP stamped and, if `type` is not
    /// `Void`, a freshly numbered result value.
    ValueId append(Inst inst);

    IrBlock& m_block;
    u64 m_rip{kInvalidRip};
};

// =============================================================================
// Verification and printing
// =============================================================================

/// Check every structural and semantic invariant this IR claims to hold.
///
/// Returns true when the block is well formed. On failure, if `why` is
/// non-null it receives a one-line human-readable reason naming the offending
/// op index — the decline reason `VITREO_JIT_IR_STATS` reports (plan §4).
/// Nothing is allocated on the success path.
///
/// Checked:
///   - SSA: every value defined exactly once, and defined before every use.
///   - Types: operand types match the definition's actual type, and match the
///     per-op rule (no implicit coercion; Zext/Sext strictly widen).
///   - `guest_rip` is present on every op, and the ops form contiguous,
///     ascending runs by RIP (plan §3.1: no motion across a guest instruction
///     boundary).
///   - `may_fault` and `fault_state` agree.
///   - **The flag/fault invariant (plan §3.3):** a flag definition that is
///     live across a `may_fault` op may not be `Dead`, and may not live only
///     `InPstate` — the fault handler does not persist NZCV. A `Dead` flag
///     definition must additionally *have* a killer (a later `DefFlags`),
///     since "dead" means "overwritten before any consumer", not "unused".
[[nodiscard]] bool verify(const IrBlock& block, std::string* why);

/// Render `block` as text. Stable and deterministic: the same block always
/// prints identically, and a copy prints identically to its original. Used by
/// tests and by `VITREO_JIT_IR_DUMP`-style debugging.
[[nodiscard]] std::string print(const IrBlock& block);

/// One op, without the trailing newline. Exposed for targeted assertions.
[[nodiscard]] std::string print_inst(const IrBlock& block, InstId id);

}  // namespace vitreo::cpu::jit::ir
