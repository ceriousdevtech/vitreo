// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Arithmetic instruction execution (ADD, SUB, XOR, CMP, TEST)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>

namespace vitreo::cpu::ops {

ExecResult exec_add(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Eager rflags writers must retire any pending JIT lazy FlagOp. Otherwise
    // a later interpreter Jcc rematerializes the stale op and ignores these
    // flags (a 32-bit installer: CMP equal + JNE taken skips the extract).
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
                value.fetch_add(source, std::memory_order_seq_cst);
            ctx.cpu.flags.update_flags_add(
                previous, source, previous + source);
            write_watch::record_store(address.value(), sizeof(u32));
            return ExecResult::Continue;
        }
    }

    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 result = dst.value() + src.value();
    
    // Update flags based on operand size
    switch (size) {
        case 1: {
            u8 r = static_cast<u8>(result);
            ctx.cpu.flags.update_flags_add(static_cast<u8>(dst.value()), static_cast<u8>(src.value()), r);
            result = r;
            break;
        }
        case 2: {
            u16 r = static_cast<u16>(result);
            ctx.cpu.flags.update_flags_add(static_cast<u16>(dst.value()), static_cast<u16>(src.value()), r);
            result = r;
            break;
        }
        case 4: {
            u32 r = static_cast<u32>(result);
            ctx.cpu.flags.update_flags_add(static_cast<u32>(dst.value()), static_cast<u32>(src.value()), r);
            result = r;
            break;
        }
        default:
            ctx.cpu.flags.update_flags_add(dst.value(), src.value(), result);
            break;
    }
    
    auto r = write_operand(ctx, instr, 0, result);
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

ExecResult exec_sub(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
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
                value.fetch_sub(source, std::memory_order_seq_cst);
            ctx.cpu.flags.update_flags_sub(
                previous, source, previous - source);
            write_watch::record_store(address.value(), sizeof(u32));
            return ExecResult::Continue;
        }
    }

    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 result = dst.value() - src.value();
    
    // Update flags based on operand size
    switch (size) {
        case 1: {
            u8 r = static_cast<u8>(result);
            ctx.cpu.flags.update_flags_sub(static_cast<u8>(dst.value()), static_cast<u8>(src.value()), r);
            result = r;
            break;
        }
        case 2: {
            u16 r = static_cast<u16>(result);
            ctx.cpu.flags.update_flags_sub(static_cast<u16>(dst.value()), static_cast<u16>(src.value()), r);
            result = r;
            break;
        }
        case 4: {
            u32 r = static_cast<u32>(result);
            ctx.cpu.flags.update_flags_sub(static_cast<u32>(dst.value()), static_cast<u32>(src.value()), r);
            result = r;
            break;
        }
        default:
            ctx.cpu.flags.update_flags_sub(dst.value(), src.value(), result);
            break;
    }
    
    auto r = write_operand(ctx, instr, 0, result);
    if (!r) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    return ExecResult::Continue;
}

ExecResult exec_xor(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
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
                value.fetch_xor(source, std::memory_order_seq_cst);
            ctx.cpu.flags.update_flags_logic(previous ^ source);
            write_watch::record_store(address.value(), sizeof(u32));
            return ExecResult::Continue;
        }
    }

    auto dst = read_operand(ctx, instr, 0);
    if (!dst) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 result = dst.value() ^ src.value();
    
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

ExecResult exec_cmp(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    auto dst = read_operand(ctx, instr, 0);
    auto src = read_operand(ctx, instr, 1);
    if (!dst || !src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = operand_size(instr.operands[0].type);
    u64 result = dst.value() - src.value();
    
    // Update flags based on operand size (CMP = SUB without storing result)
    switch (size) {
        case 1:
            ctx.cpu.flags.update_flags_sub(static_cast<u8>(dst.value()), static_cast<u8>(src.value()), static_cast<u8>(result));
            break;
        case 2:
            ctx.cpu.flags.update_flags_sub(static_cast<u16>(dst.value()), static_cast<u16>(src.value()), static_cast<u16>(result));
            break;
        case 4:
            ctx.cpu.flags.update_flags_sub(static_cast<u32>(dst.value()), static_cast<u32>(src.value()), static_cast<u32>(result));
            break;
        default:
            ctx.cpu.flags.update_flags_sub(dst.value(), src.value(), result);
            break;
    }
    return ExecResult::Continue;
}

ExecResult exec_test(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    ctx.cpu.set_flag_op(CpuState::FlagOp::None);

    auto dst = read_operand(ctx, instr, 0);
    auto src = read_operand(ctx, instr, 1);
    if (!dst || !src) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }
    
    u8 size = operand_size(instr.operands[0].type);
    u64 result = dst.value() & src.value();
    
    // Update flags based on operand size (TEST = AND without storing result)
    switch (size) {
        case 1:
            ctx.cpu.flags.update_flags_logic(static_cast<u8>(result));
            break;
        case 2:
            ctx.cpu.flags.update_flags_logic(static_cast<u16>(result));
            break;
        case 4:
            ctx.cpu.flags.update_flags_logic(static_cast<u32>(result));
            break;
        default:
            ctx.cpu.flags.update_flags_logic(result);
            break;
    }
    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
