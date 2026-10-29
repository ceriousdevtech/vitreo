// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// XSAVE / XRSTOR / XSAVEOPT / XGETBV (Intel SDM Vol. 1 ch. 13, Vol. 2)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

constexpr u32 kXsaveAlign = 64;

VoidResult write_u64_at(ExecContext& ctx, u64 addr, u64 v) noexcept {
    return write_memory(ctx, addr, v, 8);
}

Result<u64, Error> read_u64_at(ExecContext& ctx, u64 addr) noexcept {
    return read_memory(ctx, addr, 8);
}

ExecResult xsave_image(ExecContext& ctx, const DecodedInstruction& instr, bool /*opt*/) noexcept {
    auto addr = calc_effective_address(ctx, instr.operands[0].mem);
    if (!addr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    const u64 base = addr.value();
    if ((base & (kXsaveAlign - 1)) != 0) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    const u64 req = (static_cast<u64>(ctx.cpu.gpr.get32(2)) << 32) | ctx.cpu.gpr.get32(0);
    const u64 mask = req & ctx.cpu.xcr0;
    // Header XSTATE_BV at offset 512.
    if (mask & 3) {
        // Legacy 512-byte region holds x87 + MXCSR + XMM (Intel SDM Vol. 1 §13.4).
        DecodedInstruction fx = instr;
        fx.opcode = Opcode::FXSAVE_M512;
        auto r = exec_sse_data(ctx, fx);
        if (r != ExecResult::Continue) return r;
    }
    if (mask & 2) {
        // SSE XMM already stored by FXSAVE
    }
    if (mask & 4) {
        // YMM high at offset 576
        for (u8 i = 0; i < 16; ++i) {
            auto wr = ctx.mem.write_bytes(
                base + 576 + static_cast<u64>(i) * 16,
                reinterpret_cast<const Byte*>(&ctx.cpu.avx.ymm_high[i]), 16);
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
    }
    u64 bv = mask & 0x7;
    if (!write_u64_at(ctx, base + 512, bv)) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

}  // namespace

ExecResult exec_xsave_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        case Opcode::XGETBV: {
            // Intel SDM Vol. 2 XGETBV: ECX selects XCR. Only XCR0 (ECX=0) is
            // architectural in this user-mode profile; anything else is #GP.
            if (ctx.cpu.gpr.get32(1) != 0) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            const u64 xcr = ctx.cpu.xcr0;
            ctx.cpu.gpr.set32(0, static_cast<u32>(xcr));
            ctx.cpu.gpr.set32(2, static_cast<u32>(xcr >> 32));
            return ExecResult::Continue;
        }
        case Opcode::XSAVE_M:
        case Opcode::XSAVEOPT_M:
            return xsave_image(ctx, instr, instr.opcode == Opcode::XSAVEOPT_M);
        case Opcode::XRSTOR_M: {
            auto addr = calc_effective_address(ctx, instr.operands[0].mem);
            if (!addr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            const u64 base = addr.value();
            if ((base & (kXsaveAlign - 1)) != 0) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            const u64 req =
                (static_cast<u64>(ctx.cpu.gpr.get32(2)) << 32) | ctx.cpu.gpr.get32(0);
            const u64 mask = req & ctx.cpu.xcr0;
            auto bv_r = read_u64_at(ctx, base + 512);
            const u64 bv = bv_r ? bv_r.value() : 0;
            if (mask & 1) {
                DecodedInstruction fx = instr;
                fx.opcode = Opcode::FXRSTOR_M512;
                auto r = exec_sse_data(ctx, fx);
                if (r != ExecResult::Continue) return r;
            }
            if ((mask & 4) && (bv & 4)) {
                for (u8 i = 0; i < 16; ++i) {
                    auto rd = ctx.mem.read_bytes(
                        base + 576 + static_cast<u64>(i) * 16,
                        reinterpret_cast<Byte*>(&ctx.cpu.avx.ymm_high[i]), 16);
                    if (!rd) {
                        ctx.exception = CpuException::GeneralProtection;
                        return ExecResult::Exception;
                    }
                }
            } else if (mask & 4) {
                ctx.cpu.avx.clear();
            }
            return ExecResult::Continue;
        }
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

}  // namespace vitreo::cpu::ops
