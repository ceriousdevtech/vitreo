// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Indirect control flow instruction execution (CALL r/m, JMP r/m, LEAVE, ENTER)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

ExecResult exec_call_indirect(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Read absolute target address from operand 0 (register or memory)
    auto target = read_operand(ctx, instr, 0);
    if (!target) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // Intel SDM CALL: a non-canonical target is #GP(0) before the push.
    if (!near_branch_target_canonical(ctx, target.value())) {
        return ExecResult::Exception;
    }

    // Calculate return address (address of next instruction)
    u64 return_addr = ctx.cpu.rip + instr.length;
    u8 stack_size = ctx.cpu.is_64bit() ? 8 : 4;

    // Push return address onto stack
    auto r = push_value(ctx, return_addr, stack_size);
    if (!r) {
        ctx.exception = stack_access_exception(r.error());
        return ExecResult::Exception;
    }

    // Jump to the target (absolute address)
    ctx.cpu.rip = target.value();
    return ExecResult::Branch;
}

ExecResult exec_jmp_indirect(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Read absolute target address from operand 0 (register or memory)
    auto target = read_operand(ctx, instr, 0);
    if (!target) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    if (!near_branch_target_canonical(ctx, target.value())) {
        return ExecResult::Exception;
    }
    // Jump to the target (absolute address, no push)
    ctx.cpu.rip = target.value();
    return ExecResult::Branch;
}

ExecResult exec_leave(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    (void)instr;

    // Intel SDM Vol. 2 LEAVE: RSP := RBP; RBP := Pop(). The two steps are
    // ONE instruction: when the pop faults, Windows on Intel reports the
    // starting RSP (tools/oracle_probe/stack_access_fault_probe.expected,
    // `leave` ctxsp=+0), so the slot is read before anything commits.
    const u8 stack_size = ctx.cpu.is_64bit() ? 8 : 4;
    const u64 frame = ctx.cpu.is_64bit()
        ? ctx.cpu.gpr.rbp
        : ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RBP));
    auto val = read_memory(ctx, frame, stack_size);
    if (!val) {
        record_read_access_violation(ctx, frame, stack_size);
        ctx.exception = CpuException::PageFault;
        return ExecResult::Exception;
    }
    ctx.cpu.set_sp(frame + stack_size);

    if (ctx.cpu.is_64bit()) {
        ctx.cpu.gpr.rbp = val.value();
    } else {
        ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RBP), static_cast<u32>(val.value()));
    }

    return ExecResult::Continue;
}

ExecResult exec_enter(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Intel SDM Vol. 2 ENTER: AllocSize := imm16; NestingLevel := imm8 MOD 32.
    auto frame_size_r = read_operand(ctx, instr, 0);
    if (!frame_size_r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    const u16 frame_size = static_cast<u16>(frame_size_r.value());

    auto nesting_r = read_operand(ctx, instr, 1);
    if (!nesting_r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    const u8 nesting = static_cast<u8>(nesting_r.value()) & 0x1Fu;

    // Default operand size matches stack address size in our user-mode paths
    // (32-bit → 4, 64-bit → 8). 66H 16-bit ENTER is not modeled separately.
    const u8 stack_size = ctx.cpu.is_64bit() ? 8 : 4;
    const u8 bp_reg = static_cast<u8>(RegIndex::RBP);

    auto read_bp = [&]() -> u64 {
        return ctx.cpu.is_64bit() ? ctx.cpu.gpr.rbp : ctx.cpu.gpr.get32(bp_reg);
    };
    auto write_bp = [&](u64 value) {
        if (ctx.cpu.is_64bit()) {
            ctx.cpu.gpr.rbp = value;
        } else {
            ctx.cpu.gpr.set32(bp_reg, static_cast<u32>(value));
        }
    };

    // Push(BP/EBP/RBP); FrameTemp := SP
    auto r = push_value(ctx, read_bp(), stack_size);
    if (!r) {
        ctx.exception = stack_access_exception(r.error());
        return ExecResult::Exception;
    }
    const u64 frame_temp = ctx.cpu.sp();

    if (nesting > 0) {
        if (nesting > 1) {
            for (u8 i = 1; i < nesting; ++i) {
                write_bp(read_bp() - stack_size);
                auto display = read_memory(ctx, read_bp(), stack_size);
                if (!display) {
                    record_read_access_violation(ctx, read_bp(), stack_size);
                    ctx.exception = CpuException::PageFault;
                    return ExecResult::Exception;
                }
                r = push_value(ctx, display.value(), stack_size);
                if (!r) {
                    ctx.exception = stack_access_exception(r.error());
                    return ExecResult::Exception;
                }
            }
        }
        // Push(FrameTemp) — nesting level 1..31
        r = push_value(ctx, frame_temp, stack_size);
        if (!r) {
            ctx.exception = stack_access_exception(r.error());
            return ExecResult::Exception;
        }
    }

    write_bp(frame_temp);
    ctx.cpu.set_sp(ctx.cpu.sp() - frame_size);
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
