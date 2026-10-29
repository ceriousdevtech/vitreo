// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Stack instruction execution (PUSH, POP)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

ExecResult exec_push(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto val = read_operand(ctx, instr, 0);
    if (!val) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size;
    u64 push_val = val.value();

    const bool segment_push =
        instr.opcode == Opcode::PUSH_CS || instr.opcode == Opcode::PUSH_SS ||
        instr.opcode == Opcode::PUSH_DS || instr.opcode == Opcode::PUSH_ES ||
        instr.opcode == Opcode::PUSH_FS || instr.opcode == Opcode::PUSH_GS;
    if (segment_push) {
        // Intel SDM Vol. 2 PUSH: operand size selects the stack slot (2/4/8).
        // 32-bit default is 4; 66H → 2. 64-bit default is 8 (FS/GS only;
        // CS/SS/DS/ES are #UD). A 16-bit selector is stored; 64-bit slots
        // write the zero-extended selector. 32-bit slots use a 16-bit move.
        u8 slot_size = 4;
        if (instr.prefixes.operand_size) {
            slot_size = 2;
        } else if (ctx.cpu.is_64bit()) {
            slot_size = 8;
        }
        const u8 store_size = (slot_size == 8) ? 8 : 2;
        const u64 stack_address = ctx.cpu.sp() - slot_size;
        auto result = write_memory(ctx, stack_address, push_val, store_size);
        if (!result) {
            // A faulting push commits nothing; the slot is the write's
            // address (stack_access_fault_probe.expected).
            record_write_access_violation(ctx, stack_address, store_size);
            ctx.exception = CpuException::PageFault;
            return ExecResult::Exception;
        }
        ctx.cpu.set_sp(stack_address);
        return ExecResult::Continue;
    }
    
    switch (instr.opcode) {
        case Opcode::PUSH_R16:
        case Opcode::PUSH_RM16:
            // Intel SDM Vol. 2 PUSH: a 66H operand-size override selects a
            // 16-bit operand in EVERY mode, 64-bit included — the stack
            // pointer moves by 2 and exactly two bytes are written. There is
            // no promotion to the 64-bit default slot; promoting it drifts
            // RSP by 6 per instruction.
            size = 2;
            break;
        case Opcode::PUSH_R32:
        case Opcode::PUSH_RM32:
            size = ctx.cpu.is_64bit() ? 8 : 4;
            break;
        case Opcode::PUSH_R64:
        case Opcode::PUSH_RM64:
            size = 8;
            break;
        case Opcode::PUSH_IMM8:
        case Opcode::PUSH_IMM16:
        case Opcode::PUSH_IMM32: {
            // Sign-extend immediate values
            size = ctx.cpu.is_64bit() ? 8 : 4;
            i64 signed_val = static_cast<i64>(static_cast<i32>(val.value()));
            push_val = static_cast<u64>(signed_val);
            break;
        }
        default:
            size = ctx.cpu.is_64bit() ? 8 : 4;
            break;
    }
    // The override also covers the immediate forms (66 68 iw / 66 6A ib),
    // whose opcode alone does not carry the operand size.
    if (instr.prefixes.operand_size) size = 2;

    auto r = push_value(ctx, push_val, size);
    if (!r) {
        ctx.exception = stack_access_exception(r.error());
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

ExecResult exec_pop(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    u8 size;
    
    switch (instr.opcode) {
        case Opcode::POP_R16:
        case Opcode::POP_RM16:
            // Intel SDM Vol. 2 POP: 66H selects a 16-bit operand in every
            // mode; the stack pointer moves by 2 and two bytes are read.
            size = 2;
            break;
        case Opcode::POP_R32:
        case Opcode::POP_RM32:
            size = ctx.cpu.is_64bit() ? 8 : 4;
            break;
        case Opcode::POP_R64:
        case Opcode::POP_RM64:
            size = 8;
            break;
        case Opcode::POP_DS:
        case Opcode::POP_ES:
        case Opcode::POP_SS:
        case Opcode::POP_FS:
        case Opcode::POP_GS:
            // Intel SDM Vol. 2 POP: stack increment follows operand size.
            // 32-bit default 4; 66H → 2. 64-bit FS/GS default 8 (DS/ES/SS
            // are #UD). write_operand(RegSeg) keeps the low 16 bits.
            if (instr.prefixes.operand_size) {
                size = 2;
            } else if (ctx.cpu.is_64bit()) {
                size = 8;
            } else {
                size = 4;
            }
            break;
        default:
            size = ctx.cpu.is_64bit() ? 8 : 4;
            break;
    }
    if (instr.prefixes.operand_size && instr.opcode != Opcode::POP_DS &&
        instr.opcode != Opcode::POP_ES && instr.opcode != Opcode::POP_SS &&
        instr.opcode != Opcode::POP_FS && instr.opcode != Opcode::POP_GS) {
        size = 2;
    }

    // Intel SDM Vol. 2 POP: a memory destination's EA is computed with the
    // stack pointer already incremented, so pop_value commits it before the
    // write. A destination write that faults commits nothing, the stack
    // pointer included: restore it (write_operand recorded the write AV).
    const u64 sp_before = ctx.cpu.sp();
    auto val = pop_value(ctx, size);
    if (!val) {
        ctx.exception = stack_access_exception(val.error());
        return ExecResult::Exception;
    }

    auto r = write_operand(ctx, instr, 0, val.value());
    if (!r) {
        ctx.cpu.set_sp(sp_before);
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
