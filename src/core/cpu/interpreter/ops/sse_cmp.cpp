// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE comparison instruction execution (CPU-036)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "core/cpu/sse_float_semantics.hpp"
#include "core/math/host_fenv.hpp"
#include <cmath>
#include <cstring>

namespace vitreo::cpu::ops {

// ============================================================================
// SSE register / operand helpers (same as sse_arith.cpp)
// ============================================================================

static XmmReg& get_xmm(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

/// Read scalar f32 from operand (register or memory)
static Result<f32, Error> read_ss(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<f32, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].f32_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<f32, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 4);
    if (!val) {
        ctx.exception = CpuException::PageFault;
        ctx.exception_addr = addr.value();
        return Result<f32, Error>::err(val.error());
    }
    f32 f;
    u32 bits = static_cast<u32>(val.value());
    std::memcpy(&f, &bits, 4);
    return Result<f32, Error>::ok(f);
}

/// Read scalar f64 from operand (register or memory)
static Result<f64, Error> read_sd(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<f64, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].f64_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<f64, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 8);
    if (!val) {
        ctx.exception = CpuException::PageFault;
        ctx.exception_addr = addr.value();
        return Result<f64, Error>::err(val.error());
    }
    f64 d;
    u64 bits = val.value();
    std::memcpy(&d, &bits, 8);
    return Result<f64, Error>::ok(d);
}

/// Read packed 4×f32 from operand into XmmReg
/// A 128-bit MEMORY operand must be 16-byte aligned -- a property of the
/// OPERAND, not of the opcode, so it is one guarded check rather than a
/// decision per form. Measured on silicon:
/// `tools/oracle_probe/sse_align_probe.c` and its recorded output. This is the
/// THIRD file-local copy of this reader (see also `sse_int.cpp` and
/// `sse_arith.cpp`); each needed the rule separately, which is why the
/// differential gate in test_jit_parity_64.cpp exists.
///
/// `aligned_form` is false for VEX, which is unaligned unless the opcode says
/// otherwise; `ctx.vex_origin` carries that across `avx_sse_bridge`.
static Result<XmmReg, Error> read_ps(ExecContext& ctx, const Operand& op,
                                     bool aligned_form = true) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    if (aligned_form && op.type == OperandType::Mem128 &&
        (addr.value() & 0xF) != 0) {
        ctx.exception = CpuException::GeneralProtection;
        return Result<XmmReg, Error>::err(Error::InvalidArgument);
    }
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 16);
    if (!r) {
        ctx.exception = CpuException::PageFault;
        ctx.exception_addr = addr.value();
        return Result<XmmReg, Error>::err(r.error());
    }
    return Result<XmmReg, Error>::ok(tmp);
}

/// Read packed 2×f64 from operand into XmmReg (same bytes, just aliased differently)
static Result<XmmReg, Error> read_pd(ExecContext& ctx, const Operand& op,
                                     bool aligned_form = true) noexcept {
    return read_ps(ctx, op, aligned_form);  // Same 128-bit read
}

// ============================================================================
// CMPPS/CMPPD/CMPSS/CMPSD predicates (Intel SDM Vol. 2 CMPPS, Table 3-1)
// ============================================================================
//
// Legacy SSE encodings use imm8[2:0] (8 predicates). VEX encodings use
// imm8[4:0] (32 predicates): 8-15 add EQ_UQ, NGE_US, NGT_US, FALSE_OQ,
// NEQ_OQ, GE_OS, GT_OS and TRUE_UQ, and 16-31 repeat the relation of 0-15
// with only the quiet/signaling behaviour on QNaN swapped. That behaviour
// is visible only in MXCSR.IE (kSseCmpSignalsOnQnan), so the result mask
// depends on imm8[3:0]. Masking a VEX predicate to three bits turned
// _CMP_GT_OQ (0x1E) into NLE_US: TRUE for a NaN lane instead of FALSE, which
// let NaN lanes through masked AVX kernels.

[[nodiscard]] static u8 cmp_predicate_bits(const ExecContext& ctx,
                                           const DecodedInstruction& instr) noexcept {
    const bool vex = instr.prefixes.vex || ctx.vex_origin;
    return static_cast<u8>(instr.operands[2].imm) & (vex ? 0x1Fu : 0x07u);
}

/// Invalid operation per Intel SDM Vol. 2 CMPPS Table 3-1
/// (kSseCmpSignalsOnQnan, shared with tier 1): every predicate for an SNaN,
/// the signalling ones also for a QNaN. Raised in the host accumulator, where
/// tier 1's FCMGE/FCMGT (signalling) and FCMEQ (quiet) put it
/// (sse_raise_invalid).
template <typename F>
[[nodiscard]] static bool cmp_predicate(F a, F b, u8 pred) noexcept {
    const bool unord = std::isnan(a) || std::isnan(b);
    if (sse_is_snan(a) || sse_is_snan(b) ||
        (unord && kSseCmpSignalsOnQnan[pred & 31])) {
        sse_raise_invalid();
    }
    switch (pred & 15) {
        case 0:  return !unord && (a == b);        // EQ_OQ / EQ_OS
        case 1:  return !unord && (a < b);         // LT_OS / LT_OQ
        case 2:  return !unord && (a <= b);        // LE_OS / LE_OQ
        case 3:  return unord;                     // UNORD_Q / UNORD_S
        case 4:  return unord || (a != b);         // NEQ_UQ / NEQ_US
        case 5:  return unord || !(a < b);         // NLT_US / NLT_UQ
        case 6:  return unord || !(a <= b);        // NLE_US / NLE_UQ
        case 7:  return !unord;                    // ORD_Q / ORD_S
        case 8:  return unord || (a == b);         // EQ_UQ / EQ_US
        case 9:  return unord || (a < b);          // NGE_US / NGE_UQ
        case 10: return unord || (a <= b);         // NGT_US / NGT_UQ
        case 11: return false;                     // FALSE_OQ / FALSE_OS
        case 12: return !unord && (a != b);        // NEQ_OQ / NEQ_OS
        case 13: return !unord && (a >= b);        // GE_OS / GE_OQ
        case 14: return !unord && (a > b);         // GT_OS / GT_OQ
        case 15: return true;                      // TRUE_UQ / TRUE_US
    }
    return false;
}

static bool cmp_predicate_f32(f32 a, f32 b, u8 pred) noexcept {
    return cmp_predicate(a, b, pred);
}

static bool cmp_predicate_f64(f64 a, f64 b, u8 pred) noexcept {
    return cmp_predicate(a, b, pred);
}

// ============================================================================
// Main SSE comparison dispatcher
// ============================================================================

ExecResult exec_sse_cmp(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::CMPPS_XMM_XMMM128_IMM8: case Opcode::CMPSS_XMM_XMMM32_IMM8:
    case Opcode::CMPPD_XMM_XMMM128_IMM8: case Opcode::CMPSD_XMM_XMMM64_IMM8:
        // Claim the host FP environment for SSE before a floating-point
        // compare, as exec_sse_arith does: the invalid operation it signals
        // is credited to whichever class owns the host accumulator
        // (host_fenv.cpp), so after an x87 instruction it would land in the
        // FSW, not MXCSR. The same install puts MXCSR.DAZ in front of the
        // host compare (Intel SDM Vol. 1 10.2.3.4: denormal source operands
        // read as zero). The integer PCMP* forms need neither.
        math::apply_sse_rounding(ctx.cpu.sse.mxcsr);
        break;
    default:
        break;
    }
    const auto& dst_op = instr.operands[0];
    const auto& src_op = instr.operands[1];
    // VEX carries no 16-byte alignment requirement.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;

    switch (instr.opcode) {

    // ========================================================================
    // CMPPS: packed 4×f32 comparison
    // ========================================================================
    case Opcode::CMPPS_XMM_XMMM128_IMM8: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        const u8 pred = cmp_predicate_bits(ctx, instr);
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            bool result = cmp_predicate_f32(dst.f32_[i], src.value().f32_[i], pred);
            dst.u32_[i] = result ? 0xFFFFFFFFu : 0x00000000u;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // CMPSS: scalar f32 comparison (low element only, upper preserved)
    // ========================================================================
    case Opcode::CMPSS_XMM_XMMM32_IMM8: {
        auto src = read_ss(ctx, src_op);
        if (!src) return ExecResult::Exception;
        const u8 pred = cmp_predicate_bits(ctx, instr);
        auto& dst = get_xmm(ctx, dst_op.reg);
        bool result = cmp_predicate_f32(dst.f32_[0], src.value(), pred);
        dst.u32_[0] = result ? 0xFFFFFFFFu : 0x00000000u;
        // Upper elements preserved
        return ExecResult::Continue;
    }

    // ========================================================================
    // CMPPD: packed 2×f64 comparison
    // ========================================================================
    case Opcode::CMPPD_XMM_XMMM128_IMM8: {
        auto src = read_pd(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        const u8 pred = cmp_predicate_bits(ctx, instr);
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 2; ++i) {
            bool result = cmp_predicate_f64(dst.f64_[i], src.value().f64_[i], pred);
            dst.u64_[i] = result ? 0xFFFFFFFFFFFFFFFFull : 0x0000000000000000ull;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // CMPSD: scalar f64 comparison (low qword only, upper preserved)
    // ========================================================================
    case Opcode::CMPSD_XMM_XMMM64_IMM8: {
        auto src = read_sd(ctx, src_op);
        if (!src) return ExecResult::Exception;
        const u8 pred = cmp_predicate_bits(ctx, instr);
        auto& dst = get_xmm(ctx, dst_op.reg);
        bool result = cmp_predicate_f64(dst.f64_[0], src.value(), pred);
        dst.u64_[0] = result ? 0xFFFFFFFFFFFFFFFFull : 0x0000000000000000ull;
        // Upper qword preserved
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPEQB: packed byte equality comparison
    // ========================================================================
    case Opcode::PCMPEQB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            dst.u8_[i] = (dst.u8_[i] == src.value().u8_[i]) ? 0xFF : 0x00;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPEQW: packed word equality comparison
    // ========================================================================
    case Opcode::PCMPEQW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            dst.u16_[i] = (dst.u16_[i] == src.value().u16_[i]) ? 0xFFFF : 0x0000;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPEQD: packed dword equality comparison
    // ========================================================================
    case Opcode::PCMPEQD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            dst.u32_[i] = (dst.u32_[i] == src.value().u32_[i]) ? 0xFFFFFFFFu : 0x00000000u;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPGTB: packed signed byte greater-than comparison
    // ========================================================================
    case Opcode::PCMPGTB_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 16; ++i) {
            dst.u8_[i] = (dst.i8_[i] > src.value().i8_[i]) ? 0xFF : 0x00;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPGTW: packed signed word greater-than comparison
    // ========================================================================
    case Opcode::PCMPGTW_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 8; ++i) {
            dst.u16_[i] = (dst.i16_[i] > src.value().i16_[i]) ? 0xFFFF : 0x0000;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPGTD: packed signed dword greater-than comparison
    // ========================================================================
    case Opcode::PCMPGTD_XMM_XMMM128: {
        auto src = read_ps(ctx, src_op, aligned_form);
        if (!src) return ExecResult::Exception;
        auto& dst = get_xmm(ctx, dst_op.reg);
        for (int i = 0; i < 4; ++i) {
            dst.u32_[i] = (dst.i32_[i] > src.value().i32_[i]) ? 0xFFFFFFFFu : 0x00000000u;
        }
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
