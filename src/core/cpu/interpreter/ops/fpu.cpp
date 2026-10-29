// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// x87 FPU instruction execution (CPU-030)

#include "exec_ops.hpp"
#include "core/math/host_fenv.hpp"
#include "core/math/x87_transcendental.hpp"
#include "helpers.hpp"
#include <cfenv>
#include <cmath>
#include <limits>
#include <cstring>

namespace vitreo::cpu::ops {

namespace {

/// Intel SDM Vol. 2 FIST/FISTP/FISTTP and FRNDINT, "FPU Flags Affected":
///
///   * PE (the inexact-result exception) is set when the source is not
///     already an integral value. The JIT's native lowerings pick this up
///     from the host unit for free; the software path has to raise it
///     explicitly, or the two disagree at the next FNSTENV/FNSAVE/FXSAVE
///     harvest (core/math/host_fenv.hpp).
///
///   * C1 (status-word bit 9) reports the ROUNDING DIRECTION: 1 when the
///     delivered result is greater than the exact source value, 0 otherwise.
///     It is cleared, not left alone, when nothing was rounded up.
///
/// Measured on real silicon (the `Windows` host, 2026-08-30):
///
///   fld 1.5;  fistp m32  ->  FSW=0x0220   (C1 set,   PE set)  2 > 1.5
///   fld 1.25; fistp m32  ->  FSW=0x0020   (C1 clear, PE set)  1 < 1.25
///
/// C1 was never modelled, so Vitreo did not match those results.
inline void x87_record_rounding(FpuState& fpu, const Float80& source,
                                double delivered) noexcept {
    constexpr u16 kC1 = 0x0200u;
    const double src = source.to_double();
    fpu.status_word = static_cast<u16>(fpu.status_word & ~kC1);
    if (!std::isfinite(src) || !std::isfinite(delivered)) return;
    if (delivered != src) {
        std::feraiseexcept(FE_INEXACT);
        if (delivered > src) {
            fpu.status_word = static_cast<u16>(fpu.status_word | kC1);
        }
    }
}

/// Read a 32-bit float from guest memory at addr
/// Read an m32 x87 operand and widen it to binary64.
///
/// The widening is done in the integer domain on purpose. Every caller used
/// to take `static_cast<double>` of the float, and on ARM that FCVT replaces
/// a signaling NaN with the host's own default quiet NaN, discarding the
/// payload. x86 quiets the NaN in place and keeps the payload, shifted into
/// the wider significand -- real silicon turns float 7F800001 into
/// 7FF8000020000000 where Vitreo produced a bare 7FF8000000000000. Widening
/// by hand keeps the sign, the payload and the quieting all correct; finite
/// values are exact in binary64 either way.
inline Result<double, Error> read_f32_mem(
    ExecContext& ctx, u64 addr, bool* source_was_denormal = nullptr) noexcept {
    auto r = read_memory(ctx, addr, 4);
    if (!r) return Result<double, Error>::err(r.error());
    const u32 bits = static_cast<u32>(r.value());
    const u32 exponent = (bits >> 23) & 0xFFu;
    const u32 mantissa = bits & 0x007F'FFFFu;
    if (source_was_denormal != nullptr) {
        *source_was_denormal = exponent == 0 && mantissa != 0;
    }
    if (exponent == 0xFFu && mantissa != 0) {
        const u64 wide = (static_cast<u64>(bits >> 31) << 63) |
                         (0x7FFull << 52) |
                         (static_cast<u64>(mantissa) << 29) |
                         (1ull << 51);          // quiet it, as x86 does
        double out;
        std::memcpy(&out, &wide, sizeof(out));
        return Result<double, Error>::ok(out);
    }
    float val;
    std::memcpy(&val, &bits, 4);
    return Result<double, Error>::ok(static_cast<double>(val));
}

/// Read a 64-bit double from guest memory at addr
inline Result<double, Error> read_f64_mem(ExecContext& ctx, u64 addr) noexcept {
    auto r = read_memory(ctx, addr, 8);
    if (!r) return Result<double, Error>::err(r.error());
    u64 bits = r.value();
    double val;
    std::memcpy(&val, &bits, 8);
    return Result<double, Error>::ok(val);
}

/// Write a 32-bit float to guest memory
inline VoidResult write_f32_mem(ExecContext& ctx, u64 addr, float val) noexcept {
    u32 bits;
    std::memcpy(&bits, &val, 4);
    return write_memory(ctx, addr, bits, 4);
}

/// Write a 64-bit double to guest memory
inline VoidResult write_f64_mem(ExecContext& ctx, u64 addr, double val) noexcept {
    u64 bits;
    std::memcpy(&bits, &val, 8);
    return write_memory(ctx, addr, bits, 8);
}

/// Get effective address from memory operand at operand index
inline Result<u64, Error> get_mem_addr(ExecContext& ctx, const DecodedInstruction& instr, usize idx) noexcept {
    return calc_effective_address(ctx, instr.operands[idx].mem);
}

/// Get ST(i) index from imm8 operand at operand index (for register-form FPU instructions)
inline u8 get_sti(const DecodedInstruction& instr, usize idx) noexcept {
    return static_cast<u8>(instr.operands[idx].imm & 7);
}

/// Set FPU condition codes for comparison result
/// ST(0) > operand: C3=0 C2=0 C0=0
/// ST(0) < operand: C3=0 C2=0 C0=1
/// ST(0) == operand: C3=1 C2=0 C0=0
/// Unordered (NaN): C3=1 C2=1 C0=1
} // anonymous namespace


/// Does `op` leave C1 alone?
///
/// Almost nothing does. Intel SDM Vol. 2 spells C1 out per instruction as
/// "set if the result was rounded up / if stack overflow occurred; cleared
/// otherwise" -- so an instruction that neither rounds up nor faults CLEARS
/// it, rather than leaving whatever was there.
///
/// Measured on the `Windows` host 2026-08-30 by seeding FSW=0x0200 and
/// running one instruction: FLD1, FLDZ, FLD m64, FUCOMP, FCOM, FCMOVBE, FXCH,
/// FCHS and FSTP ST(i) all come back with C1 clear. Vitreo only ever SET C1,
/// so a stale bit survived across whole blocks.
///
/// The exceptions are the instructions that READ or LOAD the status word
/// rather than compute: clearing C1 for those would corrupt the value they
/// store, or undo the value they just loaded.
[[nodiscard]] bool preserves_c1(Opcode op) noexcept {
    switch (op) {
    case Opcode::FNSTSW_AX: case Opcode::FNSTSW_M16:
    case Opcode::FNSTCW_M16: case Opcode::FLDCW_M16:
    case Opcode::FNSTENV_M28: case Opcode::FLDENV_M28:
    case Opcode::FNSAVE_M108: case Opcode::FRSTOR_M108:
    case Opcode::FNCLEX: case Opcode::FNOP: case Opcode::FWAIT:
        return true;
    default:
        return false;
    }
}

// Intel SDM Vol. 1 section 8.3.12 and Vol. 3A "Interrupt 16--x87 FPU
// Floating-Point Error (#MF)": only these FN control forms ignore a pending
// unmasked exception. All other x87 instructions, including FNOP, are waiting.
[[nodiscard]] bool is_x87_waiting(Opcode op) noexcept {
    switch (op) {
    case Opcode::FNCLEX: case Opcode::FNINIT:
    case Opcode::FNSTSW_AX: case Opcode::FNSTSW_M16:
    case Opcode::FNSTCW_M16: case Opcode::FNSTENV_M28:
    case Opcode::FNSAVE_M108:
    case Opcode::EMMS: // MMX state transition, not an x87 ESC instruction.
        return false;
    default:
        return true;
    }
}

[[nodiscard]] CpuException pending_x87_exception(const FpuState& fpu) noexcept {
    if ((fpu.status_word & FPU_SW_ES) == 0) return CpuException::None;
    const u16 pending = static_cast<u16>(
        (fpu.status_word & 0x3Fu) & ~(fpu.control_word & 0x3Fu));
    if ((pending & 0x01u) != 0) {
        return (fpu.status_word & FPU_SW_SF) != 0
            ? CpuException::FloatingPointStackCheck
            : CpuException::FloatingPointInvalidOperation;
    }
    // Windows VEH observation (Windows, 2026-09-01): an x87 #D producer
    // leaves FSW=0x8082, but the subsequent waiting #MF is surfaced as
    // STATUS_FLOAT_INVALID_OPERATION (0xC0000090), not the otherwise-defined
    // STATUS_FLOAT_DENORMAL_OPERAND value.
    if ((pending & 0x02u) != 0) return CpuException::FloatingPointInvalidOperation;
    if ((pending & 0x04u) != 0) return CpuException::FloatingPointDivideByZero;
    if ((pending & 0x08u) != 0) return CpuException::FloatingPointOverflow;
    if ((pending & 0x10u) != 0) return CpuException::FloatingPointUnderflow;
    if ((pending & 0x20u) != 0) return CpuException::FloatingPointInexactResult;
    // ES can remain set after an environment/control-word operation masks
    // every recorded flag. In that state no unmasked exception is pending.
    return CpuException::None;
}

ExecResult exec_fpu(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // #MF is a fault on the waiting instruction: check before C1 clearing,
    // memory access, stack mutation, or any other primary operation.
    if (is_x87_waiting(instr.opcode)) {
        const CpuException pending = pending_x87_exception(ctx.cpu.fpu);
        if (pending != CpuException::None) {
            ctx.cpu.fpu.status_word = static_cast<u16>(
                ctx.cpu.fpu.status_word | FPU_SW_B);
            ctx.exception = pending;
            return ExecResult::Exception;
        }
    }
    // Clear C1 up front and let the paths that own it -- rounding, stack
    // faults -- set it back. See preserves_c1 above.
    if (!preserves_c1(instr.opcode)) {
        ctx.cpu.fpu.status_word =
            static_cast<u16>(ctx.cpu.fpu.status_word & ~FPU_SW_C1);
    }
    // x87 arithmetic rounds on the HOST unit, and the host has one rounding
    // mode where x86 has two. Install this class's mode here rather than only
    // when the guest writes FCW: an intervening SSE op installs MXCSR.RC, and
    // without this every x87 result after it would round the SSE way.
    // Memoised (core/math/host_fenv.hpp), so the steady state is a compare.
    math::apply_x87_rounding(ctx.cpu.fpu.control_word);
    auto& fpu = ctx.cpu.fpu;

    switch (instr.opcode) {
        // ====================================================================
        // Control
        // ====================================================================
        case Opcode::FNCLEX: {
            // Clear FPU exception flags (bits 0-5 of status word). The host's
            // accumulated flags go too, or the next harvest would resurrect
            // exactly what was just cleared.
            math::clear_host_fp_exceptions();
            // Clear B, ES, SF and all six exception flags. Condition codes
            // and TOP are preserved (Intel SDM Vol. 2, FCLEX/FNCLEX).
            fpu.status_word &= 0x7F00u;
            return ExecResult::Continue;
        }

        case Opcode::FNINIT: {
            fpu.reset();
            math::clear_host_fp_exceptions();
            math::apply_x87_rounding(fpu.control_word);
            return ExecResult::Continue;
        }

        case Opcode::FNOP: {
            return ExecResult::Continue;
        }

        case Opcode::FWAIT: {
            return ExecResult::Continue;
        }

        case Opcode::FDECSTP: {
            fpu.set_top((fpu.top() - 1) & 7);
            // Clear C1 bit
            fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C1);
            return ExecResult::Continue;
        }

        case Opcode::FINCSTP: {
            fpu.set_top((fpu.top() + 1) & 7);
            // Clear C1 bit
            fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C1);
            return ExecResult::Continue;
        }

        // ====================================================================
        // Load / Store — Floating Point
        // ====================================================================
        case Opcode::FLD_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.push(fpu.prepare_binary32_source(val.value(), denormal));
            return ExecResult::Continue;
        }

        case Opcode::FLD_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.push(fpu.prepare_ieee_source(val.value()));
            return ExecResult::Continue;
        }

        case Opcode::FLD_STI: {
            u8 i = get_sti(instr, 0);
            Float80 val = fpu.read_src(i);
            fpu.push(val);
            return ExecResult::Continue;
        }

        case Opcode::FSTP_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            const float stored = fpu.store_m32(val);
            if (!fpu.memory_store_result_committable()) return ExecResult::Continue;
            auto r = write_f32_mem(ctx, addr.value(), stored);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FST_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            const float stored = fpu.store_m32(val);
            if (!fpu.memory_store_result_committable()) return ExecResult::Continue;
            auto r = write_f32_mem(ctx, addr.value(), stored);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            return ExecResult::Continue;
        }

        case Opcode::FSTP_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            const double stored = fpu.store_m64(val);
            if (!fpu.memory_store_result_committable()) return ExecResult::Continue;
            auto r = write_f64_mem(ctx, addr.value(), stored);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FST_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            const double stored = fpu.store_m64(val);
            if (!fpu.memory_store_result_committable()) return ExecResult::Continue;
            auto r = write_f64_mem(ctx, addr.value(), stored);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            return ExecResult::Continue;
        }

        case Opcode::FLD_M80: {
            // Load 80-bit extended precision (10 bytes: 8-byte mantissa + 2-byte exponent_sign)
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto lo = read_memory(ctx, addr.value(), 8);
            auto hi = read_memory(ctx, addr.value() + 8, 2);
            if (!lo || !hi) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            Float80 val;
            val.mantissa = lo.value();
            val.exponent_sign = static_cast<u16>(hi.value());
            fpu.push(val);
            return ExecResult::Continue;
        }

        case Opcode::FSTP_M80: {
            // Store 80-bit extended precision and pop (10 bytes)
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            Float80 val = fpu.pop_src();
            if (fpu.has_unmasked_exception(FPU_SW_IE | FPU_SW_ZE)) return ExecResult::Continue;
            auto r1 = write_memory(ctx, addr.value(), val.mantissa, 8);
            auto r2 = write_memory(ctx, addr.value() + 8, val.exponent_sign, 2);
            if (!r1 || !r2) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            return ExecResult::Continue;
        }

        case Opcode::FSTP_STI: {
            u8 i = get_sti(instr, 0);
            fpu.set_st(i, fpu.read_src(0));
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FST_STI: {
            const u8 i = get_sti(instr, 0);
            fpu.set_st(i, fpu.read_src(0));
            return ExecResult::Continue;
        }

        // ====================================================================
        // Load / Store — Integer
        // ====================================================================
        case Opcode::FILD_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto raw = read_memory(ctx, addr.value(), 4);
            if (!raw) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            i32 ival = static_cast<i32>(static_cast<u32>(raw.value()));
            fpu.push(Float80::from_i64(ival));
            return ExecResult::Continue;
        }

        case Opcode::FILD_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto raw = read_memory(ctx, addr.value(), 8);
            if (!raw) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            i64 ival = static_cast<i64>(raw.value());
            fpu.push(Float80::from_i64(ival));
            return ExecResult::Continue;
        }

        case Opcode::FILD_M16: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto raw = read_memory(ctx, addr.value(), 2);
            if (!raw) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            i16 ival = static_cast<i16>(static_cast<u16>(raw.value()));
            fpu.push(Float80::from_i64(ival));
            return ExecResult::Continue;
        }

        // FIST/FISTP round per the control word's RC field; only the pop
        // behavior differs between them.
        case Opcode::FIST_M16:
        case Opcode::FISTP_M16:
        case Opcode::FIST_M32:
        case Opcode::FISTP_M32:
        case Opcode::FISTP_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const bool pops = instr.opcode == Opcode::FISTP_M16 ||
                              instr.opcode == Opcode::FISTP_M32 ||
                              instr.opcode == Opcode::FISTP_M64;
            const u32 width =
                (instr.opcode == Opcode::FIST_M16 ||
                 instr.opcode == Opcode::FISTP_M16) ? 2u
                : (instr.opcode == Opcode::FISTP_M64) ? 8u : 4u;
            const Float80 val = fpu.read_src(0);
            bool commit = false;
            const u64 stored = fpu.store_integer(val, width, false, &commit);
            if (!commit) return ExecResult::Continue;
            auto r = write_memory(ctx, addr.value(), stored, width);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            if (pops) fpu.pop();
            return ExecResult::Continue;
        }

        // FISTTP — store integer with truncation and pop (SSE3)
        // Use to_i64_truncate() to avoid the precision loss inherent in
        // converting through an IEEE 754 double (11 mantissa bits dropped).
        case Opcode::FISTTP_M16: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            bool commit = false;
            const u64 stored = fpu.store_integer(val, 2, true, &commit);
            if (!commit) return ExecResult::Continue;
            auto r = write_memory(ctx, addr.value(), stored, 2);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.pop();
            return ExecResult::Continue;
        }
        case Opcode::FISTTP_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            bool commit = false;
            const u64 stored = fpu.store_integer(val, 4, true, &commit);
            if (!commit) return ExecResult::Continue;
            auto r = write_memory(ctx, addr.value(), stored, 4);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.pop();
            return ExecResult::Continue;
        }
        case Opcode::FISTTP_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const Float80 val = fpu.read_src(0);
            bool commit = false;
            const u64 stored = fpu.store_integer(val, 8, true, &commit);
            if (!commit) return ExecResult::Continue;
            auto r = write_memory(ctx, addr.value(), stored, 8);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.pop();
            return ExecResult::Continue;
        }

        // ====================================================================
        // Load Constants
        // ====================================================================
        case Opcode::FLD1: {
            fpu.push(x87_const::fld1());
            return ExecResult::Continue;
        }

        case Opcode::FLDZ: {
            fpu.push(x87_const::fldz());
            return ExecResult::Continue;
        }

        // ====================================================================
        // Arithmetic — Register
        // ====================================================================
        case Opcode::FADD_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Add);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FADDP_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Add);
            fpu.set_st(i, result);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FSUB_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Sub);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FSUBP_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Sub);
            fpu.set_st(i, result);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FMUL_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Mul);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FMULP_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Mul);
            fpu.set_st(i, result);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FDIV_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Div);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FDIVP_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Div);
            fpu.set_st(i, result);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FDIVRP_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Div);
            fpu.set_st(i, result);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FSUBRP_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Sub);
            fpu.set_st(i, result);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FSUBR_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Sub);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FDIVR_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Div);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        // DC register forms: op ST(i), ST(0) → ST(i), no pop
        case Opcode::FADD_TO_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Add);
            fpu.set_st(i, result);
            return ExecResult::Continue;
        }
        case Opcode::FMUL_TO_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Mul);
            fpu.set_st(i, result);
            return ExecResult::Continue;
        }
        case Opcode::FSUB_TO_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Sub);
            fpu.set_st(i, result);
            return ExecResult::Continue;
        }
        case Opcode::FSUBR_TO_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Sub);
            fpu.set_st(i, result);
            return ExecResult::Continue;
        }
        case Opcode::FDIV_TO_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(i), fpu.read_src(0), X87Op::Div);
            fpu.set_st(i, result);
            return ExecResult::Continue;
        }
        case Opcode::FDIVR_TO_STI: {
            u8 i = get_sti(instr, 0);
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.read_src(i), X87Op::Div);
            fpu.set_st(i, result);
            return ExecResult::Continue;
        }

        // ====================================================================
        // Arithmetic — Memory
        // ====================================================================
        case Opcode::FADD_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_binary32_source(val.value(), denormal), X87Op::Add);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FADD_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_ieee_source(val.value()), X87Op::Add);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FMUL_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_binary32_source(val.value(), denormal), X87Op::Mul);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FMUL_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_ieee_source(val.value()), X87Op::Mul);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FSUB_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_binary32_source(val.value(), denormal), X87Op::Sub);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FSUB_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_ieee_source(val.value()), X87Op::Sub);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FDIV_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_binary32_source(val.value(), denormal), X87Op::Div);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FDIV_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // The m32/m64 source widens exactly, so building the Float80 up front
            // loses nothing and keeps the whole operation in one format.
            Float80 result = fpu.arith_x87(fpu.read_src(0), fpu.prepare_ieee_source(val.value()), X87Op::Div);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        // Integer-memory arithmetic/compare (DA m32int / DE m16int).
        // Convert signed integer to double, then apply the ST(0) op.
        // FICOM/FICOMP set x87 condition codes; FICOMP also pops ST(0).
        case Opcode::FIADD_M16:
        case Opcode::FIADD_M32:
        case Opcode::FIMUL_M16:
        case Opcode::FIMUL_M32:
        case Opcode::FISUB_M16:
        case Opcode::FISUB_M32:
        case Opcode::FISUBR_M16:
        case Opcode::FISUBR_M32:
        case Opcode::FIDIV_M16:
        case Opcode::FIDIV_M32:
        case Opcode::FIDIVR_M16:
        case Opcode::FIDIVR_M32:
        case Opcode::FICOM_M16:
        case Opcode::FICOM_M32:
        case Opcode::FICOMP_M16:
        case Opcode::FICOMP_M32: {
            const bool is16 =
                instr.opcode == Opcode::FIADD_M16 ||
                instr.opcode == Opcode::FIMUL_M16 ||
                instr.opcode == Opcode::FISUB_M16 ||
                instr.opcode == Opcode::FISUBR_M16 ||
                instr.opcode == Opcode::FIDIV_M16 ||
                instr.opcode == Opcode::FIDIVR_M16 ||
                instr.opcode == Opcode::FICOM_M16 ||
                instr.opcode == Opcode::FICOMP_M16;
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto raw = read_memory(ctx, addr.value(), is16 ? 2 : 4);
            if (!raw) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const double iv = is16
                ? static_cast<double>(static_cast<i16>(static_cast<u16>(raw.value())))
                : static_cast<double>(static_cast<i32>(static_cast<u32>(raw.value())));
            // The m16/m32 integer widens exactly into the 80-bit format, so
            // routing this family through arith_x87 keeps it on the same
            // single-rounding path as the FADD/FMUL m32/m64 forms rather than
            // evaluating in binary64 and rounding twice.
            const Float80 f_st0 = fpu.read_src(0);
            const Float80 f_iv = Float80(iv);

            // Integer-operand arithmetic obeys precision control exactly like
            // the FADD/FMUL m32/m64 forms do; this family was the one group
            // that never applied it.
            switch (instr.opcode) {
                case Opcode::FIADD_M16:
                case Opcode::FIADD_M32:
                    fpu.set_st(0, fpu.arith_x87(f_st0, f_iv, X87Op::Add));
                    break;
                case Opcode::FIMUL_M16:
                case Opcode::FIMUL_M32:
                    fpu.set_st(0, fpu.arith_x87(f_st0, f_iv, X87Op::Mul));
                    break;
                case Opcode::FISUB_M16:
                case Opcode::FISUB_M32:
                    fpu.set_st(0, fpu.arith_x87(f_st0, f_iv, X87Op::Sub));
                    break;
                case Opcode::FISUBR_M16:
                case Opcode::FISUBR_M32:
                    fpu.set_st(0, fpu.arith_x87(f_iv, f_st0, X87Op::Sub));
                    break;
                case Opcode::FIDIV_M16:
                case Opcode::FIDIV_M32:
                    fpu.set_st(0, fpu.arith_x87(f_st0, f_iv, X87Op::Div));
                    break;
                case Opcode::FIDIVR_M16:
                case Opcode::FIDIVR_M32:
                    fpu.set_st(0, fpu.arith_x87(f_iv, f_st0, X87Op::Div));
                    break;
                case Opcode::FICOM_M16:
                case Opcode::FICOM_M32:
                    (void)fpu.compare(f_st0, f_iv, false);
                    break;
                case Opcode::FICOMP_M16:
                case Opcode::FICOMP_M32:
                    (void)fpu.compare(f_st0, f_iv, false);
                    fpu.pop();
                    break;
                default:
                    break;
            }
            return ExecResult::Continue;
        }

        case Opcode::FDIVR_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            Float80 result = fpu.arith_x87(fpu.prepare_binary32_source(val.value(), denormal), fpu.read_src(0), X87Op::Div);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        case Opcode::FDIVR_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            Float80 result = fpu.arith_x87(fpu.prepare_ieee_source(val.value()), fpu.read_src(0), X87Op::Div);
            fpu.set_st(0, result);
            return ExecResult::Continue;
        }

        // ====================================================================
        // Comparison
        // ====================================================================
        case Opcode::FCOM_STI: {
            u8 i = get_sti(instr, 0);
            (void)fpu.compare(fpu.read_src(0), fpu.read_src(i), false);
            return ExecResult::Continue;
        }

        case Opcode::FCOM_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            (void)fpu.compare(fpu.read_src(0), fpu.prepare_binary32_source(val.value(), denormal), false);
            return ExecResult::Continue;
        }

        case Opcode::FCOM_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            (void)fpu.compare(fpu.read_src(0), fpu.prepare_ieee_source(val.value()), false);
            return ExecResult::Continue;
        }

        case Opcode::FCOMP_STI: {
            u8 i = get_sti(instr, 0);
            (void)fpu.compare(fpu.read_src(0), fpu.read_src(i), false);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FCOMP_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            bool denormal = false;
            auto val = read_f32_mem(ctx, addr.value(), &denormal);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            (void)fpu.compare(fpu.read_src(0), fpu.prepare_binary32_source(val.value(), denormal), false);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FCOMP_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_f64_mem(ctx, addr.value());
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            (void)fpu.compare(fpu.read_src(0), fpu.prepare_ieee_source(val.value()), false);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FCOMPP: {
            (void)fpu.compare(fpu.read_src(0), fpu.read_src(1), false);
            fpu.pop();
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FUCOMPP: {
            (void)fpu.compare(fpu.read_src(0), fpu.read_src(1), true);
            fpu.pop();
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FUCOM_STI: {
            u8 i = get_sti(instr, 0);
            (void)fpu.compare(fpu.read_src(0), fpu.read_src(i), true);
            return ExecResult::Continue;
        }

        case Opcode::FUCOMP_STI: {
            u8 i = get_sti(instr, 0);
            (void)fpu.compare(fpu.read_src(0), fpu.read_src(i), true);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FTST: {
            (void)fpu.compare(fpu.read_src(0), Float80(0.0), false);
            return ExecResult::Continue;
        }

        case Opcode::FCOMI_STI:
        case Opcode::FUCOMI_STI: {
            // Intel SDM Vol. 2 FCOMI/FUCOMI: ZF/PF/CF from the compare;
            // OF, SF, and AF are cleared.
            u8 i = get_sti(instr, 0);
            const Float80 fa = fpu.read_src(0);
            const Float80 fb = fpu.read_src(i);
            const bool quiet = instr.opcode == Opcode::FUCOMI_STI;
            if (!fpu.prepare_compare(fa, fb, quiet)) return ExecResult::Continue;
            const double a = fa.to_double();
            const double b = fb.to_double();
            ctx.cpu.materialize_lazy_flags();
            auto& flags = ctx.cpu.flags;
            if (std::isnan(a) || std::isnan(b)) {
                flags.set_cf(true);
                flags.set_zf(true);
                flags.set_pf(true);
            } else if (a > b) {
                flags.set_cf(false);
                flags.set_zf(false);
                flags.set_pf(false);
            } else if (a < b) {
                flags.set_cf(true);
                flags.set_zf(false);
                flags.set_pf(false);
            } else {
                flags.set_cf(false);
                flags.set_zf(true);
                flags.set_pf(false);
            }
            flags.set_of(false);
            flags.set_sf(false);
            flags.set_af(false);
            return ExecResult::Continue;
        }

        case Opcode::FCMOVB_STI:
        case Opcode::FCMOVE_STI:
        case Opcode::FCMOVBE_STI:
        case Opcode::FCMOVU_STI:
        case Opcode::FCMOVNB_STI:
        case Opcode::FCMOVNE_STI:
        case Opcode::FCMOVNBE_STI:
        case Opcode::FCMOVNU_STI: {
            // ST(0) ← ST(i) if the named EFLAGS condition holds (Intel SDM).
            ctx.cpu.materialize_lazy_flags();
            const bool cf = ctx.cpu.flags.cf();
            const bool zf = ctx.cpu.flags.zf();
            const bool pf = ctx.cpu.flags.pf();
            bool take = false;
            switch (instr.opcode) {
                case Opcode::FCMOVB_STI:   take = cf; break;
                case Opcode::FCMOVE_STI:   take = zf; break;
                case Opcode::FCMOVBE_STI:  take = cf || zf; break;
                case Opcode::FCMOVU_STI:   take = pf; break;
                case Opcode::FCMOVNB_STI:  take = !cf; break;
                case Opcode::FCMOVNE_STI:  take = !zf; break;
                case Opcode::FCMOVNBE_STI: take = !cf && !zf; break;
                case Opcode::FCMOVNU_STI:  take = !pf; break;
                default: break;
            }
            if (take) {
                u8 i = get_sti(instr, 0);
                fpu.set_st(0, fpu.read_src(i));
            }
            return ExecResult::Continue;
        }

        case Opcode::FCOMIP_STI:
        case Opcode::FUCOMIP_STI: {
            // Intel SDM Vol. 2 FCOMIP/FUCOMIP: same EFLAGS as FCOMI, then pop.
            u8 i = get_sti(instr, 0);
            const Float80 fa = fpu.read_src(0);
            const Float80 fb = fpu.read_src(i);
            const bool quiet = instr.opcode == Opcode::FUCOMIP_STI;
            if (!fpu.prepare_compare(fa, fb, quiet)) return ExecResult::Continue;
            const double a = fa.to_double();
            const double b = fb.to_double();
            ctx.cpu.materialize_lazy_flags();
            auto& flags = ctx.cpu.flags;
            if (std::isnan(a) || std::isnan(b)) {
                flags.set_cf(true);
                flags.set_zf(true);
                flags.set_pf(true);
            } else if (a > b) {
                flags.set_cf(false);
                flags.set_zf(false);
                flags.set_pf(false);
            } else if (a < b) {
                flags.set_cf(true);
                flags.set_zf(false);
                flags.set_pf(false);
            } else {
                flags.set_cf(false);
                flags.set_zf(true);
                flags.set_pf(false);
            }
            flags.set_of(false);
            flags.set_sf(false);
            flags.set_af(false);
            fpu.pop();
            return ExecResult::Continue;
        }

        // ====================================================================
        // Status / Control Word
        // ====================================================================
        case Opcode::FNSTSW_AX: {
            // Deliberately does NOT fold in the host's sticky flags, for two
            // reasons. It is the hot half of the x87 compare idiom
            // (fcom; fnstsw ax; sahf), where only C0-C3 are wanted; and the
            // host accumulates flags from work done outside this guest's
            // arithmetic, so folding them in here reports exceptions that
            // never happened -- JitParityTest.FnstswAxNativeParity catches
            // exactly that, seeding a status word and executing no FP at all.
            // The environment-reading instructions below are where a guest
            // actually asks for exception state, and they are all cold.
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RAX), fpu.status_word);
            return ExecResult::Continue;
        }

        case Opcode::FNSTSW_M16: {
            // Same as FNSTSW_AX: the compare idiom, not an environment read.
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = write_memory(ctx, addr.value(), fpu.status_word, 2);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            return ExecResult::Continue;
        }

        case Opcode::FNSTCW_M16: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto r = write_memory(ctx, addr.value(), fpu.control_word, 2);
            if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            return ExecResult::Continue;
        }

        case Opcode::FLDCW_M16: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_memory(ctx, addr.value(), 2);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.control_word =
                FpuState::sanitize_control_word(static_cast<u16>(val.value()));
            fpu.refresh_exception_summary();
            // Rounding mode is configured once, here, so ordinary arithmetic
            // rounds correctly with no per-operation cost -- on the JIT's
            // native lowerings too, since they run on the same host FPU.
            math::apply_x87_rounding(fpu.control_word);
            return ExecResult::Continue;
        }

        case Opcode::FNSTENV_M28: {
            fpu.record_exceptions(math::harvest_x87_exceptions());
            // Save 28-byte 32-bit-protected-mode FPU environment.
            // Layout: control(16,padded), status(16,padded), tag(16,padded),
            //         FIP(32), FCS|FOP(32), FDP(32), FDS|reserved(32).
            // We don't track FIP/FCS/FDP/FDS, so write zeros there.
            //
            // The 16-bit fields sit in 32-bit slots and real hardware leaves
            // the unused upper halves as ALL ONES, not zero -- measured
            // against Intel silicon, which returns 0xFFFF at offsets 2..3,
            // 6..7, 10..11 and 26..27. Zero-extending is a silently wrong
            // image that any guest reloading it via FLDENV/FRSTOR sees.
            constexpr u32 kRsvdHigh = kFpuEnvReservedHigh;
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            GuestAddr base = addr.value();
            auto w0 = write_memory(ctx, base + 0,  fpu.control_word | kRsvdHigh, 4);
            auto w1 = write_memory(ctx, base + 4,  fpu.status_word  | kRsvdHigh, 4);
            auto w2 = write_memory(ctx, base + 8,  fpu.tag_word     | kRsvdHigh, 4);
            auto w3 = write_memory(ctx, base + 12, 0,                4);
            auto w4 = write_memory(ctx, base + 16, 0,                4);
            auto w5 = write_memory(ctx, base + 20, 0,                4);
            auto w6 = write_memory(ctx, base + 24, kRsvdHigh,        4);
            if (!w0 || !w1 || !w2 || !w3 || !w4 || !w5 || !w6) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            // Per Intel SDM: FNSTENV masks all FPU exceptions in the control
            // word after saving the environment.
            fpu.control_word |= 0x003Fu;
            return ExecResult::Continue;
        }

        case Opcode::FLDENV_M28: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            GuestAddr base = addr.value();
            auto cw = read_memory(ctx, base + 0, 2);
            auto sw = read_memory(ctx, base + 4, 2);
            auto tw = read_memory(ctx, base + 8, 2);
            if (!cw || !sw || !tw) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            fpu.control_word =
                FpuState::sanitize_control_word(static_cast<u16>(cw.value()));
            fpu.status_word  = static_cast<u16>(sw.value());
            fpu.tag_word     = static_cast<u16>(tw.value());
            // The restored control word carries a rounding mode.
            math::apply_x87_rounding(fpu.control_word);
            return ExecResult::Continue;
        }

        case Opcode::FNSAVE_M108: {
            fpu.record_exceptions(math::harvest_x87_exceptions());
            // 32-bit protected-mode FPU state image: the same 28-byte
            // environment FNSTENV writes, followed by the eight 80-bit data
            // registers in ST(0)..ST(7) stack order.
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            GuestAddr base = addr.value();

            // Same environment image as FNSTENV, including the all-ones
            // reserved upper halves real hardware writes.
            constexpr u32 kRsvdHigh = kFpuEnvReservedHigh;
            bool ok = write_memory(ctx, base + 0,  fpu.control_word | kRsvdHigh, 4) &&
                      write_memory(ctx, base + 4,  fpu.status_word  | kRsvdHigh, 4) &&
                      write_memory(ctx, base + 8,  fpu.tag_word     | kRsvdHigh, 4) &&
                      write_memory(ctx, base + 12, 0,                4) &&
                      write_memory(ctx, base + 16, 0,                4) &&
                      write_memory(ctx, base + 20, 0,                4) &&
                      write_memory(ctx, base + 24, kRsvdHigh,        4);
            for (u8 i = 0; ok && i < 8; ++i) {
                const Float80 value = fpu.read_src(i);
                const GuestAddr slot = base + 28 + i * 10;
                ok = write_memory(ctx, slot, value.mantissa, 8) &&
                     write_memory(ctx, slot + 8, value.exponent_sign, 2);
            }
            if (!ok) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }

            // Per Intel SDM, FNSAVE reinitializes the FPU after saving —
            // equivalent to FINIT. Guests save the FPU, use the unit, then
            // restore, so that reinitialization is part of the instruction.
            fpu.control_word = 0x037F;
            fpu.status_word = 0;
            fpu.tag_word = 0xFFFF;
            for (auto& reg : fpu.st) reg = Float80();
            return ExecResult::Continue;
        }

        case Opcode::FRSTOR_M108: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            GuestAddr base = addr.value();

            auto cw = read_memory(ctx, base + 0, 2);
            auto sw = read_memory(ctx, base + 4, 2);
            auto tw = read_memory(ctx, base + 8, 2);
            if (!cw || !sw || !tw) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            // The status word carries TOP, so it must be installed before the
            // data registers are written through the stack-relative set_st().
            fpu.control_word =
                FpuState::sanitize_control_word(static_cast<u16>(cw.value()));
            fpu.status_word  = static_cast<u16>(sw.value());
            fpu.tag_word     = static_cast<u16>(tw.value());

            for (u8 i = 0; i < 8; ++i) {
                const GuestAddr slot = base + 28 + i * 10;
                auto mantissa = read_memory(ctx, slot, 8);
                auto exponent = read_memory(ctx, slot + 8, 2);
                if (!mantissa || !exponent) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                Float80 value;
                value.mantissa = mantissa.value();
                value.exponent_sign = static_cast<u16>(exponent.value());
                fpu.set_st(i, value);
            }
            // The restored control word carries a rounding mode.
            math::apply_x87_rounding(fpu.control_word);
            return ExecResult::Continue;
        }

        // ====================================================================
        // Sign / Misc
        // ====================================================================
        case Opcode::FCHS: {
            Float80 val = fpu.read_src(0);
            val.exponent_sign ^= 0x8000u;
            fpu.set_st(0, val);
            return ExecResult::Continue;
        }

        case Opcode::FABS: {
            Float80 val = fpu.read_src(0);
            val.exponent_sign &= 0x7FFFu;
            fpu.set_st(0, val);
            return ExecResult::Continue;
        }

        case Opcode::FXCH_STI: {
            u8 i = get_sti(instr, 0);
            Float80 tmp = fpu.read_src(0);
            fpu.set_st(0, fpu.read_src(i));
            fpu.set_st(i, tmp);
            return ExecResult::Continue;
        }

        case Opcode::FRNDINT: {
            double val = fpu.read_src(0).to_double();
            // Use rounding mode from control word (bits 10-11)
            // 00=nearest, 01=down, 10=up, 11=truncate
            u16 rc = (fpu.control_word >> 10) & 3;
            double result;
            switch (rc) {
                case 0: result = std::nearbyint(val); break;
                case 1: result = std::floor(val); break;
                case 2: result = std::ceil(val); break;
                case 3: result = std::trunc(val); break;
                default: result = std::nearbyint(val); break;
            }
            // Intel SDM Vol. 2 FRNDINT: the inexact-result exception (PE) is
            // raised when the source is not already an integer. std::nearbyint
            // is specified NOT to raise FE_INEXACT -- that is the whole reason
            // it exists next to std::rint -- so the host flag the harvest
            // reads has to be raised explicitly, or the JIT's native FRNDINT
            // (ARM FRINTX, which does raise it) reports PE and this path does
            // not.
            x87_record_rounding(fpu, fpu.read_src(0), result);
            fpu.set_st(0, Float80(result));
            return ExecResult::Continue;
        }

        case Opcode::FSQRT: {
            double val = fpu.read_src(0).to_double();
            // Intel SDM Vol. 2 FSQRT: a negative operand is an invalid
            // operation and delivers the "real indefinite" QNaN, which has the
            // SIGN BIT SET (0xFFF8000000000000). std::sqrt on ARM returns the
            // host's default QNaN with the sign clear, so the result differed
            // from real x86 in exactly one bit. sqrt(-0) is -0 and is not an
            // invalid operation, hence the strict comparison.
            if (val < 0.0) {
                double indefinite;
                const u64 bits = 0xFFF8'0000'0000'0000ull;
                std::memcpy(&indefinite, &bits, sizeof(indefinite));
                fpu.set_st(0, Float80(indefinite));
                return ExecResult::Continue;
            }
            fpu.set_st(0, fpu.sqrt_x87(fpu.read_src(0)));
            return ExecResult::Continue;
        }

        case Opcode::F2XM1: {
            // ST(0) = 2^ST(0) - 1, defined for |ST(0)| <= 1.
            double val = fpu.read_src(0).to_double();
            const double result = std::exp2(val) - 1.0;
            fpu.set_st(0, Float80(result));
            fpu.signal_transcendental_precision(
                result, std::exp2l(static_cast<long double>(val)) - 1.0L);
            return ExecResult::Continue;
        }

        case Opcode::FYL2X: {
            // ST(1) = ST(1) * log2(ST(0)), then pop — result lands in ST(0).
            // This is how guest CRTs build log() and pow().
            double x = fpu.read_src(0).to_double();
            double y = fpu.read_src(1).to_double();
            fpu.set_st(1, Float80(y * std::log2(x)));
            (void)fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FYL2XP1: {
            // ST(1) = ST(1) * log2(ST(0) + 1), then pop. Used for accuracy
            // when ST(0) is near zero, so compute via log2(x+1) directly.
            double x = fpu.read_src(0).to_double();
            double y = fpu.read_src(1).to_double();
            const double result = y * std::log2(x + 1.0);
            fpu.set_st(1, Float80(result));
            (void)fpu.pop();
            fpu.signal_transcendental_precision(
                result, static_cast<long double>(y) *
                            std::log2l(static_cast<long double>(x) + 1.0L));
            return ExecResult::Continue;
        }

        case Opcode::FPTAN: {
            // ST(0) = tan(ST(0)), then push 1.0 (the x87 quirk that makes the
            // result a ratio the caller can divide).
            double val = fpu.read_src(0).to_double();
            if (math::x87_trig_out_of_range(val)) {
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
                return ExecResult::Continue;
            }
            fpu.replace_st0_and_push(Float80(math::x87_tan(val)),
                                     Float80(1.0));
            fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
            return ExecResult::Continue;
        }

        case Opcode::FPATAN: {
            // ST(1) = atan2(ST(1), ST(0)), then pop.
            double x = fpu.read_src(0).to_double();
            double y = fpu.read_src(1).to_double();
            fpu.set_st(1, Float80(std::atan2(y, x)));
            (void)fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FXTRACT: {
            // ST(0) = unbiased exponent, then push the significand.
            double val = fpu.read_src(0).to_double();
            if (val == 0.0) {
                // x87 yields -inf exponent and preserves the zero significand.
                fpu.replace_st0_and_push(
                    Float80(-std::numeric_limits<double>::infinity()),
                    Float80(val));
                return ExecResult::Continue;
            }
            int exp = 0;
            double frac = std::frexp(val, &exp);
            // frexp gives [0.5,1); x87 wants a significand in [1,2).
            fpu.replace_st0_and_push(Float80(static_cast<double>(exp - 1)),
                                     Float80(frac * 2.0));
            return ExecResult::Continue;
        }

        case Opcode::FPREM:
        case Opcode::FPREM1: {
            // Intel SDM Vol. 2 FPREM/FPREM1: ST(0) ← remainder(ST(0)/ST(1)).
            // C2=0 when reduction completes. C0/C3/C1 = Q2/Q1/Q0 of the
            // quotient (trunc toward 0 for FPREM, nearest-even for FPREM1).
            double a = fpu.read_src(0).to_double();
            double b = fpu.read_src(1).to_double();
            const bool ieee = instr.opcode == Opcode::FPREM1;
            double r = ieee ? std::remainder(a, b) : std::fmod(a, b);
            // IEEE 754 / Intel SDM: when the remainder is exactly zero its
            // SIGN is the sign of the DIVIDEND, independent of the rounding
            // mode. std::remainder/std::fmod compute through a subtraction, so
            // under RC = round-toward-negative-infinity `1.0 rem 1.0` comes
            // back as -0.0 -- the rounding mode leaking into a result that is
            // exact and therefore cannot be rounded at all.
            //
            // Measured on real silicon (the `Windows` host, 2026-08-30):
            // `fld1; fld1; fprem1` returns +0 under RC=down, RC=nearest, and
            // for FPREM as well as FPREM1. Latent until the host rounding mode
            // started being installed for x87 (core/math/host_fenv.hpp).
            if (r == 0.0) {
                r = std::signbit(a) ? -0.0 : 0.0;
            }
            fpu.set_st(0, Float80(r));
            const double q = (b != 0.0) ? (a - r) / b : 0.0;
            const u64 qi = static_cast<u64>(
                static_cast<i64>(ieee ? std::nearbyint(q) : std::trunc(q)));
            u16 sw = static_cast<u16>(
                fpu.status_word &
                ~(FPU_SW_C0 | FPU_SW_C1 | FPU_SW_C2 | FPU_SW_C3));
            if (qi & 1u) sw = static_cast<u16>(sw | FPU_SW_C1);
            if (qi & 2u) sw = static_cast<u16>(sw | FPU_SW_C3);
            if (qi & 4u) sw = static_cast<u16>(sw | FPU_SW_C0);
            fpu.status_word = sw;
            return ExecResult::Continue;
        }

        case Opcode::FSCALE: {
            // ST(0) = ST(0) * 2^trunc(ST(1))
            double val = fpu.read_src(0).to_double();
            double scale = std::trunc(fpu.read_src(1).to_double());
            fpu.set_st(0, Float80(std::ldexp(val, static_cast<int>(scale))));
            return ExecResult::Continue;
        }

        case Opcode::FSIN: {
            double val = fpu.read_src(0).to_double();
            if (math::x87_trig_out_of_range(val)) {
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
                return ExecResult::Continue;
            }
            fpu.set_st(0, Float80(math::x87_sin(val)));
            // Clear C2 (reduction complete)
            fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
            return ExecResult::Continue;
        }

        case Opcode::FCOS: {
            double val = fpu.read_src(0).to_double();
            if (math::x87_trig_out_of_range(val)) {
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
                return ExecResult::Continue;
            }
            fpu.set_st(0, Float80(math::x87_cos(val)));
            fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
            return ExecResult::Continue;
        }

        case Opcode::FSINCOS: {
            // Push sin, then cos: ST(0)=cos, ST(1)=sin
            double val = fpu.read_src(0).to_double();
            if (math::x87_trig_out_of_range(val)) {
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
                return ExecResult::Continue;
            }
            fpu.replace_st0_and_push(Float80(math::x87_sin(val)),
                                     Float80(math::x87_cos(val)));
            fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
            return ExecResult::Continue;
        }

        case Opcode::FLDL2T: {
            fpu.push(x87_const::fldl2t(fpu.control_word));
            return ExecResult::Continue;
        }

        case Opcode::FLDL2E: {
            fpu.push(x87_const::fldl2e(fpu.control_word));
            return ExecResult::Continue;
        }

        case Opcode::FLDPI: {
            fpu.push(x87_const::fldpi(fpu.control_word));
            return ExecResult::Continue;
        }

        case Opcode::FLDLG2: {
            fpu.push(x87_const::fldlg2(fpu.control_word));
            return ExecResult::Continue;
        }

        case Opcode::FLDLN2: {
            fpu.push(x87_const::fldln2(fpu.control_word));
            return ExecResult::Continue;
        }

        case Opcode::FXAM: {
            // Examine ST(0) and set condition codes
            Float80 val = fpu.read_src(0);
            bool sign = (val.exponent_sign & 0x8000u) != 0;
            u16 exp = val.exponent_sign & 0x7FFFu;
            // Set C1 to sign bit
            fpu.status_word = static_cast<u16>(fpu.status_word & ~(FPU_SW_C0 | FPU_SW_C2 | FPU_SW_C3));
            if (sign) {
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C1);
            } else {
                fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C1);
            }
            if (exp == 0 && val.mantissa == 0) {
                // Zero: C3=1 C2=0 C0=0
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C3);
            } else if (exp == 0x7FFF) {
                if (val.mantissa == 0x8000000000000000ULL) {
                    // Infinity: C3=0 C2=1 C0=1
                    fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C0 | FPU_SW_C2);
                } else {
                    // NaN: C3=0 C2=0 C0=1
                    fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C0);
                }
            } else {
                // Normal: C3=0 C2=1 C0=0
                fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
            }
            return ExecResult::Continue;
        }

        case Opcode::FSUBR_M32: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_memory(ctx, addr.value(), 4);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            float f;
            u32 bits = static_cast<u32>(val.value());
            std::memcpy(&f, &bits, 4);
            // FSUBR: mem - ST(0) -> ST(0). Through arith_x87 like every other
            // arithmetic form: computing `(double)f - ST(0).to_double()` here
            // evaluated in binary64 and rounded twice, which the oracle caught
            // as a 1-ULP result at a power-of-two boundary (-1.0 delivered
            // where hardware has the value just below it). These two cases
            // were written out by hand and so were missed when the family was
            // routed through the 80-bit path.
            fpu.set_st(0, fpu.arith_x87(Float80(static_cast<double>(f)),
                                        fpu.read_src(0), X87Op::Sub));
            return ExecResult::Continue;
        }

        case Opcode::FSUBR_M64: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            auto val = read_memory(ctx, addr.value(), 8);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            double d;
            u64 bits = val.value();
            std::memcpy(&d, &bits, 8);
            // FSUBR: mem - ST(0) -> ST(0). See FSUBR_M32 above.
            fpu.set_st(0, fpu.arith_x87(Float80(d), fpu.read_src(0),
                                        X87Op::Sub));
            return ExecResult::Continue;
        }

        case Opcode::FFREE_STI: {
            // Intel SDM Vol. 2 FFREE: tag ST(i) Empty; TOP unchanged.
            const u8 i = get_sti(instr, 0);
            fpu.set_tag(fpu.st_index(i), FpuTag::Empty);
            return ExecResult::Continue;
        }

        case Opcode::FFREEP_STI: {
            // FFREEP (DF C0+i): FFREE ST(i) followed by a pop. Undocumented
            // by Intel, present on every implementation.
            //
            // The pop is the whole difference from FFREE and it is not
            // optional: sharing FFREE's arm would leave TOP one slot high for
            // the remainder of the block, silently renaming every subsequent
            // ST(i) rather than faulting. Order matters too -- ST(i) is
            // resolved against the PRE-pop TOP, so `FFREEP ST(0)` frees the
            // current top and then pops it.
            const u8 i = get_sti(instr, 0);
            fpu.set_tag(fpu.st_index(i), FpuTag::Empty);
            fpu.pop();
            return ExecResult::Continue;
        }

        case Opcode::FBLD_M80: {
            // Packed 18-digit BCD (Intel SDM Vol. 2 FBLD).
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            u8 bcd[10]{};
            auto rd = ctx.mem.read_bytes(addr.value(), bcd, 10);
            if (!rd) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            u64 mag = 0;
            for (int i = 8; i >= 0; --i) {
                mag = mag * 100ull
                    + static_cast<u64>(((bcd[i] >> 4) & 0xF) * 10 + (bcd[i] & 0xF));
            }
            double d = static_cast<double>(mag);
            if (bcd[9] & 0x80) d = -d;
            fpu.push(Float80(d));
            return ExecResult::Continue;
        }

        case Opcode::FBSTP_M80: {
            auto addr = get_mem_addr(ctx, instr, 0);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            const double raw = fpu.read_src(0).to_double();
            const bool neg = raw < 0;
            double magd = neg ? -raw : raw;
            if (magd > 999999999999999999.0) magd = 999999999999999999.0;
            u64 mag = static_cast<u64>(magd + 0.5);
            u8 bcd[10]{};
            for (int i = 0; i < 9; ++i) {
                const u8 lo = static_cast<u8>(mag % 10);
                mag /= 10;
                const u8 hi = static_cast<u8>(mag % 10);
                mag /= 10;
                bcd[i] = static_cast<u8>(lo | (hi << 4));
            }
            if (neg) bcd[9] = 0x80;
            auto wr = ctx.mem.write_bytes(addr.value(), bcd, 10);
            if (!wr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            fpu.pop();
            return ExecResult::Continue;
        }

        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
