// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "registers.hpp"
#include "flags.hpp"
#include "segments.hpp"
#include "fpu_state.hpp"
#include "sse_state.hpp"
#include "exception_state.hpp"
#include "flag_state.hpp"

namespace vitreo::cpu {

/// CPU execution mode
enum class CpuMode : u8 {
    Real,           // Real mode (16-bit)
    Protected,      // Protected mode (32-bit)
    Long,           // Long mode (64-bit)
};

/// CPU state
struct CpuState {
    GeneralRegs64 gpr;      // General-purpose registers (64-bit)
    SegmentRegs seg;        // Segment registers
    Flags flags;            // RFLAGS
    // ---- Lazy flag evaluation for the JIT (cpu-040-jit-dbt) ----
    //
    // THIS BLOCK'S PLACEMENT AND ORDER ARE LOAD-BEARING, and the reason is
    // measured. Publishing a lazy-flag record is what a block boundary costs:
    // an unsound probe that omits every live-out record measures -32% on
    // sc_blocksize bsz1, and a redundant-store probe on the publication
    // (jit-refactor/FLAG-PUBLICATION.md §3) priced ONE store at ~9.9% of bsz1.
    // The cost is the stores, not the instructions -- so the emitter writes
    // the record in two stores rather than four, and that requires exactly
    // three things this layout provides and the static_asserts below pin:
    //
    //   * `flag_op`, `cf_overlay`, `of_overlay` and a dead pad byte are four
    //     contiguous 4-ALIGNED bytes, so one STR Wd publishes the op and
    //     clears both overlays together.
    //   * `flag_src` and `flag_dst` are adjacent 8-aligned u64s WITHIN STP'S
    //     REACH of the CpuState base. STP scales a signed 7-bit immediate by
    //     8, so the pair must sit below offset 504 -- which is why this block
    //     is here, ahead of fpu/sse/avx, rather than after them where it used
    //     to be at offset 1048 and out of range.
    //   * `flag_src` is also 16-aligned so a 32-bit publisher can STR Q the
    //     pair after packing through a vector scratch (no second GPR).
    //
    // Anything inserted above this block eats that headroom.
    using FlagOp = FlagState::FlagOp;
    /// Dead. First so that the word starts here and `cf_overlay` keeps the
    /// 2-alignment `emit_clear_cf_overlay`'s paired STRH needs. Never read.
    u8 flag_word_pad{0};
    FlagOp flag_op{FlagOp::None};
    /// 0 = CF comes from FlagOp / rflags; 1 = CF forced 0; 2 = CF forced 1.
    /// Partial-flag writers (CLC/STC/CMC/BT/ROR) set this so a pending ALU
    /// FlagOp can still answer ZF/SF/OF/PF without a full materialize.
    u8 cf_overlay{0};
    /// Same encoding as `cf_overlay`, for OF. ROR/ROL count==1 write this.
    u8 of_overlay{0};
    u64 flag_src{0};            // Source operand (b for add/sub)
    u64 flag_dst{0};            // Result value
    /// JIT-only: nonzero when ARM NZCV still matches a fusable flag writer
    /// (Sub/And/Add/IncDec). Jcc-first blocks can then B.cond instead of
    /// calling jit_cond_*. Cleared by materialize_lazy_flags and by any
    /// epilogue that does not persist nzcv_bits.
    ///
    /// Deliberately NOT in the word above: publishing a record would then
    /// clear it as a side effect of the single store, silently disabling the
    /// carried-NZCV fast path at every publication.
    u8 nzcv_kind{0};
    /// ARM NZCV (MRS) saved on a linked-tail miss so execute() can MSR
    /// before the next block. Valid only when nzcv_kind != 0.
    u32 nzcv_bits{0};

    static_assert(sizeof(FlagOp) == 1);

    u64 xcr0;               // Extended control register (XGETBV / XSETBV)

    u64 rip;                // Instruction pointer
    CpuMode mode;           // Execution mode

    FpuState fpu;           // x87 FPU state
    SseState sse;           // SSE state (XMM + MXCSR)
    AvxState avx;           // AVX state (YMM upper halves)

    u32 jit_iters{0};           // TODO: Move to JitContext

    // Type aliases — canonical definitions live in ExceptionState (exception_state.hpp)
    using PendingRaiseException = ExceptionState::PendingRaise;
    using PendingUnwind = ExceptionState::PendingUnwind;

    PendingRaiseException pending_raise;
    PendingUnwind pending_unwind;

    /// True after the thread first crosses NT_TIB.StackLimit. The mapped
    /// emergency reserve remains usable while overflow SEH runs.
    bool stack_overflow_active{false};
    /// Nonzero while dispatching STATUS_STACK_OVERFLOW below StackLimit.
    u32 stack_overflow_dispatch_depth{0};
    /// Bytes kept available between overflow detection and the reserved
    /// stack's lower bound, as configured by SetThreadStackGuarantee.
    u32 stack_guarantee{0};

    /// Set by a thunk handler that transferred control itself (longjmp is the
    /// canonical case). The thunk dispatcher then skips both its simulated RET
    /// and its callee-saved-register restore, because the handler has already
    /// installed the exact rip/rsp/nonvolatile state the guest must resume
    /// with. Cleared by the dispatcher once honored.
    bool thunk_control_transfer{false};
    /// Set by a thunk handler whose documented job is to change the
    /// floating-point environment (_controlfp, _fpreset, ...). The x64 thunk
    /// dispatcher otherwise restores MXCSR's control bits after every host
    /// call, because the x64 ABI makes them callee-saved for ordinary
    /// functions. Cleared by the dispatcher once honored.
    bool thunk_sets_fp_control{false};
    /// Bumped each time host code starts a nested guest callback on this CPU
    /// (run_guest_callback), never restored. A host call that must know
    /// whether guest code ran inside it compares it before and after: the
    /// in-chain API crossing (L3) resumes in translated code only if not.
    u32 guest_callback_seq{0};

    constexpr CpuState() noexcept 
        : gpr{}, seg{}, flags{}, fpu{}, sse{}, avx{},
          xcr0(0x7), // x87 | SSE | AVX (Intel SDM Vol. 1 XCR0)
          rip(0), mode(CpuMode::Long),
          flag_op(FlagOp::None), cf_overlay(0), of_overlay(0),
          nzcv_kind(0), flag_src(0), flag_dst(0), nzcv_bits(0) {}
    
    // Mode checks
    [[nodiscard]] bool is_real_mode() const noexcept { 
        return mode == CpuMode::Real; 
    }
    [[nodiscard]] bool is_protected_mode() const noexcept { 
        return mode == CpuMode::Protected; 
    }
    [[nodiscard]] bool is_long_mode() const noexcept { 
        return mode == CpuMode::Long; 
    }
    [[nodiscard]] bool is_32bit() const noexcept { 
        return mode == CpuMode::Protected; 
    }
    [[nodiscard]] bool is_64bit() const noexcept { 
        return mode == CpuMode::Long; 
    }
    
    /// Reset to initial state
    void reset() noexcept {
        gpr = GeneralRegs64{};
        seg = SegmentRegs{};
        flags = Flags{};
        fpu.reset();
        sse.reset();
        avx.clear();
        xcr0 = 0x7;
        rip = 0;
        mode = CpuMode::Long;
        flag_op = FlagOp::None;
        cf_overlay = 0;
        of_overlay = 0;
        nzcv_kind = 0;
        flag_src = 0;
        flag_dst = 0;
        nzcv_bits = 0;
        stack_overflow_active = false;
        stack_overflow_dispatch_depth = 0;
        stack_guarantee = 0;
    }
    
    /// Initialize for 32-bit protected mode
    void init_protected_mode() noexcept {
        reset();
        mode = CpuMode::Protected;
        seg.setup_flat32();
    }
    
    /// Initialize for 64-bit long mode
    void init_long_mode() noexcept {
        reset();
        mode = CpuMode::Long;
        seg.setup_long_mode();
    }
    
    // Register access helpers (width-aware)
    
    /// Get stack pointer based on current mode
    [[nodiscard]] u64 sp() const noexcept {
        return is_32bit() ? gpr.get32(static_cast<u8>(RegIndex::RSP)) : gpr.rsp;
    }
    
    /// Set stack pointer based on current mode
    void set_sp(u64 value) noexcept {
        if (is_32bit()) {
            gpr.set32(static_cast<u8>(RegIndex::RSP), static_cast<u32>(value));
        } else {
            gpr.rsp = value;
        }
    }
    
    /// Get instruction pointer
    [[nodiscard]] u64 ip() const noexcept {
        return is_32bit() ? static_cast<u32>(rip) : rip;
    }
    
    /// Set instruction pointer
    void set_ip(u64 value) noexcept {
        rip = is_32bit() ? static_cast<u32>(value) : value;
    }
    
    /// Advance instruction pointer
    void advance_ip(usize bytes) noexcept {
        if (is_32bit()) {
            rip = static_cast<u32>(rip + bytes);
        } else {
            rip += bytes;
        }
    }

    /// Evaluate only the pending carry flag before an instruction that
    /// consumes/preserves CF and then overwrites the other arithmetic flags
    /// (ADC, SBB, INC, DEC). The lazy operation remains installed: the caller
    /// must replace it with the new instruction's flag state.
    ///
    /// Common integer producers avoid full ZF/SF/PF/OF/AF materialization.
    /// Uncommon operations fall back to the complete reference path.
    void write_cf_only(bool v) noexcept {
        cf_overlay = v ? 2 : 1;
        flags.set_cf(v);
    }

    void write_of_only(bool v) noexcept {
        of_overlay = v ? 2 : 1;
        flags.set_of(v);
    }

    /// Full-flag writers (and explicit `None` commits) own CF/OF. Clear
    /// partial-flag overlays so a later `try_lazy` does not keep answering
    /// the previous forced carry/overflow.
    void set_flag_op(FlagOp op) noexcept {
        flag_op = op;
        cf_overlay = 0;
        of_overlay = 0;
    }

    [[nodiscard]] bool lazy_cf() noexcept {
        if (cf_overlay != 0) return cf_overlay == 2;
        const FlagOp op = flag_op;

        // A self-extracting decompressor overwhelmingly consumes carry after 32-bit
        // compare/subtract, add, and logic operations. Keep these cases ahead
        // of the general switch so the common path is a couple of predictable
        // branches rather than a jump-table dispatch.
        if (op == FlagOp::Sub32) {
            const u32 src = static_cast<u32>(flag_src);
            const u32 dst = static_cast<u32>(flag_dst);
            return dst + src < src;
        }
        if (op == FlagOp::Add32) {
            const u32 src = static_cast<u32>(flag_src);
            const u32 dst = static_cast<u32>(flag_dst);
            return dst < dst - src;
        }
        if (op == FlagOp::And32 || op == FlagOp::Or32 ||
            op == FlagOp::Xor32)
            return false;

        if (op == FlagOp::None) return flags.cf();
        if (op == FlagOp::Inc8 || op == FlagOp::Inc16 ||
            op == FlagOp::Inc32 || op == FlagOp::Inc64 ||
            op == FlagOp::Dec8 || op == FlagOp::Dec16 ||
            op == FlagOp::Dec32 || op == FlagOp::Dec64)
            return ((flag_src >> 32) & 1u) != 0;

        const u32 src = static_cast<u32>(flag_src);
        const u32 dst = static_cast<u32>(flag_dst);
        bool cf = false;

        switch (op) {
        case FlagOp::Add8: {
            const u8 src8 = static_cast<u8>(src);
            const u8 dst8 = static_cast<u8>(dst);
            const u8 original = static_cast<u8>(dst8 - src8);
            cf = dst8 < original;
            break;
        }
        case FlagOp::Add16: {
            const u16 src16 = static_cast<u16>(src);
            const u16 dst16 = static_cast<u16>(dst);
            const u16 original = static_cast<u16>(dst16 - src16);
            cf = dst16 < original;
            break;
        }
        case FlagOp::Add32: {
            const u32 original = dst - src;
            cf = dst < original;
            break;
        }
        case FlagOp::Add64: {
            const u64 original = flag_dst - flag_src;
            cf = flag_dst < original;
            break;
        }
        case FlagOp::Sub8: {
            const u8 src8 = static_cast<u8>(src);
            const u8 original =
                static_cast<u8>(static_cast<u8>(dst) + src8);
            cf = original < src8;
            break;
        }
        case FlagOp::Sub16: {
            const u16 src16 = static_cast<u16>(src);
            const u16 original =
                static_cast<u16>(static_cast<u16>(dst) + src16);
            cf = original < src16;
            break;
        }
        case FlagOp::Sub32: {
            const u32 original = dst + src;
            cf = original < src;
            break;
        }
        case FlagOp::Sub64: {
            const u64 original = flag_dst + flag_src;
            cf = original < flag_src;
            break;
        }
        case FlagOp::And8:
        case FlagOp::And16:
        case FlagOp::And32:
        case FlagOp::And64:
        case FlagOp::Or8:
        case FlagOp::Or16:
        case FlagOp::Or32:
        case FlagOp::Or64:
        case FlagOp::Xor8:
        case FlagOp::Xor16:
        case FlagOp::Xor32:
        case FlagOp::Xor64:
            cf = false;
            break;
        // These four returned a flat `false` to match what `materialize()`
        // then did. Both were wrong; this is the carry-out the instruction
        // really produces, and it must stay identical to the arm in
        // `flag_state.hpp` or the fast path and the slow path disagree.
        case FlagOp::Shl8: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = count <= 8u &&
                 ((static_cast<u8>(src) >> (8u - count)) & 1u) != 0;
            break;
        }
        case FlagOp::Shl16: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = count <= 16u &&
                 ((static_cast<u16>(src) >> (16u - count)) & 1u) != 0;
            break;
        }
        case FlagOp::Sar8: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = ((static_cast<u8>(src) >> (count <= 8u ? count - 1u : 7u)) & 1u) != 0;
            break;
        }
        case FlagOp::Sar16: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = ((static_cast<u16>(src) >> (count <= 16u ? count - 1u : 15u)) & 1u) != 0;
            break;
        }
        case FlagOp::Shr8: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = count <= 8u &&
                 ((static_cast<u8>(src) >> (count - 1u)) & 1u) != 0;
            break;
        }
        case FlagOp::Shr16: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = count <= 16u &&
                 ((static_cast<u16>(src) >> (count - 1u)) & 1u) != 0;
            break;
        }
        case FlagOp::Shl32: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = ((src >> (32u - count)) & 1u) != 0;
            break;
        }
        case FlagOp::Shr32:
        case FlagOp::Sar32: {
            const u32 count = static_cast<u32>(flag_dst >> 32) & 31u;
            if (count == 0) return flags.cf();
            cf = ((src >> (count - 1u)) & 1u) != 0;
            break;
        }
        case FlagOp::Neg32:
            cf = src != 0;
            break;
        case FlagOp::Neg64:
            cf = flag_src != 0;
            break;
        case FlagOp::Mul32:
            cf = dst != 0;
            break;
        case FlagOp::Imul8: {
            const i64 full = static_cast<i64>(flag_src);
            const i8 low = static_cast<i8>(static_cast<u8>(flag_src));
            cf = full != static_cast<i64>(low);
            break;
        }
        case FlagOp::Imul16: {
            const i64 full = static_cast<i64>(flag_src);
            const i16 low = static_cast<i16>(static_cast<u16>(flag_src));
            cf = full != static_cast<i64>(low);
            break;
        }
        case FlagOp::Imul32: {
            const i64 full = static_cast<i64>(flag_src);
            const i32 low = static_cast<i32>(static_cast<u32>(flag_src));
            cf = full != static_cast<i64>(low);
            break;
        }
        case FlagOp::Imul64: {
            const i64 high = static_cast<i64>(flag_src);
            const i64 low = static_cast<i64>(flag_dst);
            cf = high != (low >> 63);
            break;
        }
        case FlagOp::Sbb8: {
            const u8 src8 = static_cast<u8>(src);
            const u8 carry_in = static_cast<u8>(flag_src >> 32);
            const u8 original = static_cast<u8>(
                static_cast<u8>(dst) + src8 + carry_in);
            cf = static_cast<u16>(original) <
                 static_cast<u16>(src8) + carry_in;
            break;
        }
        case FlagOp::Sbb16: {
            const u16 src16 = static_cast<u16>(src);
            const u16 carry_in = static_cast<u16>(flag_src >> 32);
            const u16 original = static_cast<u16>(
                static_cast<u16>(dst) + src16 + carry_in);
            cf = static_cast<u32>(original) <
                 static_cast<u32>(src16) + carry_in;
            break;
        }
        case FlagOp::Sbb32: {
            const u32 carry_in = static_cast<u32>(flag_src >> 32);
            const u32 original = dst + src + carry_in;
            cf = static_cast<u64>(original) <
                 static_cast<u64>(src) + carry_in;
            break;
        }
        case FlagOp::Adc8: {
            const u8 src8 = static_cast<u8>(src);
            const u8 carry_in = static_cast<u8>(flag_src >> 32);
            const u8 original = static_cast<u8>(
                static_cast<u8>(dst) - src8 - carry_in);
            cf = static_cast<u16>(original) + src8 + carry_in > 0xFF;
            break;
        }
        case FlagOp::Adc16: {
            const u16 src16 = static_cast<u16>(src);
            const u16 carry_in = static_cast<u16>(flag_src >> 32);
            const u16 original = static_cast<u16>(
                static_cast<u16>(dst) - src16 - carry_in);
            cf = static_cast<u32>(original) + src16 + carry_in > 0xFFFF;
            break;
        }
        case FlagOp::Adc32: {
            const u32 carry_in = static_cast<u32>(flag_src >> 32);
            const u32 original = dst - src - carry_in;
            cf = static_cast<u64>(original) + src + carry_in >
                 0xFFFFFFFFULL;
            break;
        }
        case FlagOp::Shl64_1:
            cf = (flag_src >> 63) != 0;
            break;
        case FlagOp::Shr64_1:
        case FlagOp::Sar64_1:
            cf = (flag_src & 1) != 0;
            break;
        default:
            materialize_lazy_flags();
            return flags.cf();
        }

        return cf;
    }

    /// Evaluate x86 condition code `cc` (0x0..0xF) directly from the lazy
    /// state, without materializing. Returns 0/1, or -1 when the recorded op
    /// does not determine that condition and the caller must materialize.
    /// The lazy state is left intact either way.
    ///
    /// When `flag_op == None`, rflags is already authoritative — answer from
    /// it directly so `jit_cond_*` does not call `materialize_lazy_flags`
    /// (even the None early-out) on every settled Jcc/SETcc/CMOVcc.
    [[nodiscard]] int try_lazy_condition(u8 cc) const noexcept {
        const u8 cc4 = static_cast<u8>(cc & 0xFu);
        if (of_overlay != 0) {
            const bool of = of_overlay == 2;
            switch (cc4) {
            case 0x0: return of ? 1 : 0;
            case 0x1: return of ? 0 : 1;
            case 0xC:
            case 0xD:
            case 0xE:
            case 0xF: {
                int sf;
                int zf = 0;
                if (flag_op == FlagOp::None) {
                    sf = ((flags.rflags >> 7) & 1ull) ? 1 : 0;
                    zf = ((flags.rflags >> 6) & 1ull) ? 1 : 0;
                } else {
                    FlagState fs;
                    fs.flag_op = flag_op;
                    fs.flag_src = flag_src;
                    fs.flag_dst = flag_dst;
                    sf = fs.try_condition(0x8);
                    if (sf < 0) return -1;
                    if (cc4 >= 0xE) {
                        zf = fs.try_condition(0x4);
                        if (zf < 0) return -1;
                    }
                }
                const int sx = (sf != 0) != of ? 1 : 0;
                if (cc4 == 0xC) return sx;
                if (cc4 == 0xD) return sx ^ 1;
                if (cc4 == 0xE) return (zf || sx) ? 1 : 0;
                return (!zf && !sx) ? 1 : 0;
            }
            default:
                break;
            }
        }
        if (cf_overlay != 0) {
            const bool cf = cf_overlay == 2;
            switch (cc4) {
            case 0x2: return cf ? 1 : 0;
            case 0x3: return cf ? 0 : 1;
            case 0x6:
            case 0x7: {
                int zf;
                if (flag_op == FlagOp::None) {
                    zf = ((flags.rflags >> 6) & 1ull) ? 1 : 0;
                } else {
                    FlagState fs;
                    fs.flag_op = flag_op;
                    fs.flag_src = flag_src;
                    fs.flag_dst = flag_dst;
                    zf = fs.try_condition(0x4);
                    if (zf < 0) return -1;
                }
                const int v = (cf || zf) ? 1 : 0;
                return (cc4 == 0x6) ? v : (v ^ 1);
            }
            default:
                break;
            }
        }
        if (flag_op == FlagOp::None) {
            const u64 f = flags.rflags;
            const bool cf = (f & 1ull) != 0;
            const bool pf = ((f >> 2) & 1ull) != 0;
            const bool zf = ((f >> 6) & 1ull) != 0;
            const bool sf = ((f >> 7) & 1ull) != 0;
            const bool of = ((f >> 11) & 1ull) != 0;
            switch (cc4) {
            case 0x0: return of ? 1 : 0;
            case 0x1: return of ? 0 : 1;
            case 0x2: return cf ? 1 : 0;
            case 0x3: return cf ? 0 : 1;
            case 0x4: return zf ? 1 : 0;
            case 0x5: return zf ? 0 : 1;
            case 0x6: return (cf || zf) ? 1 : 0;
            case 0x7: return (!cf && !zf) ? 1 : 0;
            case 0x8: return sf ? 1 : 0;
            case 0x9: return sf ? 0 : 1;
            case 0xA: return pf ? 1 : 0;
            case 0xB: return pf ? 0 : 1;
            case 0xC: return (sf != of) ? 1 : 0;
            case 0xD: return (sf == of) ? 1 : 0;
            case 0xE: return (zf || (sf != of)) ? 1 : 0;
            case 0xF: return (!zf && (sf == of)) ? 1 : 0;
            default: return 0;
            }
        }
        FlagState fs;
        fs.flag_op = flag_op;
        fs.flag_src = flag_src;
        fs.flag_dst = flag_dst;
        return fs.try_condition(cc);
    }

    /// Drop a pending lazy FlagOp without computing RFLAGS.
    ///
    /// Only valid when the caller has proven the pending flags are dead --
    /// i.e. the very next instruction redefines every flag and reads none
    /// (see jit::pending_flags_dead_across). Unlike materialize, this leaves
    /// flags.rflags stale, so the next writer must be about to overwrite it.
    /// The CF/OF overlays go too: a full-flag writer redefines both.
    void discard_lazy_flags() noexcept {
        flag_op = FlagOp::None;
        nzcv_kind = 0;
        nzcv_bits = 0;
        cf_overlay = 0;
        of_overlay = 0;
    }

    /// Materialize lazy flags into flags.rflags.
    /// Called when transitioning from JIT back to interpreter so that
    /// flag-reading instructions (Jcc, SETcc, CMOVcc, etc.) see correct
    /// values in flags.rflags.  After this call flag_op == FlagOp::None.
    void materialize_lazy_flags() noexcept {
        FlagOp op = flag_op;
        auto finish = [&]() {
            flag_op = FlagOp::None;
            nzcv_kind = 0;
            nzcv_bits = 0;
            if (cf_overlay != 0) {
                flags.set_cf(cf_overlay == 2);
                cf_overlay = 0;
            }
            if (of_overlay != 0) {
                flags.set_of(of_overlay == 2);
                of_overlay = 0;
            }
        };
        if (op == FlagOp::None) {
            finish();
            return;
        }

        // Fast paths for the ops that dominate materialization in practice.
        // Inline them to skip the FlagState copy and switch in
        // FlagState::materialize.
        if (op == FlagOp::Sub32) {
            const u32 src = static_cast<u32>(flag_src);
            const u32 dst = static_cast<u32>(flag_dst);
            flags.update_flags_sub(dst + src, src, dst);
            finish();
            return;
        }
        if (op == FlagOp::Add32) {
            const u32 src = static_cast<u32>(flag_src);
            const u32 dst = static_cast<u32>(flag_dst);
            flags.update_flags_add(dst - src, src, dst);
            finish();
            return;
        }
        if (op == FlagOp::And32 || op == FlagOp::Or32 ||
            op == FlagOp::Xor32) {
            flags.update_flags_logic(static_cast<u32>(flag_dst));
            finish();
            return;
        }
        // 64-bit logic. Xor64 alone is ~94% of materializations on a
        // SHLD-delegating hash loop, because the JIT leaves the XOR lazy and
        // the delegated SHLD -- a partial-flag writer -- has to settle it.
        if (op == FlagOp::And64 || op == FlagOp::Or64 ||
            op == FlagOp::Xor64) {
            flags.update_flags_logic(flag_dst);
            finish();
            return;
        }
        if (op == FlagOp::Shr32) {
            const u32 original = static_cast<u32>(flag_src);
            const u32 result = static_cast<u32>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt != 0) {
                flags.update_zsp(result);
                flags.set_cf((original >> (cnt - 1u)) & 1u);
                flags.set_of(cnt == 1u ? ((original >> 31) & 1u) != 0
                                       : false);
                flags.set_af(false);
            }
            finish();
            return;
        }
        if (op == FlagOp::Shl32) {
            const u32 original = static_cast<u32>(flag_src);
            const u32 result = static_cast<u32>(flag_dst);
            const u32 cnt = static_cast<u32>(flag_dst >> 32) & 31u;
            if (cnt != 0) {
                flags.update_zsp(result);
                flags.set_cf((original >> (32u - cnt)) & 1u);
                if (cnt == 1u)
                    flags.set_of(((result >> 31) ^ (original >> 31)) != 0);
                else
                    flags.set_of(false);
                flags.set_af(false);
            }
            finish();
            return;
        }
        if (op == FlagOp::Inc32) {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            const u32 src = static_cast<u32>(flag_src);
            const u32 dst = static_cast<u32>(flag_dst);
            flags.update_flags_add(src, static_cast<u32>(1), dst);
            flags.set_cf(cf);
            finish();
            return;
        }
        if (op == FlagOp::Dec32) {
            const bool cf = ((flag_src >> 32) & 1u) != 0;
            const u32 src = static_cast<u32>(flag_src);
            const u32 dst = static_cast<u32>(flag_dst);
            flags.update_flags_sub(src, static_cast<u32>(1), dst);
            flags.set_cf(cf);
            finish();
            return;
        }

        FlagState fs;
        fs.flag_op = op;
        fs.flag_src = flag_src;
        fs.flag_dst = flag_dst;
        fs.materialize(flags);
        finish();
    }
};

} // namespace vitreo::cpu
