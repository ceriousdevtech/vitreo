// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// AVX/AVX2 instruction execution (broadcast, extract, insert, permute, blend, variable-shift)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include <cstring>

namespace vitreo::cpu::ops {

// Helper: read XmmReg from register or m128
static Result<XmmReg, Error> read_xmm_or_m128(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&tmp), 16);
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

// Helper: read u32 from memory
static Result<u32, Error> read_m32(ExecContext& ctx, const Operand& op) noexcept {
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u32, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 4);
    if (!val) return Result<u32, Error>::err(val.error());
    return Result<u32, Error>::ok(static_cast<u32>(val.value()));
}

// Helper: get source value as u8/u16/u32/u64 from reg128 or memory
static Result<u8, Error> read_byte_src(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<u8, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].u8_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u8, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 1);
    if (!val) return Result<u8, Error>::err(val.error());
    return Result<u8, Error>::ok(static_cast<u8>(val.value()));
}

static Result<u16, Error> read_word_src(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<u16, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].u16_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u16, Error>::err(addr.error());
    auto val = read_memory(ctx, addr.value(), 2);
    if (!val) return Result<u16, Error>::err(val.error());
    return Result<u16, Error>::ok(static_cast<u16>(val.value()));
}

static Result<u64, Error> read_qword_src(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<u64, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15].u64_[0]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u64, Error>::err(addr.error());
    auto lo = read_memory(ctx, addr.value(), 4);
    auto hi = read_memory(ctx, addr.value() + 4, 4);
    if (!lo || !hi) return Result<u64, Error>::err(lo ? hi.error() : lo.error());
    return Result<u64, Error>::ok(static_cast<u64>(static_cast<u32>(lo.value())) |
                                  (static_cast<u64>(static_cast<u32>(hi.value())) << 32));
}

// Helper to select a 128-bit lane from src1/src2 per VPERM2x128 control
static XmmReg select_128_lane(ExecContext& ctx, u8 src1_idx, const XmmReg& src2_lo,
                              const XmmReg& src2_hi, u8 ctrl) noexcept {
    if (ctrl & 0x08) return XmmReg{}; // zero lane
    switch (ctrl & 3) {
        case 0: return ctx.cpu.sse.xmm[src1_idx];
        case 1: return ctx.cpu.avx.ymm_high[src1_idx];
        case 2: return src2_lo;
        case 3: return src2_hi;
    }
    return XmmReg{};
}

ExecResult exec_avx_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const auto& dst_op = instr.operands[0];
    const auto& src_op = instr.operands[1];

    switch (instr.opcode) {

    // ========================================================================
    // VBROADCASTSS — memory source (AVX)
    // ========================================================================
    case Opcode::VBROADCASTSS_XMM_M32: {
        auto val = read_m32(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        dst.u32_[0] = dst.u32_[1] = dst.u32_[2] = dst.u32_[3] = val.value();
        ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        return ExecResult::Continue;
    }
    case Opcode::VBROADCASTSS_YMM_M32: {
        auto val = read_m32(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        dst.u32_[0] = dst.u32_[1] = dst.u32_[2] = dst.u32_[3] = val.value();
        hi.u32_[0] = hi.u32_[1] = hi.u32_[2] = hi.u32_[3] = val.value();
        return ExecResult::Continue;
    }

    // ========================================================================
    // VBROADCASTSS — register source (AVX2)
    // ========================================================================
    case Opcode::VBROADCASTSS_XMM_XMM: {
        u32 v = ctx.cpu.sse.xmm[src_op.reg & 15].u32_[0];
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        dst.u32_[0] = dst.u32_[1] = dst.u32_[2] = dst.u32_[3] = v;
        ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        return ExecResult::Continue;
    }
    case Opcode::VBROADCASTSS_YMM_XMM: {
        u32 v = ctx.cpu.sse.xmm[src_op.reg & 15].u32_[0];
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        dst.u32_[0] = dst.u32_[1] = dst.u32_[2] = dst.u32_[3] = v;
        hi.u32_[0] = hi.u32_[1] = hi.u32_[2] = hi.u32_[3] = v;
        return ExecResult::Continue;
    }

    // ========================================================================
    // VBROADCASTSD — memory source (AVX) and register source (AVX2)
    // ========================================================================
    case Opcode::VBROADCASTSD_YMM_M64: {
        auto val = read_qword_src(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        dst.u64_[0] = dst.u64_[1] = val.value();
        hi.u64_[0] = hi.u64_[1] = val.value();
        return ExecResult::Continue;
    }
    case Opcode::VBROADCASTSD_YMM_XMM: {
        u64 v = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[0];
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        dst.u64_[0] = dst.u64_[1] = v;
        hi.u64_[0] = hi.u64_[1] = v;
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPBROADCASTB (AVX2)
    // ========================================================================
    case Opcode::VPBROADCASTB_XMM_XMMM8: {
        auto val = read_byte_src(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        for (int i = 0; i < 16; ++i) dst.u8_[i] = val.value();
        ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPBROADCASTB_YMM_XMMM8: {
        auto val = read_byte_src(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        for (int i = 0; i < 16; ++i) { dst.u8_[i] = val.value(); hi.u8_[i] = val.value(); }
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPBROADCASTW (AVX2)
    // ========================================================================
    case Opcode::VPBROADCASTW_XMM_XMMM16: {
        auto val = read_word_src(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        for (int i = 0; i < 8; ++i) dst.u16_[i] = val.value();
        ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPBROADCASTW_YMM_XMMM16: {
        auto val = read_word_src(ctx, src_op);
        if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        for (int i = 0; i < 8; ++i) { dst.u16_[i] = val.value(); hi.u16_[i] = val.value(); }
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPBROADCASTD (AVX2)
    // ========================================================================
    case Opcode::VPBROADCASTD_XMM_XMMM32: {
        u32 v;
        if (is_register_operand(src_op.type)) {
            v = ctx.cpu.sse.xmm[src_op.reg & 15].u32_[0];
        } else {
            auto val = read_m32(ctx, src_op);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            v = val.value();
        }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        dst.u32_[0] = dst.u32_[1] = dst.u32_[2] = dst.u32_[3] = v;
        ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPBROADCASTD_YMM_XMMM32: {
        u32 v;
        if (is_register_operand(src_op.type)) {
            v = ctx.cpu.sse.xmm[src_op.reg & 15].u32_[0];
        } else {
            auto val = read_m32(ctx, src_op);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            v = val.value();
        }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        dst.u32_[0] = dst.u32_[1] = dst.u32_[2] = dst.u32_[3] = v;
        hi.u32_[0] = hi.u32_[1] = hi.u32_[2] = hi.u32_[3] = v;
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPBROADCASTQ (AVX2)
    // ========================================================================
    case Opcode::VPBROADCASTQ_XMM_XMMM64: {
        u64 v;
        if (is_register_operand(src_op.type)) {
            v = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[0];
        } else {
            auto val = read_qword_src(ctx, src_op);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            v = val.value();
        }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        dst.u64_[0] = dst.u64_[1] = v;
        ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPBROADCASTQ_YMM_XMMM64: {
        u64 v;
        if (is_register_operand(src_op.type)) {
            v = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[0];
        } else {
            auto val = read_qword_src(ctx, src_op);
            if (!val) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            v = val.value();
        }
        auto& dst = ctx.cpu.sse.xmm[dst_op.reg & 15];
        auto& hi = ctx.cpu.avx.ymm_high[dst_op.reg & 15];
        dst.u64_[0] = dst.u64_[1] = v;
        hi.u64_[0] = hi.u64_[1] = v;
        return ExecResult::Continue;
    }

    // ========================================================================
    // VBROADCASTI128 ymm, m128 (AVX2)
    // ========================================================================
    case Opcode::VBROADCASTF128_YMM_M128:
    case Opcode::VBROADCASTI128_YMM_M128: {
        auto src = read_xmm_or_m128(ctx, src_op);
        if (!src) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        ctx.cpu.sse.xmm[dst_op.reg & 15] = src.value();
        ctx.cpu.avx.ymm_high[dst_op.reg & 15] = src.value();
        return ExecResult::Continue;
    }

    // ========================================================================
    // VEXTRACTF128 / VEXTRACTI128 — extract 128-bit lane from YMM
    // ========================================================================
    case Opcode::VEXTRACTF128_XMMM128_YMM_IMM8:
    case Opcode::VEXTRACTI128_XMMM128_YMM_IMM8: {
        // Decoder stores Intel dest (r/m xmm/m128) then src (ModR/M.reg ymm).
        // Intel SDM Vol. 2 VEXTRACTF128: DEST := SRC[127:0] or SRC[255:128].
        u8 imm = static_cast<u8>(instr.operands[2].imm & 1);
        const auto& src_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
        const auto& src_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        const auto& selected = imm ? src_hi : src_lo;
        if (is_register_operand(dst_op.type)) {
            ctx.cpu.sse.xmm[dst_op.reg & 15] = selected;
            ctx.cpu.avx.ymm_high[dst_op.reg & 15].clear();
        } else {
            auto addr = calc_effective_address(ctx, dst_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            if (!ctx.mem.write_bytes(addr.value(), reinterpret_cast<const Byte*>(&selected), 16)) {
                record_write_access_violation(ctx, addr.value(), 16);
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // VINSERTF128 / VINSERTI128 — insert 128-bit into YMM
    // ========================================================================
    case Opcode::VINSERTF128_YMM_YMM_XMMM128_IMM8:
    case Opcode::VINSERTI128_YMM_YMM_XMMM128_IMM8: {
        u8 imm = static_cast<u8>(instr.operands[2].imm & 1);
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg src2{};
        if (is_register_operand(src_op.type)) {
            src2 = ctx.cpu.sse.xmm[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            // A faulting m128 read raises and leaves DEST untouched. The
            // result used to be ignored, so an unmapped source silently
            // inserted zeros and the instruction completed.
            if (!ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&src2), 16)) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        ctx.cpu.sse.xmm[dst_idx] = ctx.cpu.sse.xmm[src1_idx];
        ctx.cpu.avx.ymm_high[dst_idx] = ctx.cpu.avx.ymm_high[src1_idx];
        if (imm) {
            ctx.cpu.avx.ymm_high[dst_idx] = src2;
        } else {
            ctx.cpu.sse.xmm[dst_idx] = src2;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPERM2F128 / VPERM2I128 — permute 128-bit lanes across two YMMs
    // ========================================================================
    case Opcode::VPERM2F128_YMM_YMM_YMMM256_IMM8:
    case Opcode::VPERM2I128_YMM_YMM_YMMM256_IMM8: {
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg src2_lo{}, src2_hi{};
        if (is_register_operand(src_op.type)) {
            src2_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            src2_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&src2_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&src2_hi), 16);
        }
        XmmReg result_lo = select_128_lane(ctx, src1_idx, src2_lo, src2_hi, imm & 0x0F);
        XmmReg result_hi = select_128_lane(ctx, src1_idx, src2_lo, src2_hi, (imm >> 4) & 0x0F);
        ctx.cpu.sse.xmm[dst_idx] = result_lo;
        ctx.cpu.avx.ymm_high[dst_idx] = result_hi;
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPERMD ymm, ymm, ymm/m256 — permute dwords using indices (AVX2)
    // ========================================================================
    case Opcode::VPERMD_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 idx_reg = instr.prefixes.vex_vvvv & 15;
        // Read indices from vvvv (8 dwords across lo+hi)
        u32 indices[8];
        for (int i = 0; i < 4; ++i) indices[i] = ctx.cpu.sse.xmm[idx_reg].u32_[i];
        for (int i = 0; i < 4; ++i) indices[4+i] = ctx.cpu.avx.ymm_high[idx_reg].u32_[i];
        // Read src (8 dwords)
        u32 src[8];
        if (is_register_operand(src_op.type)) {
            for (int i = 0; i < 4; ++i) src[i] = ctx.cpu.sse.xmm[src_op.reg & 15].u32_[i];
            for (int i = 0; i < 4; ++i) src[4+i] = ctx.cpu.avx.ymm_high[src_op.reg & 15].u32_[i];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            for (int i = 0; i < 8; ++i) {
                auto v = read_memory(ctx, addr.value() + i * 4, 4);
                if (!v) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                src[i] = static_cast<u32>(v.value());
            }
        }
        // Permute
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 4; ++i) dst_lo.u32_[i] = src[indices[i] & 7];
        for (int i = 0; i < 4; ++i) dst_hi.u32_[i] = src[indices[4+i] & 7];
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPERMPS ymm, ymm, ymm/m256 — permute f32s using indices (AVX2)
    // ========================================================================
    case Opcode::VPERMPS_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 idx_reg = instr.prefixes.vex_vvvv & 15;
        u32 indices[8];
        for (int i = 0; i < 4; ++i) indices[i] = ctx.cpu.sse.xmm[idx_reg].u32_[i];
        for (int i = 0; i < 4; ++i) indices[4+i] = ctx.cpu.avx.ymm_high[idx_reg].u32_[i];
        u32 src[8];
        if (is_register_operand(src_op.type)) {
            for (int i = 0; i < 4; ++i) src[i] = ctx.cpu.sse.xmm[src_op.reg & 15].u32_[i];
            for (int i = 0; i < 4; ++i) src[4+i] = ctx.cpu.avx.ymm_high[src_op.reg & 15].u32_[i];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            for (int i = 0; i < 8; ++i) {
                auto v = read_memory(ctx, addr.value() + i * 4, 4);
                if (!v) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                src[i] = static_cast<u32>(v.value());
            }
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 4; ++i) dst_lo.u32_[i] = src[indices[i] & 7];
        for (int i = 0; i < 4; ++i) dst_hi.u32_[i] = src[indices[4+i] & 7];
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPERMQ ymm, ymm/m256, imm8 — permute qwords (AVX2)
    // ========================================================================
    case Opcode::VPERMQ_YMM_YMMM256_IMM8: {
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 dst_idx = dst_op.reg & 15;
        u64 src_q[4];
        if (is_register_operand(src_op.type)) {
            src_q[0] = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[0];
            src_q[1] = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[1];
            src_q[2] = ctx.cpu.avx.ymm_high[src_op.reg & 15].u64_[0];
            src_q[3] = ctx.cpu.avx.ymm_high[src_op.reg & 15].u64_[1];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            for (int i = 0; i < 4; ++i) {
                auto lo = read_memory(ctx, addr.value() + i * 8, 4);
                auto hi = read_memory(ctx, addr.value() + i * 8 + 4, 4);
                if (!lo || !hi) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                src_q[i] = static_cast<u64>(static_cast<u32>(lo.value())) |
                           (static_cast<u64>(static_cast<u32>(hi.value())) << 32);
            }
        }
        ctx.cpu.sse.xmm[dst_idx].u64_[0] = src_q[(imm >> 0) & 3];
        ctx.cpu.sse.xmm[dst_idx].u64_[1] = src_q[(imm >> 2) & 3];
        ctx.cpu.avx.ymm_high[dst_idx].u64_[0] = src_q[(imm >> 4) & 3];
        ctx.cpu.avx.ymm_high[dst_idx].u64_[1] = src_q[(imm >> 6) & 3];
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPERMPD ymm, ymm/m256, imm8 — permute f64s (AVX2)
    // ========================================================================
    case Opcode::VPERMPD_YMM_YMMM256_IMM8: {
        // Same logic as VPERMQ (bit patterns identical for f64/u64)
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 dst_idx = dst_op.reg & 15;
        u64 src_q[4];
        if (is_register_operand(src_op.type)) {
            src_q[0] = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[0];
            src_q[1] = ctx.cpu.sse.xmm[src_op.reg & 15].u64_[1];
            src_q[2] = ctx.cpu.avx.ymm_high[src_op.reg & 15].u64_[0];
            src_q[3] = ctx.cpu.avx.ymm_high[src_op.reg & 15].u64_[1];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            for (int i = 0; i < 4; ++i) {
                auto lo = read_memory(ctx, addr.value() + i * 8, 4);
                auto hi = read_memory(ctx, addr.value() + i * 8 + 4, 4);
                if (!lo || !hi) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                src_q[i] = static_cast<u64>(static_cast<u32>(lo.value())) |
                           (static_cast<u64>(static_cast<u32>(hi.value())) << 32);
            }
        }
        ctx.cpu.sse.xmm[dst_idx].u64_[0] = src_q[(imm >> 0) & 3];
        ctx.cpu.sse.xmm[dst_idx].u64_[1] = src_q[(imm >> 2) & 3];
        ctx.cpu.avx.ymm_high[dst_idx].u64_[0] = src_q[(imm >> 4) & 3];
        ctx.cpu.avx.ymm_high[dst_idx].u64_[1] = src_q[(imm >> 6) & 3];
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPBLENDD — 128/256-bit dword blend by imm8 mask (AVX2)
    // ========================================================================
    case Opcode::VPBLENDD_XMM_XMM_XMMM128_IMM8: {
        auto src2 = read_xmm_or_m128(ctx, src_op);
        if (!src2) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = (imm & (1 << i)) ? src2.value().u32_[i] : src1.u32_[i];
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPBLENDD_YMM_YMM_YMMM256_IMM8: {
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        // Read src2 (256-bit)
        XmmReg s2_lo{}, s2_hi{};
        if (is_register_operand(src_op.type)) {
            s2_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            s2_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&s2_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&s2_hi), 16);
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 4; ++i)
            dst_lo.u32_[i] = (imm & (1 << i)) ? s2_lo.u32_[i]
                : ctx.cpu.sse.xmm[src1_idx].u32_[i];
        for (int i = 0; i < 4; ++i)
            dst_hi.u32_[i] = (imm & (1 << (i + 4))) ? s2_hi.u32_[i]
                : ctx.cpu.avx.ymm_high[src1_idx].u32_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPBLENDVB — byte blend by MSB of each mask byte (is4 mask register)
    // ========================================================================
    case Opcode::VPBLENDVB_XMM_XMM_XMMM128_XMM: {
        auto src2 = read_xmm_or_m128(ctx, src_op);
        if (!src2) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 is4 = static_cast<u8>(instr.operands[2].imm);
        u8 mask_idx = (is4 >> 4) & 15;
        if (!ctx.cpu.is_64bit()) mask_idx &= 7;
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        const auto& mask = ctx.cpu.sse.xmm[mask_idx];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 16; ++i)
            dst.u8_[i] = (mask.u8_[i] & 0x80) ? src2.value().u8_[i] : src1.u8_[i];
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPBLENDVB_YMM_YMM_YMMM256_YMM: {
        u8 is4 = static_cast<u8>(instr.operands[2].imm);
        u8 mask_idx = (is4 >> 4) & 15;
        if (!ctx.cpu.is_64bit()) mask_idx &= 7;
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg s2_lo{}, s2_hi{};
        if (is_register_operand(src_op.type)) {
            s2_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            s2_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&s2_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&s2_hi), 16);
        }
        const auto& m_lo = ctx.cpu.sse.xmm[mask_idx];
        const auto& m_hi = ctx.cpu.avx.ymm_high[mask_idx];
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 16; ++i) {
            dst_lo.u8_[i] = (m_lo.u8_[i] & 0x80)
                ? s2_lo.u8_[i] : ctx.cpu.sse.xmm[src1_idx].u8_[i];
            dst_hi.u8_[i] = (m_hi.u8_[i] & 0x80)
                ? s2_hi.u8_[i] : ctx.cpu.avx.ymm_high[src1_idx].u8_[i];
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // VBLENDVPS / VBLENDVPD — element blend by the MSB of the corresponding
    // mask element (mask register named in imm8[7:4]). Intel SDM Vol. 2:
    // VBLENDVPS selects per 32-bit element, VBLENDVPD per 64-bit element; the
    // remaining mask bits are ignored.
    // ========================================================================
    case Opcode::VBLENDVPS_XMM_XMM_XMMM128_XMM:
    case Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM: {
        auto src2 = read_xmm_or_m128(ctx, src_op);
        if (!src2) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        const bool pd = instr.opcode == Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM;
        u8 is4 = static_cast<u8>(instr.operands[2].imm);
        u8 mask_idx = (is4 >> 4) & 15;
        if (!ctx.cpu.is_64bit()) mask_idx &= 7;
        u8 dst_idx = dst_op.reg & 15;
        const auto src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        const auto mask = ctx.cpu.sse.xmm[mask_idx];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        if (pd) {
            for (int i = 0; i < 2; ++i)
                dst.u64_[i] = (mask.u64_[i] >> 63)
                    ? src2.value().u64_[i] : src1.u64_[i];
        } else {
            for (int i = 0; i < 4; ++i)
                dst.u32_[i] = (mask.u32_[i] >> 31)
                    ? src2.value().u32_[i] : src1.u32_[i];
        }
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM:
    case Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM: {
        const bool pd = instr.opcode == Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM;
        u8 is4 = static_cast<u8>(instr.operands[2].imm);
        u8 mask_idx = (is4 >> 4) & 15;
        if (!ctx.cpu.is_64bit()) mask_idx &= 7;
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg s2_lo{}, s2_hi{};
        if (is_register_operand(src_op.type)) {
            s2_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            s2_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&s2_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&s2_hi), 16);
        }
        const auto m_lo = ctx.cpu.sse.xmm[mask_idx];
        const auto m_hi = ctx.cpu.avx.ymm_high[mask_idx];
        const auto s1_lo = ctx.cpu.sse.xmm[src1_idx];
        const auto s1_hi = ctx.cpu.avx.ymm_high[src1_idx];
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        if (pd) {
            for (int i = 0; i < 2; ++i) {
                dst_lo.u64_[i] = (m_lo.u64_[i] >> 63) ? s2_lo.u64_[i] : s1_lo.u64_[i];
                dst_hi.u64_[i] = (m_hi.u64_[i] >> 63) ? s2_hi.u64_[i] : s1_hi.u64_[i];
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                dst_lo.u32_[i] = (m_lo.u32_[i] >> 31) ? s2_lo.u32_[i] : s1_lo.u32_[i];
                dst_hi.u32_[i] = (m_hi.u32_[i] >> 31) ? s2_hi.u32_[i] : s1_hi.u32_[i];
            }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // Variable shifts (AVX2) — each element shifted by its corresponding count
    // ========================================================================
    case Opcode::VPSLLVD_XMM_XMM_XMMM128: {
        auto counts = read_xmm_or_m128(ctx, src_op);
        if (!counts) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 4; ++i) {
            u32 cnt = counts.value().u32_[i];
            dst.u32_[i] = (cnt > 31) ? 0 : (src1.u32_[i] << cnt);
        }
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPSLLVD_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg c_lo{}, c_hi{};
        if (is_register_operand(src_op.type)) {
            c_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            c_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&c_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&c_hi), 16);
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 4; ++i) {
            u32 cnt = c_lo.u32_[i];
            dst_lo.u32_[i] = (cnt > 31) ? 0 : (ctx.cpu.sse.xmm[src1_idx].u32_[i] << cnt);
        }
        for (int i = 0; i < 4; ++i) {
            u32 cnt = c_hi.u32_[i];
            dst_hi.u32_[i] = (cnt > 31) ? 0 : (ctx.cpu.avx.ymm_high[src1_idx].u32_[i] << cnt);
        }
        return ExecResult::Continue;
    }
    case Opcode::VPSLLVQ_XMM_XMM_XMMM128: {
        auto counts = read_xmm_or_m128(ctx, src_op);
        if (!counts) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 2; ++i) {
            u64 cnt = counts.value().u64_[i];
            dst.u64_[i] = (cnt > 63) ? 0 : (src1.u64_[i] << cnt);
        }
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPSLLVQ_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg c_lo{}, c_hi{};
        if (is_register_operand(src_op.type)) {
            c_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            c_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&c_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&c_hi), 16);
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 2; ++i) {
            u64 cnt = c_lo.u64_[i];
            dst_lo.u64_[i] = (cnt > 63) ? 0 : (ctx.cpu.sse.xmm[src1_idx].u64_[i] << cnt);
        }
        for (int i = 0; i < 2; ++i) {
            u64 cnt = c_hi.u64_[i];
            dst_hi.u64_[i] = (cnt > 63) ? 0 : (ctx.cpu.avx.ymm_high[src1_idx].u64_[i] << cnt);
        }
        return ExecResult::Continue;
    }
    case Opcode::VPSRLVD_XMM_XMM_XMMM128: {
        auto counts = read_xmm_or_m128(ctx, src_op);
        if (!counts) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 4; ++i) {
            u32 cnt = counts.value().u32_[i];
            dst.u32_[i] = (cnt > 31) ? 0 : (src1.u32_[i] >> cnt);
        }
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPSRLVD_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg c_lo{}, c_hi{};
        if (is_register_operand(src_op.type)) {
            c_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            c_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&c_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&c_hi), 16);
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 4; ++i) {
            u32 cnt = c_lo.u32_[i];
            dst_lo.u32_[i] = (cnt > 31) ? 0 : (ctx.cpu.sse.xmm[src1_idx].u32_[i] >> cnt);
        }
        for (int i = 0; i < 4; ++i) {
            u32 cnt = c_hi.u32_[i];
            dst_hi.u32_[i] = (cnt > 31) ? 0 : (ctx.cpu.avx.ymm_high[src1_idx].u32_[i] >> cnt);
        }
        return ExecResult::Continue;
    }
    case Opcode::VPSRLVQ_XMM_XMM_XMMM128: {
        auto counts = read_xmm_or_m128(ctx, src_op);
        if (!counts) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 2; ++i) {
            u64 cnt = counts.value().u64_[i];
            dst.u64_[i] = (cnt > 63) ? 0 : (src1.u64_[i] >> cnt);
        }
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPSRLVQ_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg c_lo{}, c_hi{};
        if (is_register_operand(src_op.type)) {
            c_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            c_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&c_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&c_hi), 16);
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 2; ++i) {
            u64 cnt = c_lo.u64_[i];
            dst_lo.u64_[i] = (cnt > 63) ? 0 : (ctx.cpu.sse.xmm[src1_idx].u64_[i] >> cnt);
        }
        for (int i = 0; i < 2; ++i) {
            u64 cnt = c_hi.u64_[i];
            dst_hi.u64_[i] = (cnt > 63) ? 0 : (ctx.cpu.avx.ymm_high[src1_idx].u64_[i] >> cnt);
        }
        return ExecResult::Continue;
    }
    case Opcode::VPSRAVD_XMM_XMM_XMMM128: {
        auto counts = read_xmm_or_m128(ctx, src_op);
        if (!counts) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        u8 dst_idx = dst_op.reg & 15;
        const auto& src1 = ctx.cpu.sse.xmm[instr.prefixes.vex_vvvv & 15];
        auto& dst = ctx.cpu.sse.xmm[dst_idx];
        for (int i = 0; i < 4; ++i) {
            u32 cnt = counts.value().u32_[i];
            i32 v = static_cast<i32>(src1.u32_[i]);
            dst.u32_[i] = static_cast<u32>((cnt > 31) ? (v >> 31) : (v >> cnt));
        }
        ctx.cpu.avx.ymm_high[dst_idx].clear();
        return ExecResult::Continue;
    }
    case Opcode::VPSRAVD_YMM_YMM_YMMM256: {
        u8 dst_idx = dst_op.reg & 15;
        u8 src1_idx = instr.prefixes.vex_vvvv & 15;
        XmmReg c_lo{}, c_hi{};
        if (is_register_operand(src_op.type)) {
            c_lo = ctx.cpu.sse.xmm[src_op.reg & 15];
            c_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
        } else {
            auto addr = calc_effective_address(ctx, src_op.mem);
            if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctx.mem.read_bytes(addr.value(), reinterpret_cast<Byte*>(&c_lo), 16);
            ctx.mem.read_bytes(addr.value() + 16, reinterpret_cast<Byte*>(&c_hi), 16);
        }
        auto& dst_lo = ctx.cpu.sse.xmm[dst_idx];
        auto& dst_hi = ctx.cpu.avx.ymm_high[dst_idx];
        for (int i = 0; i < 4; ++i) {
            u32 cnt = c_lo.u32_[i];
            i32 v = static_cast<i32>(ctx.cpu.sse.xmm[src1_idx].u32_[i]);
            dst_lo.u32_[i] = static_cast<u32>((cnt > 31) ? (v >> 31) : (v >> cnt));
        }
        for (int i = 0; i < 4; ++i) {
            u32 cnt = c_hi.u32_[i];
            i32 v = static_cast<i32>(ctx.cpu.avx.ymm_high[src1_idx].u32_[i]);
            dst_hi.u32_[i] = static_cast<u32>((cnt > 31) ? (v >> 31) : (v >> cnt));
        }
        return ExecResult::Continue;
    }

    case Opcode::VZEROUPPER:
        ctx.cpu.avx.clear();
        return ExecResult::Continue;
    case Opcode::VZEROALL:
        for (auto& xmm : ctx.cpu.sse.xmm) xmm.clear();
        ctx.cpu.avx.clear();
        return ExecResult::Continue;

    case Opcode::VGATHERDPS_XMM_VM32X_XMM:
    case Opcode::VGATHERDPS_YMM_VM32Y_YMM:
    case Opcode::VPGATHERDD_XMM_VM32X_XMM:
    case Opcode::VPGATHERDD_YMM_VM32Y_YMM: {
        // Intel SDM Vol. 2 VGATHERDPS / VPGATHERDD. Mask is VEX.vvvv.
        const bool ymm = instr.opcode == Opcode::VGATHERDPS_YMM_VM32Y_YMM
            || instr.opcode == Opcode::VPGATHERDD_YMM_VM32Y_YMM;
        const int n = ymm ? 8 : 4;
        const u8 dst = dst_op.reg & 15;
        const u8 mask_reg = instr.prefixes.vex_vvvv & 15;
        const MemoryOperand& vm = src_op.mem;
        // Intel SDM Vol. 2 VGATHERDPS (VEX): #UD if any pair of dest,
        // index, and mask is the same register.
        const u8 index_reg = vm.index & 15;
        if (dst == mask_reg || dst == index_reg || mask_reg == index_reg) {
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
        }
        const bool is_32bit = !ctx.cpu.is_64bit();
        u64 base = 0;
        if (vm.has_base()) {
            base = is_32bit ? ctx.cpu.gpr.get32(vm.base) : ctx.cpu.gpr.get64(vm.base);
        }
        if (vm.segment != SegReg::Default) {
            auto seg_idx = static_cast<SegIndex>(static_cast<u8>(vm.segment));
            base += ctx.cpu.seg.get_desc(seg_idx).base;
        }
        auto dword_at = [&](u8 reg, int i) -> u32 {
            if (i < 4) return ctx.cpu.sse.xmm[reg].u32_[i];
            return ctx.cpu.avx.ymm_high[reg].u32_[i - 4];
        };
        auto set_dword = [&](u8 reg, int i, u32 v) {
            if (i < 4) ctx.cpu.sse.xmm[reg].u32_[i] = v;
            else ctx.cpu.avx.ymm_high[reg].u32_[i - 4] = v;
        };
        for (int i = 0; i < n; ++i) {
            const u32 m = dword_at(mask_reg, i);
            if ((m & 0x80000000u) != 0) {
                const u32 idx = dword_at(index_reg, i);
                u64 addr = base + static_cast<u64>(static_cast<i32>(idx)) * vm.scale
                    + static_cast<u64>(static_cast<i64>(vm.disp));
                if (is_32bit) addr = static_cast<u32>(addr);
                auto val = read_memory(ctx, addr, 4);
                if (!val) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                set_dword(dst, i, static_cast<u32>(val.value()));
            }
            // Intel SDM Vol. 2 VGATHERDPS: the entire mask is zeroed unless
            // the instruction takes an exception. Unmasked dest lanes stay.
            set_dword(mask_reg, i, 0);
        }
        if (!ymm) ctx.cpu.avx.ymm_high[dst].clear();
        // Intel SDM Vol. 2 VGATHERDPS: on completion the mask register is
        // zeroed in full, not merely the elements that were consulted. The
        // loop above clears the used ones; this clears the remainder,
        // including the ymm high half of a 128-bit form. Doing it only after
        // the loop keeps the partial-fault behaviour intact: a fault returns
        // early with exactly the completed prefix retired.
        ctx.cpu.sse.xmm[mask_reg].clear();
        ctx.cpu.avx.ymm_high[mask_reg].clear();
        return ExecResult::Continue;
    }

    // ========================================================================
    // AVX2 gathers with qword indices and/or qword elements.
    // Intel SDM Vol. 2 VGATHERQPS / VGATHERDPD / VGATHERQPD (and the integer
    // VPGATHER* forms, which move identical bits).
    //
    // All six share one shape: n = VEX.L ? 4 : 2 elements, an index vector
    // whose element width is set by the opcode byte, and a destination and
    // mask whose element width is set by VEX.W.
    // ========================================================================
    case Opcode::VGATHERQPS:
    case Opcode::VPGATHERQD:
    case Opcode::VGATHERDPD:
    case Opcode::VPGATHERDQ:
    case Opcode::VGATHERQPD:
    case Opcode::VPGATHERQQ: {
        const bool index_qword = instr.opcode == Opcode::VGATHERQPS
            || instr.opcode == Opcode::VPGATHERQD
            || instr.opcode == Opcode::VGATHERQPD
            || instr.opcode == Opcode::VPGATHERQQ;
        const bool elem_qword = instr.opcode == Opcode::VGATHERDPD
            || instr.opcode == Opcode::VPGATHERDQ
            || instr.opcode == Opcode::VGATHERQPD
            || instr.opcode == Opcode::VPGATHERQQ;
        const int n = instr.prefixes.vex_l ? 4 : 2;
        const u8 elem_size = elem_qword ? 8 : 4;

        const u8 dst = dst_op.reg & 15;
        const u8 mask_reg = instr.prefixes.vex_vvvv & 15;
        const MemoryOperand& vm = src_op.mem;
        const u8 index_reg = vm.index & 15;

        // Intel SDM Vol. 2 VGATHERQPS: #UD if any pair of destination, index,
        // and mask names the same register.
        if (dst == mask_reg || dst == index_reg || mask_reg == index_reg) {
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
        }

        const bool is_32bit = !ctx.cpu.is_64bit();
        u64 base = 0;
        if (vm.has_base()) {
            base = is_32bit ? ctx.cpu.gpr.get32(vm.base) : ctx.cpu.gpr.get64(vm.base);
        }
        if (vm.segment != SegReg::Default) {
            auto seg_idx = static_cast<SegIndex>(static_cast<u8>(vm.segment));
            base += ctx.cpu.seg.get_desc(seg_idx).base;
        }

        auto dword_at = [&](u8 reg, int i) -> u32 {
            return (i < 4) ? ctx.cpu.sse.xmm[reg].u32_[i]
                           : ctx.cpu.avx.ymm_high[reg].u32_[i - 4];
        };
        auto qword_at = [&](u8 reg, int i) -> u64 {
            return (i < 2) ? ctx.cpu.sse.xmm[reg].u64_[i]
                           : ctx.cpu.avx.ymm_high[reg].u64_[i - 2];
        };
        auto set_elem = [&](u8 reg, int i, u64 v) {
            if (elem_qword) {
                if (i < 2) ctx.cpu.sse.xmm[reg].u64_[i] = v;
                else ctx.cpu.avx.ymm_high[reg].u64_[i - 2] = v;
            } else if (i < 4) {
                ctx.cpu.sse.xmm[reg].u32_[i] = static_cast<u32>(v);
            } else {
                ctx.cpu.avx.ymm_high[reg].u32_[i - 4] = static_cast<u32>(v);
            }
        };
        auto mask_elem = [&](int i) -> bool {
            return elem_qword ? ((qword_at(mask_reg, i) >> 63) != 0)
                              : ((dword_at(mask_reg, i) & 0x80000000u) != 0);
        };

        for (int i = 0; i < n; ++i) {
            if (mask_elem(i)) {
                // Index elements are sign-extended before scaling.
                const i64 idx = index_qword
                    ? static_cast<i64>(qword_at(index_reg, i))
                    : static_cast<i64>(static_cast<i32>(dword_at(index_reg, i)));
                u64 addr = base + static_cast<u64>(idx) * vm.scale
                    + static_cast<u64>(static_cast<i64>(vm.disp));
                if (is_32bit) addr = static_cast<u32>(addr);
                u64 value = 0;
                if (elem_qword) {
                    auto lo = read_memory(ctx, addr, 4);
                    auto hi = read_memory(ctx, addr + 4, 4);
                    if (!lo || !hi) {
                        ctx.exception = CpuException::GeneralProtection;
                        return ExecResult::Exception;
                    }
                    value = static_cast<u64>(static_cast<u32>(lo.value()))
                        | (static_cast<u64>(static_cast<u32>(hi.value())) << 32);
                } else {
                    auto val = read_memory(ctx, addr, 4);
                    if (!val) {
                        ctx.exception = CpuException::GeneralProtection;
                        return ExecResult::Exception;
                    }
                    value = static_cast<u32>(val.value());
                }
                set_elem(dst, i, value);
            }
            // The mask is fully zeroed on completion; destination elements
            // whose mask bit was clear keep their previous contents.
            set_elem(mask_reg, i, 0);
        }

        // Zero the destination bits above the n gathered elements. For a
        // dword-element form with n=2 that is the upper 64 bits of the xmm as
        // well as the whole ymm high half.
        if (!elem_qword) {
            for (int i = n; i < 4; ++i) ctx.cpu.sse.xmm[dst].u32_[i] = 0;
            ctx.cpu.avx.ymm_high[dst].clear();
        } else if (n == 2) {
            ctx.cpu.avx.ymm_high[dst].clear();
        }
        // The whole mask register is zeroed, not just the consulted elements
        // — see the note on the dword-index arm above.
        ctx.cpu.sse.xmm[mask_reg].clear();
        ctx.cpu.avx.ymm_high[mask_reg].clear();
        return ExecResult::Continue;
    }

    // ========================================================================
    // VPERMILPS / VPERMILPD — permute floats *within* each 128-bit lane.
    // Intel SDM Vol. 2 VPERMILPS: the control never crosses the lane boundary,
    // which is what separates these from VPERMPS/VPERMPD.
    // ========================================================================
    case Opcode::VPERMILPS_VAR:
    case Opcode::VPERMILPS_IMM:
    case Opcode::VPERMILPD_VAR:
    case Opcode::VPERMILPD_IMM: {
        const bool pd = instr.opcode == Opcode::VPERMILPD_VAR
            || instr.opcode == Opcode::VPERMILPD_IMM;
        const bool var = instr.opcode == Opcode::VPERMILPS_VAR
            || instr.opcode == Opcode::VPERMILPD_VAR;
        const bool ymm = instr.prefixes.vex_l;
        const u8 dst = dst_op.reg & 15;

        // Variable form: source is VEX.vvvv, control is the r/m operand.
        // Immediate form: source is the r/m operand, control is imm8.
        XmmReg ctrl_lo{}, ctrl_hi{};
        XmmReg src_lo{}, src_hi{};
        u8 imm = 0;
        if (var) {
            const u8 src_reg = instr.prefixes.vex_vvvv & 15;
            src_lo = ctx.cpu.sse.xmm[src_reg];
            src_hi = ctx.cpu.avx.ymm_high[src_reg];
            auto c = read_xmm_or_m128(ctx, src_op);
            if (!c) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            ctrl_lo = c.value();
            if (ymm) {
                if (src_op.type == OperandType::Reg128) {
                    ctrl_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
                } else {
                    auto addr = calc_effective_address(ctx, src_op.mem);
                    if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                    auto r = ctx.mem.read_bytes(addr.value() + 16,
                                                reinterpret_cast<Byte*>(&ctrl_hi), 16);
                    if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                }
            }
        } else {
            if (instr.operand_count < 3) {
                ctx.exception = CpuException::InvalidOpcode;
                return ExecResult::Exception;
            }
            imm = static_cast<u8>(instr.operands[2].imm);
            auto s = read_xmm_or_m128(ctx, src_op);
            if (!s) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            src_lo = s.value();
            if (ymm) {
                if (src_op.type == OperandType::Reg128) {
                    src_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
                } else {
                    auto addr = calc_effective_address(ctx, src_op.mem);
                    if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                    auto r = ctx.mem.read_bytes(addr.value() + 16,
                                                reinterpret_cast<Byte*>(&src_hi), 16);
                    if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                }
            }
        }

        XmmReg out_lo{}, out_hi{};
        auto permute_lane = [&](const XmmReg& src, const XmmReg& ctrl,
                                int lane, XmmReg& out) {
            if (pd) {
                // One selector bit per qword: bit 1 of the control element in
                // the variable form, or imm8 bit (2*lane + i) in the immediate
                // form (Intel SDM Vol. 2 VPERMILPD).
                for (int i = 0; i < 2; ++i) {
                    const unsigned sel = var
                        ? static_cast<unsigned>((ctrl.u64_[i] >> 1) & 1)
                        : static_cast<unsigned>((imm >> (2 * lane + i)) & 1);
                    out.u64_[i] = src.u64_[sel];
                }
            } else {
                // Two selector bits per dword. The immediate form applies the
                // same imm8 to both lanes.
                for (int i = 0; i < 4; ++i) {
                    const unsigned sel = var
                        ? static_cast<unsigned>(ctrl.u32_[i] & 3)
                        : static_cast<unsigned>((imm >> (2 * i)) & 3);
                    out.u32_[i] = src.u32_[sel];
                }
            }
        };

        permute_lane(src_lo, ctrl_lo, 0, out_lo);
        if (ymm) permute_lane(src_hi, ctrl_hi, 1, out_hi);

        ctx.cpu.sse.xmm[dst] = out_lo;
        if (ymm) ctx.cpu.avx.ymm_high[dst] = out_hi;
        else ctx.cpu.avx.ymm_high[dst].clear();
        return ExecResult::Continue;
    }

    // ========================================================================
    // VTESTPS / VTESTPD — like PTEST, but only the element sign bits
    // participate. Intel SDM Vol. 2 VTESTPS.
    //
    // This is a *full* flag write (ZF and CF computed, the rest cleared), so
    // it clears the lazy FlagOp rather than leaving one pending — a partial
    // flag writer that leaves a lazy op live diverges in mixed blocks.
    // ========================================================================
    case Opcode::VTESTPS:
    case Opcode::VTESTPD: {
        const bool pd = instr.opcode == Opcode::VTESTPD;
        const bool ymm = instr.prefixes.vex_l;
        const u8 dst = dst_op.reg & 15;

        auto src_r = read_xmm_or_m128(ctx, src_op);
        if (!src_r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
        XmmReg src_lo = src_r.value(), src_hi{};
        if (ymm) {
            if (src_op.type == OperandType::Reg128) {
                src_hi = ctx.cpu.avx.ymm_high[src_op.reg & 15];
            } else {
                auto addr = calc_effective_address(ctx, src_op.mem);
                if (!addr) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
                auto r = ctx.mem.read_bytes(addr.value() + 16,
                                            reinterpret_cast<Byte*>(&src_hi), 16);
                if (!r) { ctx.exception = CpuException::GeneralProtection; return ExecResult::Exception; }
            }
        }

        // Sign bits only. Collapse each 128-bit lane to a mask of its element
        // sign bits, then apply the PTEST relations to those masks.
        auto sign_mask = [&](const XmmReg& v) -> u32 {
            u32 m = 0;
            if (pd) {
                for (int i = 0; i < 2; ++i)
                    m |= static_cast<u32>((v.u64_[i] >> 63) & 1) << i;
            } else {
                for (int i = 0; i < 4; ++i)
                    m |= static_cast<u32>((v.u32_[i] >> 31) & 1) << i;
            }
            return m;
        };

        const u32 d = sign_mask(ctx.cpu.sse.xmm[dst])
            | (ymm ? sign_mask(ctx.cpu.avx.ymm_high[dst]) << 4 : 0u);
        const u32 s = sign_mask(src_lo) | (ymm ? sign_mask(src_hi) << 4 : 0u);

        ctx.cpu.flags.set_zf((s & d) == 0);
        ctx.cpu.flags.set_cf((s & ~d) == 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_sf(false);
        ctx.cpu.flags.set_af(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }

    // VPMASKMOVD/Q — Intel SDM Vol. 2. Mask is VEX.vvvv MSB of each
    // dword/qword. Load zeros unmasked dest lanes; store leaves unmasked
    // memory unchanged. Faults only on masked element accesses.
    // VMASKMOVPS/PD move exactly the same bits as VPMASKMOVD/Q — the float
    // and integer forms differ only in which execution unit a real CPU uses,
    // not in the result — so they share this arm (Intel SDM Vol. 2
    // VMASKMOV / VPMASKMOV).
    case Opcode::VPMASKMOVD_LOAD:
    case Opcode::VPMASKMOVD_STORE:
    case Opcode::VPMASKMOVQ_LOAD:
    case Opcode::VPMASKMOVQ_STORE:
    case Opcode::VMASKMOVPS_LOAD:
    case Opcode::VMASKMOVPS_STORE:
    case Opcode::VMASKMOVPD_LOAD:
    case Opcode::VMASKMOVPD_STORE: {
        const bool store = instr.opcode == Opcode::VPMASKMOVD_STORE
            || instr.opcode == Opcode::VPMASKMOVQ_STORE
            || instr.opcode == Opcode::VMASKMOVPS_STORE
            || instr.opcode == Opcode::VMASKMOVPD_STORE;
        const bool qword = instr.opcode == Opcode::VPMASKMOVQ_LOAD
            || instr.opcode == Opcode::VPMASKMOVQ_STORE
            || instr.opcode == Opcode::VMASKMOVPD_LOAD
            || instr.opcode == Opcode::VMASKMOVPD_STORE;
        const bool ymm = instr.prefixes.vex_l;
        const int n = qword ? (ymm ? 4 : 2) : (ymm ? 8 : 4);
        const u8 elem = qword ? 8 : 4;
        const u8 mask_reg = instr.prefixes.vex_vvvv & 15;
        auto lane_bits = [&](u8 reg, int i) -> u64 {
            if (qword) {
                return (i < 2) ? ctx.cpu.sse.xmm[reg].u64_[i]
                               : ctx.cpu.avx.ymm_high[reg].u64_[i - 2];
            }
            return (i < 4) ? ctx.cpu.sse.xmm[reg].u32_[i]
                           : ctx.cpu.avx.ymm_high[reg].u32_[i - 4];
        };
        auto set_lane = [&](u8 reg, int i, u64 v) {
            if (qword) {
                if (i < 2) ctx.cpu.sse.xmm[reg].u64_[i] = v;
                else ctx.cpu.avx.ymm_high[reg].u64_[i - 2] = v;
            } else if (i < 4) {
                ctx.cpu.sse.xmm[reg].u32_[i] = static_cast<u32>(v);
            } else {
                ctx.cpu.avx.ymm_high[reg].u32_[i - 4] = static_cast<u32>(v);
            }
        };
        bool masked[8]{};
        for (int i = 0; i < n; ++i) {
            const u64 m = lane_bits(mask_reg, i);
            masked[i] = qword ? ((m >> 63) != 0)
                              : ((static_cast<u32>(m) & 0x80000000u) != 0);
        }
        const Operand& mem_op = store ? dst_op : src_op;
        auto addr = calc_effective_address(ctx, mem_op.mem);
        if (!addr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        if (store) {
            const u8 src = src_op.reg & 15;
            for (int i = 0; i < n; ++i) {
                if (!masked[i]) continue;
                auto w = write_memory(ctx, addr.value() + static_cast<u64>(i) * elem,
                                      lane_bits(src, i), elem);
                if (!w) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
            }
        } else {
            const u8 dst = dst_op.reg & 15;
            for (int i = 0; i < n; ++i) {
                if (!masked[i]) {
                    set_lane(dst, i, 0);
                    continue;
                }
                auto val = read_memory(ctx, addr.value() + static_cast<u64>(i) * elem,
                                       elem);
                if (!val) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                set_lane(dst, i, val.value());
            }
            if (!ymm) ctx.cpu.avx.ymm_high[dst].clear();
        }
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
