// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Shared interpreter helpers implementation

#include "helpers.hpp"

#include <cstdio>   // fprintf
#include <cstdlib>  // getenv

namespace vitreo::cpu::ops {

Result<u64, Error> calc_effective_address(
    ExecContext& ctx,
    const MemoryOperand& mem
) noexcept {
    // Intel SDM Vol. 2, MOV: A0-A3 use a full 64-bit absolute memory offset
    // in 64-bit address mode. It is not a sign-extended ModR/M displacement.
    u64 addr = mem.has_absolute ? mem.absolute : 0;
    const bool is_32bit = !ctx.cpu.is_64bit();
    
    // Base register - use 32-bit read in 32-bit mode per Intel spec
    if (mem.has_base()) {
        addr += is_32bit ? ctx.cpu.gpr.get32(mem.base)
                         : ctx.cpu.gpr.get64(mem.base);
    }
    
    // RIP-relative addressing: displacement is relative to end of instruction
    if (mem.is_rip_relative()) {
        addr += ctx.cpu.rip + ctx.instr_length;
    }
    
    // Index register with scale - use 32-bit read in 32-bit mode per Intel spec
    // In 32-bit mode: effective_addr = (u32)(base + index*scale + disp)
    if (mem.has_index()) {
        u64 index_val = is_32bit ? ctx.cpu.gpr.get32(mem.index)
                                 : ctx.cpu.gpr.get64(mem.index);
        addr += index_val * mem.scale;
    }
    
    // Displacement - sign-extended to 64-bit, then add
    addr += static_cast<u64>(static_cast<i64>(mem.disp));

    // Intel SDM Vol. 1 §§3.3.7/3.7.5: in long mode, truncate the
    // effective offset to address size BEFORE adding the full FS/GS base.
    // CS/DS/ES/SS bases are ignored. Preserve the existing compatibility-mode
    // segment-and-final-truncation contract independently.
    if (!is_32bit) {
        if (ctx.address_size_override) addr = static_cast<u32>(addr);
        if (mem.segment == SegReg::FS || mem.segment == SegReg::GS) {
            addr += ctx.cpu.seg.get_desc(
                static_cast<SegIndex>(static_cast<u8>(mem.segment))).base;
        }
    } else {
        if (mem.segment != SegReg::Default) {
            addr += ctx.cpu.seg.get_desc(
                static_cast<SegIndex>(static_cast<u8>(mem.segment))).base;
        }
        addr = static_cast<u32>(addr);
    }

    return Result<u64, Error>::ok(addr);
}

Result<u64, Error> maskmov_ds_edi_address(
    ExecContext& ctx,
    const DecodedInstruction& instr
) noexcept {
    constexpr u8 rdi = static_cast<u8>(RegIndex::RDI);

    u64 offset = 0;
    if (ctx.cpu.is_long_mode()) {
        offset = instr.prefixes.address_size
            ? static_cast<u64>(ctx.cpu.gpr.get32(rdi))
            : ctx.cpu.gpr.get64(rdi);
    } else if (ctx.cpu.is_real_mode()) {
        offset = instr.prefixes.address_size
            ? static_cast<u64>(ctx.cpu.gpr.get32(rdi))
            : static_cast<u64>(ctx.cpu.gpr.get16(rdi));
    } else {
        offset = instr.prefixes.address_size
            ? static_cast<u64>(ctx.cpu.gpr.get16(rdi))
            : static_cast<u64>(ctx.cpu.gpr.get32(rdi));
    }

    if (!ctx.cpu.is_long_mode()) {
        if ((ctx.cpu.is_real_mode() && !instr.prefixes.address_size) ||
            (ctx.cpu.is_protected_mode() && instr.prefixes.address_size)) {
            offset = static_cast<u16>(offset);
        } else {
            offset = static_cast<u32>(offset);
        }
    } else if (instr.prefixes.address_size) {
        offset = static_cast<u32>(offset);
    }

    const SegReg segment =
        instr.prefixes.segment == SegReg::Default
            ? SegReg::DS
            : instr.prefixes.segment;
    const auto segment_index =
        static_cast<SegIndex>(static_cast<u8>(segment));
    if (!ctx.cpu.is_long_mode() ||
        segment == SegReg::FS ||
        segment == SegReg::GS) {
        offset += ctx.cpu.seg.get_desc(segment_index).base;
    }
    return Result<u64, Error>::ok(offset);
}

Result<u64, Error> read_memory(
    ExecContext& ctx,
    u64 addr,
    u8 size
) noexcept {
    switch (size) {
        case 1: {
            auto r = ctx.mem.read_u8(addr);
            if (!r) return Result<u64, Error>::err(r.error());
            return Result<u64, Error>::ok(r.value());
        }
        case 2: {
            auto r = ctx.mem.read_u16(addr);
            if (!r) return Result<u64, Error>::err(r.error());
            return Result<u64, Error>::ok(r.value());
        }
        case 4: {
            auto r = ctx.mem.read_u32(addr);
            if (!r) return Result<u64, Error>::err(r.error());
            return Result<u64, Error>::ok(r.value());
        }
        case 8: {
            auto r = ctx.mem.read_u64(addr);
            if (!r) return Result<u64, Error>::err(r.error());
            return Result<u64, Error>::ok(r.value());
        }
        default:
            return Result<u64, Error>::err(Error::InvalidArgument);
    }
}

VoidResult write_memory(
    ExecContext& ctx,
    u64 addr,
    u64 value,
    u8 size
) noexcept {
    switch (size) {
        case 1: {
            auto r = ctx.mem.write_u8(addr, static_cast<u8>(value));
            if (!r) return VoidResult::err(r.error());
            return VoidResult::ok();
        }
        case 2: {
            auto r = ctx.mem.write_u16(addr, static_cast<u16>(value));
            if (!r) return VoidResult::err(r.error());
            return VoidResult::ok();
        }
        case 4: {
            auto r = ctx.mem.write_u32(addr, static_cast<u32>(value));
            if (!r) return VoidResult::err(r.error());
            return VoidResult::ok();
        }
        case 8: {
            auto r = ctx.mem.write_u64(addr, value);
            if (!r) return VoidResult::err(r.error());
            return VoidResult::ok();
        }
        default:
            return VoidResult::err(Error::InvalidArgument);
    }
}

u64 read_register(
    ExecContext& ctx,
    OperandType type,
    u8 reg
) noexcept {
    switch (type) {
        case OperandType::Reg8:
            // All Reg8 indices here are low-byte registers (0-15).
            // Legacy high-byte registers (AH/CH/DH/BH) are handled
            // upstream in read_operand() using prefix info.
            return ctx.cpu.gpr.get8l(reg);
        case OperandType::Reg16:
            return ctx.cpu.gpr.get16(reg);
        case OperandType::Reg32:
            return ctx.cpu.gpr.get32(reg);
        case OperandType::Reg64:
            return ctx.cpu.gpr.get64(reg);
        case OperandType::RegSeg:
            return ctx.cpu.seg.get(static_cast<SegIndex>(reg));
        default:
            return 0;
    }
}

void write_register(
    ExecContext& ctx,
    OperandType type,
    u8 reg,
    u64 value
) noexcept {
    switch (type) {
        case OperandType::Reg8:
            // All Reg8 indices here are low-byte registers (0-15).
            // Legacy high-byte registers (AH/CH/DH/BH) are handled
            // upstream in write_operand() using prefix info.
            ctx.cpu.gpr.set8l(reg, static_cast<u8>(value));
            break;
        case OperandType::Reg16:
            ctx.cpu.gpr.set16(reg, static_cast<u16>(value));
            break;
        case OperandType::Reg32:
            // 32-bit write clears upper 32 bits in 64-bit mode
            ctx.cpu.gpr.set32(reg, static_cast<u32>(value));
            break;
        case OperandType::Reg64:
            ctx.cpu.gpr.set64(reg, value);
            break;
        case OperandType::RegSeg:
            ctx.cpu.seg.set(static_cast<SegIndex>(reg), static_cast<u16>(value));
            break;
        default:
            break;
    }
}

} // namespace vitreo::cpu::ops
