// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE data transfer instruction execution (CPU-031)
//
// Integration checklist (modifications to existing files):
//
// 1. exec_ops.hpp — add declaration:
//      [[nodiscard]] ExecResult exec_sse_data(ExecContext& ctx,
//          const DecodedInstruction& instr) noexcept;
//
// 2. interpreter.cpp — add cases in execute_instruction switch:
//      case Opcode::MOVAPS_XMM_XMMM128: case Opcode::MOVAPS_XMMM128_XMM:
//      case Opcode::MOVUPS_XMM_XMMM128: case Opcode::MOVUPS_XMMM128_XMM:
//      case Opcode::MOVAPD_XMM_XMMM128: case Opcode::MOVAPD_XMMM128_XMM:
//      case Opcode::MOVUPD_XMM_XMMM128: case Opcode::MOVUPD_XMMM128_XMM:
//      case Opcode::MOVDQA_XMM_XMMM128: case Opcode::MOVDQA_XMMM128_XMM:
//      case Opcode::MOVDQU_XMM_XMMM128: case Opcode::MOVDQU_XMMM128_XMM:
//      case Opcode::MOVSS_XMM_XMMM32:   case Opcode::MOVSS_XMMM32_XMM:
//      case Opcode::MOVSD_XMM_XMMM64:   case Opcode::MOVSD_XMMM64_XMM:
//      case Opcode::MOVLPS_XMM_M64:     case Opcode::MOVLPS_M64_XMM:
//      case Opcode::MOVHPS_XMM_M64:     case Opcode::MOVHPS_M64_XMM:
//      case Opcode::MOVLPD_XMM_M64:     case Opcode::MOVLPD_M64_XMM:
//      case Opcode::MOVHPD_XMM_M64:     case Opcode::MOVHPD_M64_XMM:
//      case Opcode::MOVMSKPS_R32_XMM:   case Opcode::MOVMSKPD_R32_XMM:
//      case Opcode::XORPS_XMM_XMMM128:  case Opcode::XORPD_XMM_XMMM128:
//      case Opcode::ANDPS_XMM_XMMM128:  case Opcode::ANDPD_XMM_XMMM128:
//      case Opcode::ORPS_XMM_XMMM128:   case Opcode::ORPD_XMM_XMMM128:
//      case Opcode::ANDNPS_XMM_XMMM128: case Opcode::ANDNPD_XMM_XMMM128:
//      case Opcode::LDMXCSR_M32:        case Opcode::STMXCSR_M32:
//          return exec_sse_data(ctx, instr);
//
// 3. src/CMakeLists.txt — add to vitreo_cpu_interpreter sources:
//      cpu/interpreter/ops/sse_data.cpp

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "core/cpu/state/fx_image.hpp"

#include <cstring>

#include "core/math/host_fenv.hpp"

namespace vitreo::cpu::ops {

namespace {

/// Get reference to XMM register by index (masked to 0-15)
inline XmmReg& xmm(ExecContext& ctx, u8 idx) noexcept {
    return ctx.cpu.sse.xmm[idx & 15];
}

/// Read 128 bits from guest memory into an XmmReg
inline VoidResult read_xmm_mem(ExecContext& ctx, u64 addr, XmmReg& dst) noexcept {
    return ctx.mem.read_bytes(addr, dst.u8_, 16);
}

/// Write 128 bits from an XmmReg to guest memory
inline VoidResult write_xmm_mem(ExecContext& ctx, u64 addr, const XmmReg& src) noexcept {
    return ctx.mem.write_bytes(addr, src.u8_, 16);
}

/// Validate 16-byte alignment; sets GP exception and returns false on failure
inline bool require_align16(ExecContext& ctx, u64 addr) noexcept {
    if (addr & 0xF) {
        ctx.exception = CpuException::GeneralProtection;
        return false;
    }
    return true;
}

/// Generic GP-fault helper — sets exception and returns ExecResult::Exception
inline ExecResult gp_fault(ExecContext& ctx) noexcept {
    ctx.exception = CpuException::GeneralProtection;
    return ExecResult::Exception;
}

/// A rejected vector store: Windows reports a write of the first
/// inaccessible byte and leaves memory untouched
/// (tools/oracle_probe/simd_store_fault_probe.expected).
inline ExecResult store_fault(ExecContext& ctx, u64 addr, u8 size) noexcept {
    record_write_access_violation(ctx, addr, size);
    return gp_fault(ctx);
}

/// Load 128 bits from operand with 16-byte alignment requirement.
///
/// Every legacy SSE form with a 128-bit MEMORY operand requires it, not just
/// the MOVAP*/MOVDQA copies -- measured on silicon, not read off the manual:
/// `tools/oracle_probe/sse_align_probe.c` misaligns the operand by one and
/// reports which forms raise. On a real Alder Lake core XORPS, XORPD, PXOR,
/// ANDPS, PAND, ORPS and POR all fault exactly as MOVAPS and MOVDQA do, while
/// MOVUPS and MOVDQU do not. Its recorded output sits beside it.
///
/// This helper existed and was never called: the bitwise forms below used the
/// unchecked `load_xmm`, so a misaligned operand silently succeeded.
///
/// STILL OPEN: the integer-domain siblings in `sse_int.cpp` (PAND/POR/PXOR and
/// the rest) read through `read_ps`, which has 43 call sites and no alignment
/// check. Most of them require alignment too, but auditing which is a separate
/// change -- so the Tier 1 frontend declines their memory forms rather than
/// raising where this reference would not.
/// `aligned_form` is false for a VEX encoding, which carries NO 16-byte
/// alignment requirement (Intel SDM Vol. 2: unaligned unless the opcode says
/// otherwise). Defaulting it to true and letting the caller pass the real
/// value keeps the legacy spellings checked while VXORPS/VANDPS/VPXOR are not.
///
/// Raising here on a VEX form is not a harmless extra check -- it fabricates a
/// #GP in the middle of code the guest expects to run. The JIT had the same
/// defect on its own side of this instruction and it terminates a launcher at startup
/// deterministically, so this is the mirror of a bug that reached a user.
inline ExecResult load_xmm_aligned(ExecContext& ctx, const Operand& src,
                                   XmmReg& dst,
                                   bool aligned_form = true) noexcept {
    if (src.type == OperandType::Reg128) {
        dst = xmm(ctx, src.reg);
    } else {
        auto addr = calc_effective_address(ctx, src.mem);
        if (!addr) return gp_fault(ctx);
        if (aligned_form && !require_align16(ctx, addr.value()))
            return ExecResult::Exception;
        auto r = read_xmm_mem(ctx, addr.value(), dst);
        if (!r) return gp_fault(ctx);
    }
    return ExecResult::Continue;
}

} // anonymous namespace

ExecResult exec_sse_data(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // A VEX encoding carries no 16-byte alignment requirement. `ctx.vex_origin`
    // catches the forms the AVX bridge re-dispatches here with `prefixes.vex`
    // already cleared.
    const bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;
    switch (instr.opcode) {

    // =====================================================================
    // MOVAPS / MOVAPD  (aligned load: xmm ← xmm/m128)
    // =====================================================================
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVAPD_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        if (src.type == OperandType::Reg128) {
            xmm(ctx, dst) = xmm(ctx, src.reg);
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            if (!require_align16(ctx, addr.value())) return ExecResult::Exception;
            auto r = read_xmm_mem(ctx, addr.value(), xmm(ctx, dst));
            if (!r) return gp_fault(ctx);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVAPS / MOVAPD  (aligned store: xmm/m128 ← xmm)
    // MOVNTPS / MOVNTPD — same store; non-temporal hint ignored (SDM Vol. 2).
    // Decoder already requires memory destination for MOVNT*.
    // =====================================================================
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVAPD_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
    case Opcode::MOVNTDQ_M128_XMM: {
        const auto& dst = instr.operands[0];
        u8 src = instr.operands[1].reg;
        if (dst.type == OperandType::Reg128) {
            xmm(ctx, dst.reg) = xmm(ctx, src);
        } else {
            auto addr = calc_effective_address(ctx, dst.mem);
            if (!addr) return gp_fault(ctx);
            if (!require_align16(ctx, addr.value())) return ExecResult::Exception;
            auto r = write_xmm_mem(ctx, addr.value(), xmm(ctx, src));
            if (!r) return store_fault(ctx, addr.value(), 16);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVUPS / MOVUPD  (unaligned load: xmm ← xmm/m128)
    // =====================================================================
    case Opcode::MOVUPS_XMM_XMMM128:
    case Opcode::MOVUPD_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        if (src.type == OperandType::Reg128) {
            xmm(ctx, dst) = xmm(ctx, src.reg);
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            auto r = read_xmm_mem(ctx, addr.value(), xmm(ctx, dst));
            if (!r) return gp_fault(ctx);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVUPS / MOVUPD  (unaligned store: xmm/m128 ← xmm)
    // =====================================================================
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVUPD_XMMM128_XMM: {
        const auto& dst = instr.operands[0];
        u8 src = instr.operands[1].reg;
        if (dst.type == OperandType::Reg128) {
            xmm(ctx, dst.reg) = xmm(ctx, src);
        } else {
            auto addr = calc_effective_address(ctx, dst.mem);
            if (!addr) return gp_fault(ctx);
            auto r = write_xmm_mem(ctx, addr.value(), xmm(ctx, src));
            if (!r) return store_fault(ctx, addr.value(), 16);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVDQA  (aligned integer load: xmm ← xmm/m128)
    // =====================================================================
    case Opcode::MOVDQA_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        if (src.type == OperandType::Reg128) {
            xmm(ctx, dst) = xmm(ctx, src.reg);
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            if (!require_align16(ctx, addr.value())) return ExecResult::Exception;
            auto r = read_xmm_mem(ctx, addr.value(), xmm(ctx, dst));
            if (!r) return gp_fault(ctx);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVDQA  (aligned integer store: xmm/m128 ← xmm)
    // =====================================================================
    case Opcode::MOVDQA_XMMM128_XMM: {
        const auto& dst = instr.operands[0];
        u8 src = instr.operands[1].reg;
        if (dst.type == OperandType::Reg128) {
            xmm(ctx, dst.reg) = xmm(ctx, src);
        } else {
            auto addr = calc_effective_address(ctx, dst.mem);
            if (!addr) return gp_fault(ctx);
            if (!require_align16(ctx, addr.value())) return ExecResult::Exception;
            auto r = write_xmm_mem(ctx, addr.value(), xmm(ctx, src));
            if (!r) return store_fault(ctx, addr.value(), 16);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVDQU  (unaligned integer load: xmm ← xmm/m128)
    // LDDQU   (SSE3; SDM: same architectural result as MOVDQU from memory)
    // =====================================================================
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::LDDQU_XMM_M128: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        if (src.type == OperandType::Reg128) {
            // LDDQU is memory-only (#UD for reg form); decoder rejects that.
            xmm(ctx, dst) = xmm(ctx, src.reg);
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            auto r = read_xmm_mem(ctx, addr.value(), xmm(ctx, dst));
            if (!r) return gp_fault(ctx);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVDQU  (unaligned integer store: xmm/m128 ← xmm)
    // =====================================================================
    case Opcode::MOVDQU_XMMM128_XMM: {
        const auto& dst = instr.operands[0];
        u8 src = instr.operands[1].reg;
        if (dst.type == OperandType::Reg128) {
            xmm(ctx, dst.reg) = xmm(ctx, src);
        } else {
            auto addr = calc_effective_address(ctx, dst.mem);
            if (!addr) return gp_fault(ctx);
            auto r = write_xmm_mem(ctx, addr.value(), xmm(ctx, src));
            if (!r) return store_fault(ctx, addr.value(), 16);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVSS  (load: xmm ← xmm/m32)
    //   From memory: load 32 bits into [31:0], zero [127:32]
    //   From register: copy [31:0], preserve [127:32] of destination
    // =====================================================================
    case Opcode::MOVSS_XMM_XMMM32: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        if (src.type == OperandType::Reg128) {
            xmm(ctx, dst).u32_[0] = xmm(ctx, src.reg).u32_[0];
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            auto val = ctx.mem.read_u32(addr.value());
            if (!val) return gp_fault(ctx);
            auto& d = xmm(ctx, dst);
            d.clear();
            d.u32_[0] = val.value();
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVSS  (store: xmm/m32 ← xmm)
    //   To memory: write [31:0]
    //   To register: copy [31:0], preserve [127:32] of destination
    // =====================================================================
    case Opcode::MOVSS_XMMM32_XMM: {
        const auto& dst = instr.operands[0];
        u8 src = instr.operands[1].reg;
        if (dst.type == OperandType::Reg128) {
            xmm(ctx, dst.reg).u32_[0] = xmm(ctx, src).u32_[0];
        } else {
            auto addr = calc_effective_address(ctx, dst.mem);
            if (!addr) return gp_fault(ctx);
            auto r = ctx.mem.write_u32(addr.value(), xmm(ctx, src).u32_[0]);
            if (!r) return gp_fault(ctx);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVSD  (load: xmm ← xmm/m64)
    //   From memory: load 64 bits into [63:0], zero [127:64]
    //   From register: copy [63:0], preserve [127:64] of destination
    // =====================================================================
    case Opcode::MOVSD_XMM_XMMM64: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        if (src.type == OperandType::Reg128) {
            xmm(ctx, dst).u64_[0] = xmm(ctx, src.reg).u64_[0];
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            auto val = ctx.mem.read_u64(addr.value());
            if (!val) return gp_fault(ctx);
            auto& d = xmm(ctx, dst);
            d.clear();
            d.u64_[0] = val.value();
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVSD  (store: xmm/m64 ← xmm)
    // =====================================================================
    case Opcode::MOVSD_XMMM64_XMM: {
        const auto& dst = instr.operands[0];
        u8 src = instr.operands[1].reg;
        if (dst.type == OperandType::Reg128) {
            xmm(ctx, dst.reg).u64_[0] = xmm(ctx, src).u64_[0];
        } else {
            auto addr = calc_effective_address(ctx, dst.mem);
            if (!addr) return gp_fault(ctx);
            auto r = ctx.mem.write_u64(addr.value(), xmm(ctx, src).u64_[0]);
            if (!r) return gp_fault(ctx);
        }
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVLPS / MOVLPD  (load low: xmm[63:0] ← m64, preserve [127:64])
    // =====================================================================
    case Opcode::MOVLPS_XMM_M64:
    case Opcode::MOVLPD_XMM_M64: {
        u8 dst = instr.operands[0].reg;
        auto addr = calc_effective_address(ctx, instr.operands[1].mem);
        if (!addr) return gp_fault(ctx);
        auto val = ctx.mem.read_u64(addr.value());
        if (!val) return gp_fault(ctx);
        xmm(ctx, dst).u64_[0] = val.value();
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVLPS / MOVLPD  (store low: m64 ← xmm[63:0])
    // =====================================================================
    case Opcode::MOVLPS_M64_XMM:
    case Opcode::MOVLPD_M64_XMM: {
        u8 src = instr.operands[1].reg;
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        auto r = ctx.mem.write_u64(addr.value(), xmm(ctx, src).u64_[0]);
        if (!r) return gp_fault(ctx);
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVLHPS: dst[127:64] ← src[63:0], dst[63:0] unchanged
    // =====================================================================
    case Opcode::MOVLHPS_XMM_XMM: {
        u8 dst = instr.operands[0].reg;
        u8 src = instr.operands[1].reg;
        xmm(ctx, dst).u64_[1] = xmm(ctx, src).u64_[0];
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVHLPS: dst[63:0] ← src[127:64], dst[127:64] unchanged
    // =====================================================================
    case Opcode::MOVHLPS_XMM_XMM: {
        u8 dst = instr.operands[0].reg;
        u8 src = instr.operands[1].reg;
        xmm(ctx, dst).u64_[0] = xmm(ctx, src).u64_[1];
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVSLDUP (SSE3): dst[31:0]=src[31:0], dst[63:32]=src[31:0],
    //                   dst[95:64]=src[95:64], dst[127:96]=src[95:64]
    // =====================================================================
    case Opcode::MOVSLDUP_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        XmmReg s;
        if (src.type == OperandType::Reg128) {
            s = xmm(ctx, src.reg);
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            // Measured, not reasoned: `movsldup xmm0,[eax]` with the operand
            // misaligned by one raises #GP on real silicon
            // (tools/oracle_probe/sse_align_probe.expected). Both engines used
            // to read this unchecked and so agreed with each other while both
            // were wrong -- the failure mode parity testing cannot see.
            // A VEX encoding carries no alignment requirement.
            if (!instr.prefixes.vex && !ctx.vex_origin &&
                !require_align16(ctx, addr.value()))
                return ExecResult::Exception;
            auto r = read_xmm_mem(ctx, addr.value(), s);
            if (!r) return gp_fault(ctx);
        }
        auto& d = xmm(ctx, dst);
        d.u32_[0] = s.u32_[0];
        d.u32_[1] = s.u32_[0];
        d.u32_[2] = s.u32_[2];
        d.u32_[3] = s.u32_[2];
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVSHDUP (SSE3): duplicate high 32-bit of each 64-bit half
    //   dst[31:0]=src[63:32], dst[63:32]=src[63:32],
    //   dst[95:64]=src[127:96], dst[127:96]=src[127:96]
    // =====================================================================
    case Opcode::MOVSHDUP_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        XmmReg s;
        if (src.type == OperandType::Reg128) {
            s = xmm(ctx, src.reg);
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            // Same silicon measurement as MOVSLDUP above: misaligned raises.
            if (!instr.prefixes.vex && !ctx.vex_origin &&
                !require_align16(ctx, addr.value()))
                return ExecResult::Exception;
            auto r = read_xmm_mem(ctx, addr.value(), s);
            if (!r) return gp_fault(ctx);
        }
        auto& d = xmm(ctx, dst);
        d.u32_[0] = s.u32_[1];
        d.u32_[1] = s.u32_[1];
        d.u32_[2] = s.u32_[3];
        d.u32_[3] = s.u32_[3];
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVDDUP (SSE3): dst[63:0] = src[63:0]; dst[127:64] = src[63:0]
    // =====================================================================
    case Opcode::MOVDDUP_XMM_XMMM64: {
        u8 dst = instr.operands[0].reg;
        const auto& src = instr.operands[1];
        u64 lo;
        if (src.type == OperandType::Reg128) {
            lo = xmm(ctx, src.reg).u64_[0];
        } else {
            auto addr = calc_effective_address(ctx, src.mem);
            if (!addr) return gp_fault(ctx);
            auto val = ctx.mem.read_u64(addr.value());
            if (!val) return gp_fault(ctx);
            lo = val.value();
        }
        auto& d = xmm(ctx, dst);
        d.u64_[0] = lo;
        d.u64_[1] = lo;
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVHPS / MOVHPD  (load high: xmm[127:64] ← m64, preserve [63:0])
    // =====================================================================
    case Opcode::MOVHPS_XMM_M64:
    case Opcode::MOVHPD_XMM_M64: {
        u8 dst = instr.operands[0].reg;
        auto addr = calc_effective_address(ctx, instr.operands[1].mem);
        if (!addr) return gp_fault(ctx);
        auto val = ctx.mem.read_u64(addr.value());
        if (!val) return gp_fault(ctx);
        xmm(ctx, dst).u64_[1] = val.value();
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVHPS / MOVHPD  (store high: m64 ← xmm[127:64])
    // =====================================================================
    case Opcode::MOVHPS_M64_XMM:
    case Opcode::MOVHPD_M64_XMM: {
        u8 src = instr.operands[1].reg;
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        auto r = ctx.mem.write_u64(addr.value(), xmm(ctx, src).u64_[1]);
        if (!r) return gp_fault(ctx);
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVMSKPS: extract sign bits of 4 single-precision floats → r32
    // =====================================================================
    case Opcode::MOVMSKPS_R32_XMM: {
        u8 dst_gpr = instr.operands[0].reg;
        u8 src_xmm = instr.operands[1].reg;
        const auto& x = xmm(ctx, src_xmm);
        u32 mask = 0;
        for (u8 i = 0; i < 4; ++i) {
            if (x.u32_[i] & 0x80000000u) mask |= (1u << i);
        }
        write_register(ctx, OperandType::Reg32, dst_gpr, mask);
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVMSKPD: extract sign bits of 2 double-precision floats → r32
    // =====================================================================
    case Opcode::MOVMSKPD_R32_XMM: {
        u8 dst_gpr = instr.operands[0].reg;
        u8 src_xmm = instr.operands[1].reg;
        const auto& x = xmm(ctx, src_xmm);
        u32 mask = 0;
        for (u8 i = 0; i < 2; ++i) {
            if (x.u64_[i] & (1ULL << 63)) mask |= (1u << i);
        }
        write_register(ctx, OperandType::Reg32, dst_gpr, mask);
        return ExecResult::Continue;
    }

    // =====================================================================
    // XORPS / XORPD: bitwise XOR of all 128 bits
    // =====================================================================
    case Opcode::XORPS_XMM_XMMM128:
    case Opcode::XORPD_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        XmmReg src_val;
        auto res = load_xmm_aligned(ctx, instr.operands[1], src_val, aligned_form);
        if (res != ExecResult::Continue) return res;
        auto& d = xmm(ctx, dst);
        d.u64_[0] ^= src_val.u64_[0];
        d.u64_[1] ^= src_val.u64_[1];
        return ExecResult::Continue;
    }

    // =====================================================================
    // ANDPS / ANDPD: bitwise AND of all 128 bits
    // =====================================================================
    case Opcode::ANDPS_XMM_XMMM128:
    case Opcode::ANDPD_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        XmmReg src_val;
        auto res = load_xmm_aligned(ctx, instr.operands[1], src_val, aligned_form);
        if (res != ExecResult::Continue) return res;
        auto& d = xmm(ctx, dst);
        d.u64_[0] &= src_val.u64_[0];
        d.u64_[1] &= src_val.u64_[1];
        return ExecResult::Continue;
    }

    // =====================================================================
    // ORPS / ORPD: bitwise OR of all 128 bits
    // =====================================================================
    case Opcode::ORPS_XMM_XMMM128:
    case Opcode::ORPD_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        XmmReg src_val;
        auto res = load_xmm_aligned(ctx, instr.operands[1], src_val, aligned_form);
        if (res != ExecResult::Continue) return res;
        auto& d = xmm(ctx, dst);
        d.u64_[0] |= src_val.u64_[0];
        d.u64_[1] |= src_val.u64_[1];
        return ExecResult::Continue;
    }

    // =====================================================================
    // ANDNPS / ANDNPD: bitwise AND-NOT  (dst = ~dst & src)
    // =====================================================================
    case Opcode::ANDNPS_XMM_XMMM128:
    case Opcode::ANDNPD_XMM_XMMM128: {
        u8 dst = instr.operands[0].reg;
        XmmReg src_val;
        auto res = load_xmm_aligned(ctx, instr.operands[1], src_val, aligned_form);
        if (res != ExecResult::Continue) return res;
        auto& d = xmm(ctx, dst);
        d.u64_[0] = ~d.u64_[0] & src_val.u64_[0];
        d.u64_[1] = ~d.u64_[1] & src_val.u64_[1];
        return ExecResult::Continue;
    }

    // =====================================================================
    // FXSAVE: save x87, MMX, MXCSR, and XMM state to the 512-byte legacy
    // extended-state image defined by Intel SDM Vol. 2, FXSAVE.
    // =====================================================================
    case Opcode::FXSAVE_M512: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        if (!require_align16(ctx, addr.value())) return ExecResult::Exception;

        const bool fxsave64 = ctx.cpu.is_long_mode() && instr.prefixes.rex_w();
        const u8 xmm_count = ctx.cpu.is_long_mode() ? 16 : 8;
        u8 image[kFxImageBytes]{};
        fx_image_save(ctx.cpu, image, fxsave64, xmm_count);

        auto result = ctx.mem.write_bytes(addr.value(), image, sizeof(image));
        if (!result) return gp_fault(ctx);
        return ExecResult::Continue;
    }

    // =====================================================================
    // FXRSTOR: restore x87, MXCSR, and XMM state from the 512-byte image
    // defined by Intel SDM Vol. 2, FXRSTOR (inverse of FXSAVE).
    // =====================================================================
    case Opcode::FXRSTOR_M512: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        if (!require_align16(ctx, addr.value())) return ExecResult::Exception;

        u8 image[kFxImageBytes]{};
        auto loaded = ctx.mem.read_bytes(addr.value(), image, sizeof(image));
        if (!loaded) return gp_fault(ctx);

        const bool fxrstor64 = ctx.cpu.is_long_mode() && instr.prefixes.rex_w();
        const u8 xmm_count = ctx.cpu.is_long_mode() ? 16 : 8;
        fx_image_restore(ctx.cpu, image, fxrstor64, xmm_count);

        return ExecResult::Continue;
    }

    // =====================================================================
    // CLFLUSH: flush the cache line containing m8. Cache is a hint in
    // Vitreo; the address must still be valid (Intel SDM Vol. 2, CLFLUSH).
    // =====================================================================
    case Opcode::CLFLUSH_M8: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        auto probe = ctx.mem.read_u8(addr.value());
        if (!probe) return gp_fault(ctx);
        return ExecResult::Continue;
    }

    // =====================================================================
    // MOVNTI: store GPR to memory. Non-temporal hint is ignored.
    // Intel SDM Vol. 2, MOVNTI (0F C3).
    // =====================================================================
    case Opcode::MOVNTI_M32_R32: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        const u64 val = read_register(ctx, OperandType::Reg32, instr.operands[1].reg);
        auto r = write_memory(ctx, addr.value(), val, 4);
        if (!r) return gp_fault(ctx);
        return ExecResult::Continue;
    }
    case Opcode::MOVNTI_M64_R64: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        const u64 val = read_register(ctx, OperandType::Reg64, instr.operands[1].reg);
        auto r = write_memory(ctx, addr.value(), val, 8);
        if (!r) return gp_fault(ctx);
        return ExecResult::Continue;
    }

    // =====================================================================
    // LDMXCSR: load 32-bit MXCSR from memory
    // =====================================================================
    case Opcode::LDMXCSR_M32: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        auto val = ctx.mem.read_u32(addr.value());
        if (!val) return gp_fault(ctx);
        // LDMXCSR writes the sticky exception bits outright, so anything the
        // host accumulated for the SSE class before now is superseded and must
        // not resurface on the next STMXCSR.
        math::discard_sse_exceptions();
        // Apply mask to only set supported bits
        ctx.cpu.sse.mxcsr = val.value() & ctx.cpu.sse.mxcsr_mask;
        return ExecResult::Continue;
    }

    // =====================================================================
    // STMXCSR: store MXCSR to memory as 32-bit value
    // =====================================================================
    case Opcode::STMXCSR_M32: {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) return gp_fault(ctx);
        // Publish the sticky exception bits the host has accumulated for the
        // SSE class. They are computed lazily, exactly like the x87 status
        // word's: the host FPU raises them as a side effect of ordinary
        // arithmetic, and they are folded into architectural state only where
        // the guest can observe them. Without this MXCSR read back as its
        // reset value forever, so a guest asking "was that computation exact?"
        // saw a CPU on which nothing had ever rounded.
        ctx.cpu.sse.mxcsr |= math::harvest_sse_exceptions();
        auto r = ctx.mem.write_u32(addr.value(), ctx.cpu.sse.mxcsr);
        if (!r) return gp_fault(ctx);
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
