// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DAA/DAS/AAA/AAS/AAM/AAD, BOUND, LES/LDS/LSS/LFS/LGS (Intel SDM Vol. 2)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

void set_szp8(ExecContext& ctx, u8 result) noexcept {
    ctx.cpu.flags.set_sf((result & 0x80) != 0);
    ctx.cpu.flags.set_zf(result == 0);
    ctx.cpu.flags.set_pf(Flags::compute_parity(result));
}

ExecResult load_far_ptr(ExecContext& ctx, const DecodedInstruction& instr,
                        SegIndex seg, u8 off_bytes) noexcept {
    auto addr = calc_effective_address(ctx, instr.operands[1].mem);
    if (!addr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    auto off = read_memory(ctx, addr.value(), off_bytes);
    auto sel = read_memory(ctx, addr.value() + off_bytes, 2);
    if (!off || !sel) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    auto wr = write_operand(ctx, instr, 0, off.value());
    if (!wr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    ctx.cpu.seg.set(seg, static_cast<u16>(sel.value()));
    return ExecResult::Continue;
}

}  // namespace

ExecResult exec_legacy_usermode(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
        case Opcode::DAA: {
            // Intel SDM Vol. 2 DAA. #UD in 64-bit is enforced by the decoder.
            u8 al = ctx.cpu.gpr.get8l(0);
            const u8 old_al = al;
            const bool old_cf = ctx.cpu.flags.cf();
            ctx.cpu.flags.set_cf(false);
            if (((al & 0x0F) > 9) || ctx.cpu.flags.af()) {
                const u16 sum = static_cast<u16>(al) + 6;
                al = static_cast<u8>(sum);
                ctx.cpu.flags.set_cf(old_cf || (sum > 0xFF));
                ctx.cpu.flags.set_af(true);
            } else {
                ctx.cpu.flags.set_af(false);
            }
            if (old_al > 0x99 || old_cf) {
                al = static_cast<u8>(al + 0x60);
                ctx.cpu.flags.set_cf(true);
            } else {
                ctx.cpu.flags.set_cf(false);
            }
            ctx.cpu.gpr.set8l(0, al);
            set_szp8(ctx, al);
            return ExecResult::Continue;
        }
        case Opcode::DAS: {
            u8 al = ctx.cpu.gpr.get8l(0);
            const u8 old_al = al;
            const bool old_cf = ctx.cpu.flags.cf();
            ctx.cpu.flags.set_cf(false);
            if (((al & 0x0F) > 9) || ctx.cpu.flags.af()) {
                const u16 diff = static_cast<u16>(al) - 6;
                al = static_cast<u8>(diff);
                ctx.cpu.flags.set_cf(old_cf || (old_al < 6));
                ctx.cpu.flags.set_af(true);
            } else {
                ctx.cpu.flags.set_af(false);
            }
            if (old_al > 0x99 || old_cf) {
                al = static_cast<u8>(al - 0x60);
                ctx.cpu.flags.set_cf(true);
            } else {
                // Intel SDM Vol. 2 DAS: second IF else clears CF even if the
                // low-nibble adjust set it (old_AL<=99H and old_CF=0).
                ctx.cpu.flags.set_cf(false);
            }
            ctx.cpu.gpr.set8l(0, al);
            set_szp8(ctx, al);
            return ExecResult::Continue;
        }
        case Opcode::AAA: {
            u16 ax = ctx.cpu.gpr.get16(0);
            u8 al = static_cast<u8>(ax);
            u8 ah = static_cast<u8>(ax >> 8);
            if (((al & 0x0F) > 9) || ctx.cpu.flags.af()) {
                al = static_cast<u8>(al + 6);
                ah = static_cast<u8>(ah + 1);
                ctx.cpu.flags.set_af(true);
                ctx.cpu.flags.set_cf(true);
            } else {
                ctx.cpu.flags.set_af(false);
                ctx.cpu.flags.set_cf(false);
            }
            al &= 0x0F;
            ctx.cpu.gpr.set16(0, static_cast<u16>((static_cast<u16>(ah) << 8) | al));
            return ExecResult::Continue;
        }
        case Opcode::AAS: {
            u16 ax = ctx.cpu.gpr.get16(0);
            u8 al = static_cast<u8>(ax);
            u8 ah = static_cast<u8>(ax >> 8);
            if (((al & 0x0F) > 9) || ctx.cpu.flags.af()) {
                al = static_cast<u8>(al - 6);
                ah = static_cast<u8>(ah - 1);
                ctx.cpu.flags.set_af(true);
                ctx.cpu.flags.set_cf(true);
            } else {
                ctx.cpu.flags.set_af(false);
                ctx.cpu.flags.set_cf(false);
            }
            al &= 0x0F;
            ctx.cpu.gpr.set16(0, static_cast<u16>((static_cast<u16>(ah) << 8) | al));
            return ExecResult::Continue;
        }
        case Opcode::AAM: {
            const u8 base = static_cast<u8>(instr.operands[0].imm);
            if (base == 0) {
                ctx.exception = CpuException::DivideError;
                return ExecResult::Exception;
            }
            const u8 al = ctx.cpu.gpr.get8l(0);
            const u8 ah = static_cast<u8>(al / base);
            const u8 nal = static_cast<u8>(al % base);
            ctx.cpu.gpr.set16(0, static_cast<u16>((static_cast<u16>(ah) << 8) | nal));
            set_szp8(ctx, nal);
            return ExecResult::Continue;
        }
        case Opcode::AAD: {
            const u8 base = static_cast<u8>(instr.operands[0].imm);
            const u16 ax = ctx.cpu.gpr.get16(0);
            const u8 al = static_cast<u8>(
                static_cast<u8>(ax) + static_cast<u8>(ax >> 8) * base);
            ctx.cpu.gpr.set16(0, al);
            set_szp8(ctx, al);
            return ExecResult::Continue;
        }
        case Opcode::BOUND_R16_M32:
        case Opcode::BOUND_R32_M64: {
            auto addr = calc_effective_address(ctx, instr.operands[1].mem);
            if (!addr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            const bool word = instr.opcode == Opcode::BOUND_R16_M32;
            if (word) {
                auto lo = read_memory(ctx, addr.value(), 2);
                auto hi = read_memory(ctx, addr.value() + 2, 2);
                if (!lo || !hi) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                const i16 idx = static_cast<i16>(ctx.cpu.gpr.get16(instr.operands[0].reg));
                const i16 lower = static_cast<i16>(lo.value());
                const i16 upper = static_cast<i16>(hi.value());
                if (idx < lower || idx > upper) {
                    ctx.exception = CpuException::BoundRange;
                    return ExecResult::Exception;
                }
            } else {
                auto lo = read_memory(ctx, addr.value(), 4);
                auto hi = read_memory(ctx, addr.value() + 4, 4);
                if (!lo || !hi) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                const i32 idx = static_cast<i32>(ctx.cpu.gpr.get32(instr.operands[0].reg));
                const i32 lower = static_cast<i32>(lo.value());
                const i32 upper = static_cast<i32>(hi.value());
                if (idx < lower || idx > upper) {
                    ctx.exception = CpuException::BoundRange;
                    return ExecResult::Exception;
                }
            }
            return ExecResult::Continue;
        }
        case Opcode::LES_R16_M32:
        case Opcode::LES_R32_M48:
            return load_far_ptr(ctx, instr, SegIndex::ES,
                instr.opcode == Opcode::LES_R16_M32 ? 2 : 4);
        case Opcode::LDS_R16_M32:
        case Opcode::LDS_R32_M48:
            return load_far_ptr(ctx, instr, SegIndex::DS,
                instr.opcode == Opcode::LDS_R16_M32 ? 2 : 4);
        case Opcode::LSS_R16_M32:
            return load_far_ptr(ctx, instr, SegIndex::SS, 2);
        case Opcode::LSS_R32_M48:
            return load_far_ptr(ctx, instr, SegIndex::SS, 4);
        case Opcode::LSS_R64_M80:
            return load_far_ptr(ctx, instr, SegIndex::SS, 8);
        case Opcode::LFS_R16_M32:
            return load_far_ptr(ctx, instr, SegIndex::FS, 2);
        case Opcode::LFS_R32_M48:
            return load_far_ptr(ctx, instr, SegIndex::FS, 4);
        case Opcode::LFS_R64_M80:
            return load_far_ptr(ctx, instr, SegIndex::FS, 8);
        case Opcode::LGS_R16_M32:
            return load_far_ptr(ctx, instr, SegIndex::GS, 2);
        case Opcode::LGS_R32_M48:
            return load_far_ptr(ctx, instr, SegIndex::GS, 4);
        case Opcode::LGS_R64_M80:
            return load_far_ptr(ctx, instr, SegIndex::GS, 8);
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

}  // namespace vitreo::cpu::ops
