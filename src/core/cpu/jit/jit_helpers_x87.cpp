// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_x87.cpp
/// @brief Small x87 state helpers called from JIT code.
///
/// Store-value peeks, POP, the transcendental lowerings (jit_fpu_math), host
/// exception harvest and rounding-mode installs, deferred #MF delivery, and
/// FNSAVE/FRSTOR. The generic dispatcher is in jit_helpers_x87_op.cpp.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving; the cut is open only
/// by the two promotions declared in jit_helpers_internal.hpp.

#include "jit_helpers.hpp"
#include "core/math/x87_transcendental.hpp"
#include "core/math/host_fenv.hpp"
#include "../seh/seh32.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace vitreo::cpu::jit {

extern "C" JitFpuStoreResult jit_fpu_peek_m32(CpuState* cpu) {
    // A store narrows, and narrowing obeys RC. See FpuState::store_m32.
    const float stored = cpu->fpu.store_m32(cpu->fpu.read_src(0));
    u32 bits = 0;
    std::memcpy(&bits, &stored, sizeof(bits));
    return {bits, cpu->fpu.memory_store_result_committable() ? 1u : 0u};
}

extern "C" JitFpuStoreResult jit_fpu_peek_m64(CpuState* cpu) {
    const double stored = cpu->fpu.store_m64(cpu->fpu.read_src(0));
    u64 bits = 0;
    std::memcpy(&bits, &stored, sizeof(bits));
    return {bits, cpu->fpu.memory_store_result_committable() ? 1u : 0u};
}

extern "C" JitFpuStoreResult jit_fpu_peek_int(CpuState* cpu, u32 packed) {
    const u32 width = packed & 0xFFu;
    const bool trunc = (packed & 0x100u) != 0;
    const Float80 val = cpu->fpu.read_src(0);
    bool commit = false;
    const u64 bits = cpu->fpu.store_integer(val, width, trunc, &commit);
    return {bits, commit ? 1u : 0u};
}

extern "C" void jit_fpu_pop(CpuState* cpu) {
    (void)cpu->fpu.pop();
}

extern "C" void jit_fpu_math(CpuState* cpu, u32 opcode) {
    // Intel SDM Vol. 2 x87 transcendentals. ARM has no fyl2x/f2xm1; keep the
    // same libm lowering as exec_fpu without rebuilding a DecodedInstruction.
    auto& fpu = cpu->fpu;
    switch (static_cast<Opcode>(opcode)) {
    case Opcode::F2XM1: {
        const double val = fpu.read_src(0).to_double();
        const double result = std::exp2(val) - 1.0;
        fpu.set_st(0, Float80(result));
        fpu.signal_transcendental_precision(
            result, std::exp2l(static_cast<long double>(val)) - 1.0L);
        return;
    }
    case Opcode::FYL2X: {
        const double x = fpu.read_src(0).to_double();
        const double y = fpu.read_src(1).to_double();
        fpu.set_st(1, Float80(y * std::log2(x)));
        (void)fpu.pop();
        return;
    }
    case Opcode::FYL2XP1: {
        const double x = fpu.read_src(0).to_double();
        const double y = fpu.read_src(1).to_double();
        const double result = y * std::log2(x + 1.0);
        fpu.set_st(1, Float80(result));
        (void)fpu.pop();
        fpu.signal_transcendental_precision(
            result, static_cast<long double>(y) *
                        std::log2l(static_cast<long double>(x) + 1.0L));
        return;
    }
    case Opcode::FPTAN: {
        const double val = fpu.read_src(0).to_double();
        if (math::x87_trig_out_of_range(val)) {
            fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
            return;
        }
        fpu.replace_st0_and_push(Float80(math::x87_tan(val)), Float80(1.0));
        fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
        return;
    }
    case Opcode::FPATAN: {
        const double x = fpu.read_src(0).to_double();
        const double y = fpu.read_src(1).to_double();
        fpu.set_st(1, Float80(std::atan2(y, x)));
        (void)fpu.pop();
        return;
    }
    case Opcode::FXTRACT: {
        const double val = fpu.read_src(0).to_double();
        if (val == 0.0) {
            fpu.replace_st0_and_push(
                Float80(-std::numeric_limits<double>::infinity()),
                Float80(val));
            return;
        }
        int exp = 0;
        const double frac = std::frexp(val, &exp);
        fpu.replace_st0_and_push(Float80(static_cast<double>(exp - 1)),
                                 Float80(frac * 2.0));
        return;
    }
    case Opcode::FPREM:
    case Opcode::FPREM1: {
        // Intel SDM Vol. 2: C2=0 when reduction completes; C0/C3/C1 = Q2/Q1/Q0.
        const double a = fpu.read_src(0).to_double();
        const double b = fpu.read_src(1).to_double();
        const bool ieee = static_cast<Opcode>(opcode) == Opcode::FPREM1;
        double r = ieee ? std::remainder(a, b) : std::fmod(a, b);
        // An exactly-zero remainder takes the DIVIDEND's sign, whatever the
        // rounding mode: std::remainder/std::fmod subtract, so RC=round-down
        // turns `1.0 rem 1.0` into -0.0. Confirmed +0 on silicon (Windows,
        // 2026-08-30). Mirrors exec_fpu.
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
        return;
    }
    case Opcode::FSCALE: {
        const double val = fpu.read_src(0).to_double();
        const double scale = std::trunc(fpu.read_src(1).to_double());
        fpu.set_st(0, Float80(std::ldexp(val, static_cast<int>(scale))));
        return;
    }
    case Opcode::FSIN: {
        const double val = fpu.read_src(0).to_double();
        if (math::x87_trig_out_of_range(val)) {
            fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
            return;
        }
        fpu.set_st(0, Float80(math::x87_sin(val)));
        fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
        return;
    }
    case Opcode::FCOS: {
        const double val = fpu.read_src(0).to_double();
        if (math::x87_trig_out_of_range(val)) {
            fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
            return;
        }
        fpu.set_st(0, Float80(math::x87_cos(val)));
        fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
        return;
    }
    case Opcode::FSINCOS: {
        const double val = fpu.read_src(0).to_double();
        if (math::x87_trig_out_of_range(val)) {
            fpu.status_word = static_cast<u16>(fpu.status_word | FPU_SW_C2);
            return;
        }
        fpu.replace_st0_and_push(Float80(math::x87_sin(val)),
                                 Float80(math::x87_cos(val)));
        fpu.status_word = static_cast<u16>(fpu.status_word & ~FPU_SW_C2);
        return;
    }
    default:
        return;
    }
}

extern "C" void jit_fpu_harvest(CpuState* cpu) {
    cpu->fpu.record_exceptions(math::harvest_x87_exceptions());
}

extern "C" void jit_fpu_clear_host(CpuState* cpu) {
    math::clear_host_fp_exceptions();
}

extern "C" void jit_fpu_apply_rounding(CpuState* cpu) {
    math::apply_x87_rounding(cpu->fpu.control_word);
}

extern "C" void jit_fpu_apply_loaded_control_word(CpuState* cpu) {
    cpu->fpu.refresh_exception_summary();
    math::apply_x87_rounding(cpu->fpu.control_word);
}

extern "C" void jit_fpu_deliver_pending(CpuState* cpu) {
    if (cpu == nullptr || cpu->pending_raise.active ||
        (cpu->fpu.status_word & FPU_SW_ES) == 0) {
        return;
    }

    const u16 pending = static_cast<u16>(
        (cpu->fpu.status_word & 0x3Fu) &
        ~(cpu->fpu.control_word & 0x3Fu));
    if (pending == 0) return;
    u32 code = STATUS_FLOAT_INVALID_OPERATION;
    if ((pending & 0x01u) != 0) {
        code = (cpu->fpu.status_word & FPU_SW_SF) != 0
            ? STATUS_FLOAT_STACK_CHECK
            : STATUS_FLOAT_INVALID_OPERATION;
    } else if ((pending & 0x02u) != 0) {
        // Native Windows maps a deferred x87 #D/#MF to INVALID_OPERATION
        // even though FSW retains DE as the pending exception class.
        code = STATUS_FLOAT_INVALID_OPERATION;
    } else if ((pending & 0x04u) != 0) {
        code = STATUS_FLOAT_DIVIDE_BY_ZERO;
    } else if ((pending & 0x08u) != 0) {
        code = STATUS_FLOAT_OVERFLOW;
    } else if ((pending & 0x10u) != 0) {
        code = STATUS_FLOAT_UNDERFLOW;
    } else if ((pending & 0x20u) != 0) {
        code = STATUS_FLOAT_INEXACT_RESULT;
    }

    auto& raise = cpu->pending_raise;
    cpu->fpu.status_word = static_cast<u16>(
        cpu->fpu.status_word | FPU_SW_B);
    raise.active = true;
    raise.from_helper = true;
    raise.code = code;
    raise.flags = 0;
    raise.num_args = code == STATUS_FLOAT_STACK_CHECK ? 1u : 0u;
    if (raise.num_args != 0) raise.args[0] = 0;
}

extern "C" void jit_fnsave(CpuState* cpu, GuestMemory* mem, u64 addr) {
    // Intel SDM Vol. 2 FNSAVE + Vol. 1 Figure 8-9: 32-bit protected-mode
    // image is the 28-byte FNSTENV environment plus eight 80-bit registers
    // in ST(0)..ST(7) stack order, then FINIT. Match exec_fpu, including
    // zeroing the physical ST array (FNINIT itself does not).
    auto& fpu = cpu->fpu;
    const GuestAddr base = static_cast<GuestAddr>(addr);
    // Same as exec_fpu: the status word a guest reads out of the saved
    // environment has to include the exceptions the host unit accumulated
    // while the native lowerings did the arithmetic.
    fpu.record_exceptions(math::harvest_x87_exceptions());
    if (!mem->write_u32(base + 0, fpu.control_word | kFpuEnvReservedHigh).is_ok() ||
        !mem->write_u32(base + 4, fpu.status_word | kFpuEnvReservedHigh).is_ok() ||
        !mem->write_u32(base + 8, fpu.tag_word | kFpuEnvReservedHigh).is_ok() ||
        !mem->write_u32(base + 12, 0).is_ok() ||
        !mem->write_u32(base + 16, 0).is_ok() ||
        !mem->write_u32(base + 20, 0).is_ok() ||
        !mem->write_u32(base + 24, kFpuEnvReservedHigh).is_ok()) {
        return;
    }
    for (u8 i = 0; i < 8; ++i) {
        const Float80 value = fpu.read_src(i);
        const GuestAddr slot = base + 28 + static_cast<GuestAddr>(i) * 10;
        if (!mem->write_u64(slot, value.mantissa).is_ok() ||
            !mem->write_u16(slot + 8, value.exponent_sign).is_ok()) {
            return;
        }
    }
    fpu.control_word = 0x037F;
    fpu.status_word = 0;
    fpu.tag_word = 0xFFFF;
    for (auto& reg : fpu.st) {
        reg = Float80();
    }
    // FNSAVE ends in FINIT: drop the host's flags too, or the next harvest
    // resurrects exactly what was just cleared, and put the host unit back on
    // the reset control word's rounding mode.
    math::clear_host_fp_exceptions();
    math::apply_x87_rounding(fpu.control_word);
}

extern "C" void jit_frstor(CpuState* cpu, GuestMemory* mem, u64 addr) {
    auto& fpu = cpu->fpu;
    const GuestAddr base = static_cast<GuestAddr>(addr);
    const auto cw = mem->read_u16(base + 0);
    const auto sw = mem->read_u16(base + 4);
    const auto tw = mem->read_u16(base + 8);
    if (!cw.is_ok() || !sw.is_ok() || !tw.is_ok()) {
        return;
    }
    fpu.control_word = FpuState::sanitize_control_word(cw.value());
    fpu.status_word = sw.value();
    fpu.tag_word = tw.value();
    // The restored control word selects a rounding mode; install it on the
    // host unit that the native lowerings actually round with.
    math::apply_x87_rounding(fpu.control_word);
    for (u8 i = 0; i < 8; ++i) {
        const GuestAddr slot = base + 28 + static_cast<GuestAddr>(i) * 10;
        const auto mantissa = mem->read_u64(slot);
        const auto exponent = mem->read_u16(slot + 8);
        if (!mantissa.is_ok() || !exponent.is_ok()) {
            return;
        }
        Float80 value;
        value.mantissa = mantissa.value();
        value.exponent_sign = exponent.value();
        fpu.set_st(i, value);
    }
}

} // namespace vitreo::cpu::jit
