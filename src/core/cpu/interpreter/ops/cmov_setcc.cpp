// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CMOVcc/SETcc execution (CPU-021)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

/// Evaluate condition code. Returns true if condition is met.
bool evaluate_cc(u8 cc, const Flags& flags) noexcept {
    switch (cc) {
        case 0x0: return flags.of();                        // O
        case 0x1: return !flags.of();                       // NO
        case 0x2: return flags.cf();                        // B/C/NAE
        case 0x3: return !flags.cf();                       // NB/NC/AE
        case 0x4: return flags.zf();                        // Z/E
        case 0x5: return !flags.zf();                       // NZ/NE
        case 0x6: return flags.cf() || flags.zf();          // BE/NA
        case 0x7: return !flags.cf() && !flags.zf();        // NBE/A
        case 0x8: return flags.sf();                        // S
        case 0x9: return !flags.sf();                       // NS
        case 0xA: return flags.pf();                        // P/PE
        case 0xB: return !flags.pf();                       // NP/PO
        case 0xC: return flags.sf() != flags.of();          // L/NGE
        case 0xD: return flags.sf() == flags.of();          // NL/GE
        case 0xE: return flags.zf() || (flags.sf() != flags.of()); // LE/NG
        case 0xF: return !flags.zf() && (flags.sf() == flags.of()); // NLE/G
        default: return false;
    }
}

/// Map CMOVcc opcode to condition code (0x0-0xF).
/// CMOVcc opcodes are arranged in groups of 3 (16/32/64) per condition.
u8 cmov_opcode_to_cc(Opcode op) noexcept {
    // CMOVO_R16_RM16 is the first CMOVcc opcode.
    // Each condition has 3 variants (16/32/64), so cc = offset / 3.
    auto base = static_cast<u32>(Opcode::CMOVO_R16_RM16);
    auto val = static_cast<u32>(op);
    return static_cast<u8>((val - base) / 3);
}

/// Map SETcc opcode to condition code (0x0-0xF).
/// SETcc opcodes are arranged sequentially, one per condition.
u8 setcc_opcode_to_cc(Opcode op) noexcept {
    auto base = static_cast<u32>(Opcode::SETO_RM8);
    auto val = static_cast<u32>(op);
    return static_cast<u8>(val - base);
}

} // anonymous namespace

ExecResult exec_cmov(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    u8 cc = cmov_opcode_to_cc(instr.opcode);
    if (cc > 0xF) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }

    // See exec_jcc: a lazy flag op recorded by the JIT must be settled before
    // the architectural EFLAGS image can be read.
    ctx.cpu.materialize_lazy_flags();

    bool condition = evaluate_cc(cc, ctx.cpu.flags);

    if (condition) {
        auto src = read_operand(ctx, instr, 1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        auto r = write_operand(ctx, instr, 0, src.value());
        if (!r) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
    } else if (ctx.cpu.is_64bit()) {
        // Intel SDM Vol. 2 CMOVcc Operation: 32-bit dest in IA-32e mode
        // zero-extends DEST[63:32] even when the condition is false.
        auto base = static_cast<u32>(Opcode::CMOVO_R16_RM16);
        auto off = static_cast<u32>(instr.opcode) - base;
        if ((off % 3u) == 1u) {
            auto cur = read_operand(ctx, instr, 0);
            if (!cur) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto r = write_operand(ctx, instr, 0,
                                   static_cast<u32>(cur.value()));
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
    }

    // CMOVcc does not modify flags
    return ExecResult::Continue;
}

ExecResult exec_setcc(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    u8 cc = setcc_opcode_to_cc(instr.opcode);
    if (cc > 0xF) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }

    // See exec_jcc: a lazy flag op recorded by the JIT must be settled before
    // the architectural EFLAGS image can be read.
    ctx.cpu.materialize_lazy_flags();

    bool condition = evaluate_cc(cc, ctx.cpu.flags);

    auto r = write_operand(ctx, instr, 0, condition ? 1 : 0);
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // SETcc does not modify flags
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
