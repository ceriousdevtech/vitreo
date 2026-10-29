// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Control flow instruction execution (JMP, Jcc, CALL, RET)

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

ExecResult exec_jmp(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto offset = read_operand(ctx, instr, 0);
    if (!offset) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    i64 signed_offset = static_cast<i64>(offset.value());
    if (!near_branch_target_canonical(
            ctx, ctx.cpu.rip + instr.length + static_cast<u64>(signed_offset))) {
        return ExecResult::Exception;
    }
    ctx.cpu.advance_ip(instr.length);
    ctx.cpu.rip = static_cast<u64>(static_cast<i64>(ctx.cpu.rip) + signed_offset);
    return ExecResult::Branch;
}

ExecResult exec_jcc(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // The JIT records flag-producing ops lazily (CpuState::flag_op) and only
    // materializes them inside its own condition helpers. When execution leaves
    // a compiled block before the consumer runs — self-modifying guest code,
    // an interpreter fallback, a thunk return — the pending op is still live,
    // so reading flags.rflags here would observe the EFLAGS of some earlier
    // instruction instead of the immediately preceding CMP/TEST/SUB.
    ctx.cpu.materialize_lazy_flags();

    bool take_branch = false;
    
    switch (instr.opcode) {
        // JO - Jump if Overflow
        case Opcode::JO_REL8:
        case Opcode::JO_REL32:
            take_branch = ctx.cpu.flags.of();
            break;
        
        // JNO - Jump if Not Overflow
        case Opcode::JNO_REL8:
        case Opcode::JNO_REL32:
            take_branch = !ctx.cpu.flags.of();
            break;
        
        // JB/JC/JNAE - Jump if Below (CF=1)
        case Opcode::JB_REL8:
        case Opcode::JB_REL32:
            take_branch = ctx.cpu.flags.cf();
            break;
        
        // JNB/JNC/JAE - Jump if Not Below (CF=0)
        case Opcode::JNB_REL8:
        case Opcode::JNB_REL32:
            take_branch = !ctx.cpu.flags.cf();
            break;
        
        // JZ/JE - Jump if Zero
        case Opcode::JZ_REL8:
        case Opcode::JZ_REL32:
            take_branch = ctx.cpu.flags.zf();
            break;
        
        // JNZ/JNE - Jump if Not Zero
        case Opcode::JNZ_REL8:
        case Opcode::JNZ_REL32:
            take_branch = !ctx.cpu.flags.zf();
            break;
        
        // JBE/JNA - Jump if Below or Equal (CF=1 || ZF=1)
        case Opcode::JBE_REL8:
        case Opcode::JBE_REL32:
            take_branch = ctx.cpu.flags.cf() || ctx.cpu.flags.zf();
            break;
        
        // JNBE/JA - Jump if Not Below or Equal (CF=0 && ZF=0)
        case Opcode::JNBE_REL8:
        case Opcode::JNBE_REL32:
            take_branch = !ctx.cpu.flags.cf() && !ctx.cpu.flags.zf();
            break;
        
        // JS - Jump if Sign (SF=1)
        case Opcode::JS_REL8:
        case Opcode::JS_REL32:
            take_branch = ctx.cpu.flags.sf();
            break;
        
        // JNS - Jump if Not Sign (SF=0)
        case Opcode::JNS_REL8:
        case Opcode::JNS_REL32:
            take_branch = !ctx.cpu.flags.sf();
            break;
        
        // JP/JPE - Jump if Parity (PF=1)
        case Opcode::JP_REL8:
        case Opcode::JP_REL32:
            take_branch = ctx.cpu.flags.pf();
            break;
        
        // JNP/JPO - Jump if Not Parity (PF=0)
        case Opcode::JNP_REL8:
        case Opcode::JNP_REL32:
            take_branch = !ctx.cpu.flags.pf();
            break;
        
        // JL/JNGE - Jump if Less (SF!=OF)
        case Opcode::JL_REL8:
        case Opcode::JL_REL32:
            take_branch = ctx.cpu.flags.sf() != ctx.cpu.flags.of();
            break;
        
        // JNL/JGE - Jump if Not Less (SF==OF)
        case Opcode::JNL_REL8:
        case Opcode::JNL_REL32:
            take_branch = ctx.cpu.flags.sf() == ctx.cpu.flags.of();
            break;
        
        // JLE/JNG - Jump if Less or Equal (ZF=1 || SF!=OF)
        case Opcode::JLE_REL8:
        case Opcode::JLE_REL32:
            take_branch = ctx.cpu.flags.zf() || (ctx.cpu.flags.sf() != ctx.cpu.flags.of());
            break;
        
        // JNLE/JG - Jump if Not Less or Equal (ZF=0 && SF==OF)
        case Opcode::JNLE_REL8:
        case Opcode::JNLE_REL32:
            take_branch = !ctx.cpu.flags.zf() && (ctx.cpu.flags.sf() == ctx.cpu.flags.of());
            break;
        
        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
    
    if (take_branch) {
        auto offset = read_operand(ctx, instr, 0);
        if (!offset) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        // A Jcc not taken never examines its target.
        if (!near_branch_target_canonical(
                ctx, ctx.cpu.rip + instr.length + offset.value())) {
            return ExecResult::Exception;
        }
        ctx.cpu.advance_ip(instr.length);
        ctx.cpu.rip = static_cast<u64>(static_cast<i64>(ctx.cpu.rip) + static_cast<i64>(offset.value()));
        return ExecResult::Branch;
    }

    return ExecResult::Continue;
}

ExecResult exec_loop_jcxz(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // LOOPE/LOOPNE consume ZF; none of these forms produce flags, so settling a
    // JIT-pending lazy op here is both required and non-destructive.
    ctx.cpu.materialize_lazy_flags();

    // All LOOP/JCXZ forms use a rel8 displacement.
    auto offset = read_operand(ctx, instr, 0);
    if (!offset) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    const i64 signed_offset = static_cast<i64>(offset.value());

    auto read_count = [&]() -> u64 {
        if (ctx.cpu.is_64bit()) {
            return ctx.cpu.gpr.rcx;
        }

        // 32-bit mode: address-size override selects CX vs ECX for these instructions.
        if (instr.prefixes.address_size) {
            return ctx.cpu.gpr.get16(static_cast<u8>(RegIndex::RCX));
        }
        return ctx.cpu.gpr.get32(static_cast<u8>(RegIndex::RCX));
    };

    auto write_count = [&](u64 value) {
        if (ctx.cpu.is_64bit()) {
            ctx.cpu.gpr.rcx = value;
            return;
        }

        if (instr.prefixes.address_size) {
            ctx.cpu.gpr.set16(static_cast<u8>(RegIndex::RCX), static_cast<u16>(value));
            return;
        }

        ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RCX), static_cast<u32>(value));
    };

    bool take_branch = false;
    u64 original_count = 0;
    const bool decrements = instr.opcode == Opcode::LOOP_REL8 ||
                            instr.opcode == Opcode::LOOPE_REL8 ||
                            instr.opcode == Opcode::LOOPNE_REL8;

    switch (instr.opcode) {
        case Opcode::JCXZ_REL8:
        case Opcode::JECXZ_REL8:
        case Opcode::JRCXZ_REL8: {
            take_branch = (read_count() == 0);
            break;
        }

        case Opcode::LOOP_REL8:
        case Opcode::LOOPE_REL8:
        case Opcode::LOOPNE_REL8: {
            u64 count = read_count();
            original_count = count;
            count -= 1;
            write_count(count);

            const bool not_zero = (count != 0);
            if (instr.opcode == Opcode::LOOP_REL8) {
                take_branch = not_zero;
            } else if (instr.opcode == Opcode::LOOPE_REL8) {
                take_branch = not_zero && ctx.cpu.flags.zf();
            } else {
                take_branch = not_zero && !ctx.cpu.flags.zf();
            }
            break;
        }

        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }

    if (take_branch) {
        if (!near_branch_target_canonical(
                ctx, ctx.cpu.rip + instr.length + static_cast<u64>(signed_offset))) {
            if (decrements) write_count(original_count);
            return ExecResult::Exception;
        }
        ctx.cpu.advance_ip(instr.length);
        ctx.cpu.rip = static_cast<u64>(static_cast<i64>(ctx.cpu.rip) + signed_offset);
        return ExecResult::Branch;
    }

    return ExecResult::Continue;
}

ExecResult exec_call(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto offset = read_operand(ctx, instr, 0);
    if (!offset) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u64 return_addr = ctx.cpu.rip + instr.length;
    u8 stack_size = ctx.cpu.is_64bit() ? 8 : 4;
    // The target is checked before the push: a #GP CALL pushes nothing.
    if (!near_branch_target_canonical(ctx, return_addr + offset.value())) {
        return ExecResult::Exception;
    }

    auto r = push_value(ctx, return_addr, stack_size);
    if (!r) {
        ctx.exception = stack_access_exception(r.error());
        return ExecResult::Exception;
    }

    ctx.cpu.advance_ip(instr.length);
    ctx.cpu.rip = static_cast<u64>(static_cast<i64>(ctx.cpu.rip) + static_cast<i64>(offset.value()));
    return ExecResult::Branch;
}

ExecResult exec_ret(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Intel SDM Vol. 2 RET (near). In 64-bit mode "the operand size for near
    // call (and all near branches) is forced to 64-bits" (SDM CALL), so 66H
    // changes nothing there. Otherwise 66H selects OperandSize 16: a 2-byte
    // pop and EIP := tempEIP AND 0000FFFFH (the flat CS limit cannot #GP it).
    const bool op16 = !ctx.cpu.is_64bit() && instr.prefixes.operand_size;
    const u8 stack_size = ctx.cpu.is_64bit() ? 8 : (op16 ? 2 : 4);
    const u64 slot = ctx.cpu.sp();
    auto ret_addr = pop_value(ctx, stack_size);
    if (!ret_addr) {
        // A faulting pop is a faulting guest READ (#PF), which Windows
        // reports as STATUS_ACCESS_VIOLATION with the read address -- what
        // the JIT raises for the same RET. Nothing was committed: pop_value
        // moves the stack pointer only after the read succeeds.
        record_read_access_violation(ctx, slot, stack_size);
        ctx.exception = CpuException::PageFault;
        return ExecResult::Exception;
    }

    const u64 target = op16 ? (ret_addr.value() & 0xFFFFu) : ret_addr.value();
    if (!near_branch_target_canonical(ctx, target)) {
        ctx.cpu.set_sp(slot);  // a #GP RET has not popped
        return ExecResult::Exception;
    }
    ctx.cpu.rip = target;

    // RET imm16: "ESP := ESP + SRC" / "RSP := RSP + SRC" by StackAddressSize
    // (32 for the flat SS, 64 in long mode; set_sp wraps at that width), SRC
    // being the zero-extended imm16.
    if (instr.opcode == Opcode::RET_NEAR_IMM16) {
        auto imm = read_operand(ctx, instr, 0);
        if (!imm) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        ctx.cpu.set_sp(ctx.cpu.sp() + imm.value());
    }
    
    return ExecResult::Branch;
}

// Far-return stack slot: 8 bytes in 64-bit mode, else operand-size (66H → 2).
// Intel SDM Vol. 2 RET: far return pops IP then CS. Vitreo is a flat user-mode
// runtime and treats 64-bit far returns as 64-bit operand size (RIP+CS).
static u8 far_return_slot_size(const ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    if (ctx.cpu.is_64bit()) {
        return 8;
    }
    return instr.prefixes.operand_size ? 2 : 4;
}

ExecResult exec_ret_far(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 slot = far_return_slot_size(ctx, instr);
    auto ip = pop_value(ctx, slot);
    if (!ip) {
        ctx.exception = stack_access_exception(ip.error());
        return ExecResult::Exception;
    }
    auto cs = pop_value(ctx, slot);
    if (!cs) {
        ctx.exception = stack_access_exception(cs.error());
        return ExecResult::Exception;
    }

    ctx.cpu.rip = ip.value();
    ctx.cpu.seg.set(SegIndex::CS, static_cast<u16>(cs.value()));

    if (instr.opcode == Opcode::RET_FAR_IMM16) {
        auto imm = read_operand(ctx, instr, 0);
        if (!imm) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        ctx.cpu.set_sp(ctx.cpu.sp() + imm.value());
    }

    return ExecResult::Branch;
}

// Intel SDM Vol. 2 JMP/CALL far: offset then selector, little-endian in memory.
// Direct ptr forms store offset in operand 0 and selector in operand 1.
static bool load_far_cs_ip(ExecContext& ctx, const DecodedInstruction& instr,
                           u64& offset, u16& selector) noexcept {
    switch (instr.opcode) {
        case Opcode::JMP_PTR16_16:
        case Opcode::CALL_PTR16_16: {
            auto off = read_operand(ctx, instr, 0);
            auto sel = read_operand(ctx, instr, 1);
            if (!off || !sel) {
                return false;
            }
            offset = off.value() & 0xFFFFu;
            selector = static_cast<u16>(sel.value());
            return true;
        }
        case Opcode::JMP_PTR16_32:
        case Opcode::CALL_PTR16_32: {
            auto off = read_operand(ctx, instr, 0);
            auto sel = read_operand(ctx, instr, 1);
            if (!off || !sel) {
                return false;
            }
            offset = off.value() & 0xFFFFFFFFu;
            selector = static_cast<u16>(sel.value());
            return true;
        }
        case Opcode::JMP_M16_16:
        case Opcode::CALL_M16_16:
        case Opcode::JMP_M16_32:
        case Opcode::CALL_M16_32:
        case Opcode::JMP_M16_64:
        case Opcode::CALL_M16_64: {
            if (instr.operand_count < 1 || !is_mem_type(instr.operands[0].type)) {
                return false;
            }
            auto addr = calc_effective_address(ctx, instr.operands[0].mem);
            if (!addr) {
                return false;
            }
            u8 off_size = 4;
            if (instr.opcode == Opcode::JMP_M16_16 ||
                instr.opcode == Opcode::CALL_M16_16) {
                off_size = 2;
            } else if (instr.opcode == Opcode::JMP_M16_64 ||
                       instr.opcode == Opcode::CALL_M16_64) {
                off_size = 8;
            }
            auto off = read_memory(ctx, addr.value(), off_size);
            if (!off) {
                return false;
            }
            auto sel = read_memory(ctx, addr.value() + off_size, 2);
            if (!sel) {
                return false;
            }
            offset = off.value();
            selector = static_cast<u16>(sel.value());
            return true;
        }
        default:
            return false;
    }
}

ExecResult exec_jmp_far(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Same-privilege user-mode far JMP: load CS:IP, no privilege/gate/task switch.
    u64 offset = 0;
    u16 selector = 0;
    if (!load_far_cs_ip(ctx, instr, offset, selector)) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    ctx.cpu.rip = offset;
    ctx.cpu.seg.set(SegIndex::CS, selector);
    return ExecResult::Branch;
}

ExecResult exec_call_far(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Same-privilege user-mode far CALL: push CS then IP (RETF pops IP then CS).
    u64 offset = 0;
    u16 selector = 0;
    if (!load_far_cs_ip(ctx, instr, offset, selector)) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    const u8 slot = far_return_slot_size(ctx, instr);
    const u64 return_ip = ctx.cpu.rip + instr.length;
    const u16 return_cs = ctx.cpu.seg.get(SegIndex::CS);

    auto pushed = push_value(ctx, return_cs, slot);
    if (!pushed) {
        ctx.exception = stack_access_exception(pushed.error());
        return ExecResult::Exception;
    }
    pushed = push_value(ctx, return_ip, slot);
    if (!pushed) {
        ctx.exception = stack_access_exception(pushed.error());
        return ExecResult::Exception;
    }

    ctx.cpu.rip = offset;
    ctx.cpu.seg.set(SegIndex::CS, selector);
    return ExecResult::Branch;
}

ExecResult exec_iret(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Intel SDM Vol. 2 IRET/IRETD/IRETQ, same-privilege (CPL == CS.RPL).
    // 32-bit IRETD: pop EIP, CS, EFLAGS (4, or 2 with 66H).
    // 64-bit IRETQ: pop RIP, CS, RFLAGS, RSP, SS (8 each). 64-bit mode always
    // reloads SS:RSP even at the same privilege.
    u8 slot;
    const bool iretq = instr.opcode == Opcode::IRETQ ||
                       (instr.opcode != Opcode::IRET && ctx.cpu.is_64bit());
    if (iretq) {
        slot = 8;
    } else if (instr.opcode == Opcode::IRET || instr.prefixes.operand_size) {
        slot = 2;
    } else {
        slot = 4;
    }

    auto ip = pop_value(ctx, slot);
    if (!ip) {
        ctx.exception = stack_access_exception(ip.error());
        return ExecResult::Exception;
    }
    auto cs = pop_value(ctx, slot);
    if (!cs) {
        ctx.exception = stack_access_exception(cs.error());
        return ExecResult::Exception;
    }
    auto flags = pop_value(ctx, slot);
    if (!flags) {
        ctx.exception = stack_access_exception(flags.error());
        return ExecResult::Exception;
    }

    u64 new_rsp = 0;
    u64 new_ss = 0;
    if (iretq) {
        auto rsp = pop_value(ctx, slot);
        if (!rsp) {
            ctx.exception = stack_access_exception(rsp.error());
            return ExecResult::Exception;
        }
        auto ss = pop_value(ctx, slot);
        if (!ss) {
            ctx.exception = stack_access_exception(ss.error());
            return ExecResult::Exception;
        }
        new_rsp = rsp.value();
        new_ss = ss.value();
    }

    ctx.cpu.rip = ip.value();
    ctx.cpu.seg.set(SegIndex::CS, static_cast<u16>(cs.value()));

    // Reuse POPF's user-mode writable mask (interpreter PUSHF/POPF).
    constexpr u64 user_mask = 0x00244FD5; // CF,PF,AF,ZF,SF,TF,IF,DF,OF,AC,ID
    u64 new_flags = (ctx.cpu.flags.rflags & ~user_mask) | (flags.value() & user_mask);
    new_flags |= 0x0002; // bit 1 always set
    ctx.cpu.flags.rflags = new_flags;
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    if (iretq) {
        ctx.cpu.set_sp(new_rsp);
        ctx.cpu.seg.set(SegIndex::SS, static_cast<u16>(new_ss));
    }

    return ExecResult::Branch;
}

} // namespace vitreo::cpu::ops
