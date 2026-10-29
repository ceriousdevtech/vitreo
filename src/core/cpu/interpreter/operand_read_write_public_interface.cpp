// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Interpreter - instruction dispatcher

#include "interpreter.hpp"
#include "ops/exec_ops.hpp"
#include "ops/helpers.hpp"
#include "../lock_prefix.hpp"
#include "core/memory/mem_watch.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>

/// @file operand_read_write_public_interface.cpp
/// @brief Operand read/write (public interface, calls helpers)
///
/// Closed cut from interpreter.cpp — verified with plan_extraction.py.

namespace vitreo::cpu {

namespace {

/// The exception record of a failed @p size-byte guest access at @p addr --
/// the same rule as the JIT's checked helpers (jit_raise_memory_av): a
/// NON-CANONICAL byte is #GP, which Windows reports as a read of
/// 0xFFFFFFFFFFFFFFFF; otherwise the direction and the first inaccessible
/// byte, which for an access running from an accessible page into a denied
/// one is the denied page's first byte
/// (tools/oracle_probe/high_address_fault_probe.expected,
/// split_access_fault_address_probe.expected).
void record_access_violation(ExecContext& ctx, u64 addr, u8 size,
                             bool is_write) {
    const auto canonical = [](u64 a) {
        return static_cast<u64>(static_cast<i64>(a << 16) >> 16) == a;
    };
    const u64 last = addr + (size > 0 ? size - 1u : 0u);
    if (!canonical(addr) || !canonical(last)) {
        ctx.exception_error = 0;
        ctx.exception_addr = ~u64{0};
        return;
    }
    ctx.exception_error = is_write ? 1 : 0;
    ctx.exception_addr = addr;
    const u64 next_page = (addr | u64{0xFFF}) + 1;
    if (size <= 1 || next_page == 0 || next_page - addr >= size) return;
    GuestAddr base = 0;
    usize region_size = 0;
    MemoryProtection prot{};
    if (ctx.mem.query_region(addr, base, region_size, prot) &&
        has_flag(prot, is_write ? MemoryProtection::Write
                                : MemoryProtection::Read)) {
        ctx.exception_addr = next_page;
    }
}

}  // namespace

// ============================================================================
// Operand read/write (public interface, calls helpers)
// ============================================================================

Result<u64, Error> read_operand(
    ExecContext& ctx,
    const DecodedInstruction& instr,
    usize operand_idx
) noexcept {
    using namespace ops;
    
    if (operand_idx >= instr.operand_count) {
        return Result<u64, Error>::err(Error::InvalidArgument);
    }
    
    const auto& op = instr.operands[operand_idx];
    
    if (is_reg_type(op.type)) {
        // Handle legacy high-byte registers (AH/CH/DH/BH):
        // Without REX prefix, Reg8 indices 4-7 mean AH/CH/DH/BH (byte 1 of the GPR).
        // With REX prefix, indices 4-7 mean SPL/BPL/SIL/DIL and 8-15 mean R8B-R15B.
        if (op.type == OperandType::Reg8 && !instr.prefixes.has_rex()
            && op.reg >= 4 && op.reg <= 7) {
            return Result<u64, Error>::ok(ctx.cpu.gpr.get8h(op.reg - 4));
        }
        return Result<u64, Error>::ok(read_register(ctx, op.type, op.reg));
    }
    
    if (is_mem_type(op.type)) {
        auto addr_result = calc_effective_address(ctx, op.mem);
        if (!addr_result) return Result<u64, Error>::err(addr_result.error());

        u8 size = mem_operand_size(op.type);
        if (size == 0) size = 8; // Default to 64-bit for MemUnknown

        const u64 addr = addr_result.value();
        auto loaded = read_memory(ctx, addr, size);
        if (!loaded) {
            // Windows ACCESS_VIOLATION: ExceptionInformation[0]=0 (read),
            // [1]=faulting VA. Packer SEH (Petite, …) keys off both.
            record_access_violation(ctx, addr, size, /*is_write=*/false);
        }
        return loaded;
    }
    
    if (is_imm_type(op.type)) {
        return Result<u64, Error>::ok(static_cast<u64>(op.imm));
    }
    
    if (is_rel_type(op.type)) {
        // Sign-extend the relative offset
        switch (op.type) {
            case OperandType::Rel8:
                return Result<u64, Error>::ok(static_cast<u64>(static_cast<i64>(static_cast<i8>(op.imm))));
            case OperandType::Rel16:
                return Result<u64, Error>::ok(static_cast<u64>(static_cast<i64>(static_cast<i16>(op.imm))));
            case OperandType::Rel32:
                return Result<u64, Error>::ok(static_cast<u64>(static_cast<i64>(static_cast<i32>(op.imm))));
            default:
                break;
        }
    }
    
    return Result<u64, Error>::err(Error::NotSupported);
}

VoidResult write_operand(
    ExecContext& ctx,
    const DecodedInstruction& instr,
    usize operand_idx,
    u64 value
) noexcept {
    using namespace ops;
    
    if (operand_idx >= instr.operand_count) {
        return VoidResult::err(Error::InvalidArgument);
    }
    
    const auto& op = instr.operands[operand_idx];
    
    if (is_reg_type(op.type)) {
        // Handle legacy high-byte registers (AH/CH/DH/BH)
        if (op.type == OperandType::Reg8 && !instr.prefixes.has_rex()
            && op.reg >= 4 && op.reg <= 7) {
            ctx.cpu.gpr.set8h(op.reg - 4, static_cast<u8>(value));
            return VoidResult::ok();
        }
        write_register(ctx, op.type, op.reg, value);
        return VoidResult::ok();
    }
    
    if (is_mem_type(op.type)) {
        auto addr_result = calc_effective_address(ctx, op.mem);
        if (!addr_result) return VoidResult::err(addr_result.error());

        u8 size = mem_operand_size(op.type);
        if (size == 0) size = 8; // Default for MemUnknown

        const u64 addr = addr_result.value();
        auto stored = write_memory(ctx, addr, value, size);
        if (!stored) {
            // Windows ACCESS_VIOLATION: ExceptionInformation[0]=1 (write),
            // [1]=faulting VA. Petite's unpack SEH expects a write fault at
            // ImageBase, not a zero address reported as a read.
            record_access_violation(ctx, addr, size, /*is_write=*/true);
        }
        return stored;
    }
    
    return VoidResult::err(Error::NotSupported);
}

VoidResult push_value(
    ExecContext& ctx,
    u64 value,
    u8 size
) noexcept {
    u64 sp = ctx.cpu.sp();
    sp -= size;
    if (ctx.cpu.is_64bit() &&
        ctx.cpu.stack_overflow_dispatch_depth == 0) {
        const GuestAddr teb =
            static_cast<GuestAddr>(ctx.cpu.seg.gs_desc.base);
        const auto stack_limit = ctx.mem.read_u64(teb + 0x10);
        if (stack_limit.is_ok()) {
            // The stack guarantee is recovery space used after an overflow;
            // normal stack operations continue to compare with StackLimit.
            if (sp >= stack_limit.value()) {
                ctx.cpu.stack_overflow_active = false;
            } else if (!ctx.cpu.stack_overflow_active) {
                ctx.cpu.stack_overflow_active = true;
                return VoidResult::err(Error::Overflow);
            }
        }
    }
    auto stored = ops::write_memory(ctx, sp, value, size);
    if (!stored) {
        // A #PF on the slot (tools/oracle_probe/stack_access_fault_probe):
        // STATUS_ACCESS_VIOLATION, a write of the slot. The dispatcher turns
        // one inside the stack guard into STATUS_STACK_OVERFLOW.
        record_access_violation(ctx, sp, size, /*is_write=*/true);
        return stored;
    }

    // Intel SDM Vol. 2 PUSH/CALL: a stack-write fault is precise. Commit the
    // architectural stack pointer only after the return value was written.
    ctx.cpu.set_sp(sp);
    return VoidResult::ok();
}

Result<u64, Error> pop_value(
    ExecContext& ctx,
    u8 size
) noexcept {
    u64 sp = ctx.cpu.sp();
    
    auto result = ops::read_memory(ctx, sp, size);
    if (!result) {
        record_access_violation(ctx, sp, size, /*is_write=*/false);
        return result;
    }

    ctx.cpu.set_sp(sp + size);

    return result;
}

void record_read_access_violation(
    ExecContext& ctx,
    u64 addr,
    u8 size
) noexcept {
    record_access_violation(ctx, addr, size, /*is_write=*/false);
}

void record_write_access_violation(
    ExecContext& ctx,
    u64 addr,
    u8 size
) noexcept {
    record_access_violation(ctx, addr, size, /*is_write=*/true);
}

} // namespace vitreo::cpu
