// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE shuffle and unpack instruction execution (CPU-038)
//
// Integration checklist (modifications to existing files):
//
// 1. exec_ops.hpp — add declaration:
//      [[nodiscard]] ExecResult exec_sse_shuffle(ExecContext& ctx,
//          const DecodedInstruction& instr) noexcept;
//
// 2. interpreter.cpp — add cases in execute_instruction switch:
//      case Opcode::SHUFPS_XMM_XMMM128_IMM8: case Opcode::SHUFPD_XMM_XMMM128_IMM8:
//      case Opcode::PSHUFD_XMM_XMMM128_IMM8: case Opcode::PSHUFLW_XMM_XMMM128_IMM8:
//      case Opcode::PSHUFHW_XMM_XMMM128_IMM8:
//      case Opcode::UNPCKLPS_XMM_XMMM128: case Opcode::UNPCKHPS_XMM_XMMM128:
//      case Opcode::UNPCKLPD_XMM_XMMM128: case Opcode::UNPCKHPD_XMM_XMMM128:
//      case Opcode::PUNPCKLBW_XMM_XMMM128: case Opcode::PUNPCKLWD_XMM_XMMM128:
//      case Opcode::PUNPCKLDQ_XMM_XMMM128: case Opcode::PUNPCKLQDQ_XMM_XMMM128:
//      case Opcode::PUNPCKHBW_XMM_XMMM128: case Opcode::PUNPCKHWD_XMM_XMMM128:
//      case Opcode::PUNPCKHDQ_XMM_XMMM128: case Opcode::PUNPCKHQDQ_XMM_XMMM128:
//          return exec_sse_shuffle(ctx, instr);
//
// 3. src/CMakeLists.txt — add to vitreo_cpu_interpreter sources:
//      cpu/interpreter/ops/sse_shuffle.cpp

#include "exec_ops.hpp"
#include "helpers.hpp"
#include <cstring>

namespace vitreo::cpu::ops {

// ============================================================================
// Helpers
// ============================================================================

static XmmReg& get_xmm(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

/// Read packed 128 bits from operand into XmmReg
/// A 128-bit MEMORY operand must be 16-byte aligned -- a property of the
/// OPERAND, not of the opcode. Measured on silicon:
/// `tools/oracle_probe/sse_align_probe.c` and its recorded output. This is the
/// FOURTH file-local copy of this reader (see `sse_int.cpp`, `sse_arith.cpp`
/// and `sse_cmp.cpp`); each SSE file keeps its own, which is why the rule had
/// to be added to each separately and why the differential gate in
/// test_jit_parity_64.cpp exists.
///
/// `aligned_form` is false for VEX, which is unaligned unless the opcode says
/// otherwise; `ctx.vex_origin` carries that across `avx_sse_bridge`.
static Result<XmmReg, Error> read_pack(ExecContext& ctx, const Operand& op,
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
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

// ============================================================================
// Main dispatch
// ============================================================================

ExecResult exec_sse_shuffle(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // VEX carries no 16-byte alignment requirement.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;
    const auto& op0 = instr.operands[0]; // destination XMM
    const auto& op1 = instr.operands[1]; // source XMM/m128

    XmmReg& dst = get_xmm(ctx, op0.reg);

    // Read source operand
    auto src_r = read_pack(ctx, op1, aligned_form);
    if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
    XmmReg src = src_r.value();

    switch (instr.opcode) {

        // ====================================================================
        // SHUFPS: imm8 selects floats
        // result[0]=dst[imm[1:0]], result[1]=dst[imm[3:2]],
        // result[2]=src[imm[5:4]], result[3]=src[imm[7:6]]
        // ====================================================================
        case Opcode::SHUFPS_XMM_XMMM128_IMM8: {
            u8 imm = static_cast<u8>(instr.operands[2].imm);
            XmmReg result{};
            result.f32_[0] = dst.f32_[(imm >> 0) & 3];
            result.f32_[1] = dst.f32_[(imm >> 2) & 3];
            result.f32_[2] = src.f32_[(imm >> 4) & 3];
            result.f32_[3] = src.f32_[(imm >> 6) & 3];
            dst = result;
            break;
        }

        // ====================================================================
        // SHUFPD: imm8 bit 0 selects dst double, bit 1 selects src double
        // ====================================================================
        case Opcode::SHUFPD_XMM_XMMM128_IMM8: {
            u8 imm = static_cast<u8>(instr.operands[2].imm);
            XmmReg result{};
            result.f64_[0] = dst.f64_[(imm >> 0) & 1];
            result.f64_[1] = src.f64_[(imm >> 1) & 1];
            dst = result;
            break;
        }

        // ====================================================================
        // PSHUFD: result[i] = src.u32_[(imm >> (2*i)) & 3] for i=0..3
        // ====================================================================
        case Opcode::PSHUFD_XMM_XMMM128_IMM8: {
            u8 imm = static_cast<u8>(instr.operands[2].imm);
            XmmReg result{};
            result.u32_[0] = src.u32_[(imm >> 0) & 3];
            result.u32_[1] = src.u32_[(imm >> 2) & 3];
            result.u32_[2] = src.u32_[(imm >> 4) & 3];
            result.u32_[3] = src.u32_[(imm >> 6) & 3];
            dst = result;
            break;
        }

        // ====================================================================
        // PSHUFLW: low 4 words shuffled by imm8, high 4 words copied from src
        // ====================================================================
        case Opcode::PSHUFLW_XMM_XMMM128_IMM8: {
            u8 imm = static_cast<u8>(instr.operands[2].imm);
            XmmReg result{};
            result.u16_[0] = src.u16_[(imm >> 0) & 3];
            result.u16_[1] = src.u16_[(imm >> 2) & 3];
            result.u16_[2] = src.u16_[(imm >> 4) & 3];
            result.u16_[3] = src.u16_[(imm >> 6) & 3];
            // High 4 words copied from source
            result.u16_[4] = src.u16_[4];
            result.u16_[5] = src.u16_[5];
            result.u16_[6] = src.u16_[6];
            result.u16_[7] = src.u16_[7];
            dst = result;
            break;
        }

        // ====================================================================
        // PSHUFHW: high 4 words shuffled by imm8, low 4 words copied from src
        // ====================================================================
        case Opcode::PSHUFHW_XMM_XMMM128_IMM8: {
            u8 imm = static_cast<u8>(instr.operands[2].imm);
            XmmReg result{};
            // Low 4 words copied from source
            result.u16_[0] = src.u16_[0];
            result.u16_[1] = src.u16_[1];
            result.u16_[2] = src.u16_[2];
            result.u16_[3] = src.u16_[3];
            // High 4 words shuffled (indices 4..7 selected by imm8 2-bit fields)
            result.u16_[4] = src.u16_[4 + ((imm >> 0) & 3)];
            result.u16_[5] = src.u16_[4 + ((imm >> 2) & 3)];
            result.u16_[6] = src.u16_[4 + ((imm >> 4) & 3)];
            result.u16_[7] = src.u16_[4 + ((imm >> 6) & 3)];
            dst = result;
            break;
        }

        // ====================================================================
        // UNPCKLPS: {dst[0], src[0], dst[1], src[1]}
        // ====================================================================
        case Opcode::UNPCKLPS_XMM_XMMM128: {
            XmmReg result{};
            result.f32_[0] = dst.f32_[0];
            result.f32_[1] = src.f32_[0];
            result.f32_[2] = dst.f32_[1];
            result.f32_[3] = src.f32_[1];
            dst = result;
            break;
        }

        // ====================================================================
        // UNPCKHPS: {dst[2], src[2], dst[3], src[3]}
        // ====================================================================
        case Opcode::UNPCKHPS_XMM_XMMM128: {
            XmmReg result{};
            result.f32_[0] = dst.f32_[2];
            result.f32_[1] = src.f32_[2];
            result.f32_[2] = dst.f32_[3];
            result.f32_[3] = src.f32_[3];
            dst = result;
            break;
        }

        // ====================================================================
        // UNPCKLPD: {dst.f64_[0], src.f64_[0]}
        // ====================================================================
        case Opcode::UNPCKLPD_XMM_XMMM128: {
            XmmReg result{};
            result.f64_[0] = dst.f64_[0];
            result.f64_[1] = src.f64_[0];
            dst = result;
            break;
        }

        // ====================================================================
        // UNPCKHPD: {dst.f64_[1], src.f64_[1]}
        // ====================================================================
        case Opcode::UNPCKHPD_XMM_XMMM128: {
            XmmReg result{};
            result.f64_[0] = dst.f64_[1];
            result.f64_[1] = src.f64_[1];
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKLBW: interleave low 8 bytes
        // result[2i]=dst.u8_[i], result[2i+1]=src.u8_[i] for i=0..7
        // ====================================================================
        case Opcode::PUNPCKLBW_XMM_XMMM128: {
            XmmReg result{};
            for (int i = 0; i < 8; ++i) {
                result.u8_[2 * i]     = dst.u8_[i];
                result.u8_[2 * i + 1] = src.u8_[i];
            }
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKHBW: interleave high 8 bytes
        // result[2i]=dst.u8_[8+i], result[2i+1]=src.u8_[8+i] for i=0..7
        // ====================================================================
        case Opcode::PUNPCKHBW_XMM_XMMM128: {
            XmmReg result{};
            for (int i = 0; i < 8; ++i) {
                result.u8_[2 * i]     = dst.u8_[8 + i];
                result.u8_[2 * i + 1] = src.u8_[8 + i];
            }
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKLWD: interleave low 4 words
        // result[2i]=dst.u16_[i], result[2i+1]=src.u16_[i] for i=0..3
        // ====================================================================
        case Opcode::PUNPCKLWD_XMM_XMMM128: {
            XmmReg result{};
            for (int i = 0; i < 4; ++i) {
                result.u16_[2 * i]     = dst.u16_[i];
                result.u16_[2 * i + 1] = src.u16_[i];
            }
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKHWD: interleave high 4 words
        // result[2i]=dst.u16_[4+i], result[2i+1]=src.u16_[4+i] for i=0..3
        // ====================================================================
        case Opcode::PUNPCKHWD_XMM_XMMM128: {
            XmmReg result{};
            for (int i = 0; i < 4; ++i) {
                result.u16_[2 * i]     = dst.u16_[4 + i];
                result.u16_[2 * i + 1] = src.u16_[4 + i];
            }
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKLDQ: interleave low 2 dwords
        // {dst.u32_[0], src.u32_[0], dst.u32_[1], src.u32_[1]}
        // ====================================================================
        case Opcode::PUNPCKLDQ_XMM_XMMM128: {
            XmmReg result{};
            result.u32_[0] = dst.u32_[0];
            result.u32_[1] = src.u32_[0];
            result.u32_[2] = dst.u32_[1];
            result.u32_[3] = src.u32_[1];
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKHDQ: interleave high 2 dwords
        // {dst.u32_[2], src.u32_[2], dst.u32_[3], src.u32_[3]}
        // ====================================================================
        case Opcode::PUNPCKHDQ_XMM_XMMM128: {
            XmmReg result{};
            result.u32_[0] = dst.u32_[2];
            result.u32_[1] = src.u32_[2];
            result.u32_[2] = dst.u32_[3];
            result.u32_[3] = src.u32_[3];
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKLQDQ: {dst.u64_[0], src.u64_[0]}
        // ====================================================================
        case Opcode::PUNPCKLQDQ_XMM_XMMM128: {
            XmmReg result{};
            result.u64_[0] = dst.u64_[0];
            result.u64_[1] = src.u64_[0];
            dst = result;
            break;
        }

        // ====================================================================
        // PUNPCKHQDQ: {dst.u64_[1], src.u64_[1]}
        // ====================================================================
        case Opcode::PUNPCKHQDQ_XMM_XMMM128: {
            XmmReg result{};
            result.u64_[0] = dst.u64_[1];
            result.u64_[1] = src.u64_[1];
            dst = result;
            break;
        }

        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
