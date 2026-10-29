// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Logical instruction execution (AND, OR, NOT)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>

namespace vitreo::cpu::ops {

ExecResult exec_and(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // See exec_add: retire JIT lazy FlagOp before eager rflags writes.
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    auto src = read_operand(ctx, instr, 1);
    if (!src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);
    if (instr.prefixes.lock && size == 4 &&
        is_memory_operand(instr.operands[0].type)) {
        auto address = calc_effective_address(ctx, instr.operands[0].mem);
        Byte* host = address
            ? ctx.mem.host_ptr_store(address.value(), sizeof(u32))
            : nullptr;
        if (host != nullptr &&
            reinterpret_cast<uintptr_t>(host) % alignof(u32) == 0) {
            const u32 source = static_cast<u32>(src.value());
            std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
            const u32 previous =
                value.fetch_and(source, std::memory_order_seq_cst);
            ctx.cpu.flags.update_flags_logic(previous & source);
            write_watch::record_store(address.value(), sizeof(u32));
            return ExecResult::Continue;
        }
    }

    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 result = dst.value() & src.value();
    
    // Update flags based on operand size
    switch (size) {
        case 1: {
            u8 r = static_cast<u8>(result);
            ctx.cpu.flags.update_flags_logic(r);
            result = r;
            break;
        }
        case 2: {
            u16 r = static_cast<u16>(result);
            ctx.cpu.flags.update_flags_logic(r);
            result = r;
            break;
        }
        case 4: {
            u32 r = static_cast<u32>(result);
            ctx.cpu.flags.update_flags_logic(r);
            result = r;
            break;
        }
        default:
            ctx.cpu.flags.update_flags_logic(result);
            break;
    }
    
    auto r = write_operand(ctx, instr, 0, result);
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

ExecResult exec_or(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    auto src = read_operand(ctx, instr, 1);
    if (!src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);
    if (instr.prefixes.lock && size == 1 &&
        is_memory_operand(instr.operands[0].type)) {
        // Intel SDM Vol. 2, OR: the byte RMW is atomic, with CF/OF cleared
        // and SF/ZF/PF from the result. Every byte address is aligned.
        auto address = calc_effective_address(ctx, instr.operands[0].mem);
        if (!address) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        Byte* host = ctx.mem.host_ptr_store(address.value(), sizeof(u8));
        LockPrefixGuard guard(host, sizeof(u8));
        const u8 source = static_cast<u8>(src.value());
        u8 result;
        if (host != nullptr) {
            std::atomic_ref<u8> value(*host);
            result = static_cast<u8>(value.fetch_or(source, std::memory_order_seq_cst) | source);
            write_watch::record_store(address.value(), sizeof(u8));
        } else {
            // Watched/code mappings and faults retain the normal memory path.
            auto previous = ctx.mem.read_u8(address.value());
            if (!previous || ctx.mem.write_u8(address.value(),
                    static_cast<u8>(previous.value() | source)).is_err()) {
                ctx.exception = CpuException::PageFault;
                return ExecResult::Exception;
            }
            result = static_cast<u8>(previous.value() | source);
        }
        ctx.cpu.flags.update_flags_logic(result);
        return ExecResult::Continue;
    }
    if (instr.prefixes.lock && size == 4 &&
        is_memory_operand(instr.operands[0].type)) {
        auto address = calc_effective_address(ctx, instr.operands[0].mem);
        Byte* host = address
            ? ctx.mem.host_ptr_store(address.value(), sizeof(u32))
            : nullptr;
        if (host != nullptr &&
            reinterpret_cast<uintptr_t>(host) % alignof(u32) == 0) {
            const u32 source = static_cast<u32>(src.value());
            std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
            const u32 previous =
                value.fetch_or(source, std::memory_order_seq_cst);
            ctx.cpu.flags.update_flags_logic(previous | source);
            write_watch::record_store(address.value(), sizeof(u32));
            return ExecResult::Continue;
        }
    }

    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 result = dst.value() | src.value();
    
    // Update flags based on operand size
    switch (size) {
        case 1: {
            u8 r = static_cast<u8>(result);
            ctx.cpu.flags.update_flags_logic(r);
            result = r;
            break;
        }
        case 2: {
            u16 r = static_cast<u16>(result);
            ctx.cpu.flags.update_flags_logic(r);
            result = r;
            break;
        }
        case 4: {
            u32 r = static_cast<u32>(result);
            ctx.cpu.flags.update_flags_logic(r);
            result = r;
            break;
        }
        default:
            ctx.cpu.flags.update_flags_logic(result);
            break;
    }
    
    auto r = write_operand(ctx, instr, 0, result);
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

ExecResult exec_not(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 size = operand_size(instr.operands[0].type);
    if (instr.prefixes.lock && size == 4 &&
        is_memory_operand(instr.operands[0].type)) {
        auto address = calc_effective_address(ctx, instr.operands[0].mem);
        Byte* host = address
            ? ctx.mem.host_ptr_store(address.value(), sizeof(u32))
            : nullptr;
        if (host != nullptr &&
            reinterpret_cast<uintptr_t>(host) % alignof(u32) == 0) {
            std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
            u32 previous = value.load(std::memory_order_seq_cst);
            while (!value.compare_exchange_weak(
                previous, ~previous, std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {}
            write_watch::record_store(address.value(), sizeof(u32));
            return ExecResult::Continue;
        }
    }

    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u64 result = ~dst.value();
    
    // NOT does NOT update any flags - mask result to size
    switch (size) {
        case 1:
            result = static_cast<u8>(result);
            break;
        case 2:
            result = static_cast<u16>(result);
            break;
        case 4:
            result = static_cast<u32>(result);
            break;
        default:
            // 64-bit, no masking needed
            break;
    }
    
    auto r = write_operand(ctx, instr, 0, result);
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
