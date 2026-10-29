// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Atomic-like and miscellaneous instruction execution
// (CMPXCHG, XADD, XCHG r/m, flag ops)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "../../lock_prefix.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <atomic>

namespace vitreo::cpu::ops {

// ============================================================================
// CMPXCHG - Compare and Exchange
// ============================================================================

ExecResult exec_cmpxchg(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    if (instr.operand_count < 2) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }

    // Determine operand size from opcode
    u8 size = operand_size(instr.operands[0].type);

    // Intel SDM Vol. 2 CMPXCHG: the destination is ALWAYS written back — with
    // the source when the comparison succeeds, and with itself when it fails.
    // So a CMPXCHG against a read-only page faults on real hardware either
    // way, and the "not equal" path must not quietly succeed just because it
    // has nothing new to store. Permission is asked without performing the
    // write-back, so write-watch and cache state are untouched.
    if (is_memory_operand(instr.operands[0].type)) {
        auto probe = calc_effective_address(ctx, instr.operands[0].mem);
        if (probe && !ctx.mem.guest_can_write(probe.value(), size)) {
            ctx.exception = CpuException::PageFault;
            return ExecResult::Exception;
        }
    }

    // `accumulator - DEST`; see the note on the non-atomic path below.
    auto apply_cmpxchg_flags = [&](u64 observed, u64 acc, bool equal) {
        const u64 cmp_result = acc - observed;
        switch (size) {
            case 1:
                ctx.cpu.flags.update_flags_sub(
                    static_cast<u8>(acc), static_cast<u8>(observed),
                    static_cast<u8>(cmp_result));
                break;
            case 2:
                ctx.cpu.flags.update_flags_sub(
                    static_cast<u16>(acc), static_cast<u16>(observed),
                    static_cast<u16>(cmp_result));
                break;
            case 4:
                ctx.cpu.flags.update_flags_sub(
                    static_cast<u32>(acc), static_cast<u32>(observed),
                    static_cast<u32>(cmp_result));
                break;
            default:
                ctx.cpu.flags.update_flags_sub(acc, observed, cmp_result);
                break;
        }
        ctx.cpu.flags.set_zf(equal);
        if (!equal) {
            switch (size) {
                case 1: ctx.cpu.gpr.set8l(0, static_cast<u8>(observed)); break;
                case 2: ctx.cpu.gpr.set16(0, static_cast<u16>(observed)); break;
                case 4: ctx.cpu.gpr.set32(0, static_cast<u32>(observed)); break;
                default: ctx.cpu.gpr.set64(0, observed); break;
            }
        }
    };

    // Intel SDM Vol. 2 CMPXCHG + LOCK: the r/m memory operand is a single
    // atomic RMW. TIMEPROC dispatch runs execute_instruction without
    // lock_prefix_mutex, so a plain load/store here races the UI thread's
    // JIT LOCK CMPXCHG and both sides can "acquire" a QMutex.
    //
    // The fast path below therefore uses a host atomic and depends on no
    // mutex at all. Only the operand shapes that have no host atomic --
    // unaligned (split lock) or no host pointer -- fall through to the
    // serialized path after it.
    if (instr.prefixes.lock &&
        is_memory_operand(instr.operands[0].type)) {
        auto address = calc_effective_address(ctx, instr.operands[0].mem);
        Byte* host = address
            ? ctx.mem.host_ptr_store(address.value(), size)
            : nullptr;
        // This handler owns LOCK serialization so JIT-delegated aligned
        // CMPXCHG can use the reader/host-atomic path instead of first taking
        // the process-wide fallback mutex in Interpreter::execute_predecoded.
        // A split or unmapped operand still enters exclusive mode here.
        cpu::LockPrefixGuard lock_guard(host, size);
        const uintptr_t host_addr = reinterpret_cast<uintptr_t>(host);
        if (host != nullptr && (host_addr % size) == 0) {
            auto reg_val = read_operand(ctx, instr, 1);
            if (!reg_val) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            u64 acc = 0;
            bool exchanged = false;
            u64 observed = 0;
            switch (size) {
                case 1: {
                    acc = ctx.cpu.gpr.get8l(0);
                    u8 expected = static_cast<u8>(acc);
                    std::atomic_ref<u8> value(*host);
                    exchanged = value.compare_exchange_strong(
                        expected, static_cast<u8>(reg_val.value()),
                        std::memory_order_seq_cst, std::memory_order_seq_cst);
                    observed = expected;
                    break;
                }
                case 2: {
                    acc = ctx.cpu.gpr.get16(0);
                    u16 expected = static_cast<u16>(acc);
                    std::atomic_ref<u16> value(*reinterpret_cast<u16*>(host));
                    exchanged = value.compare_exchange_strong(
                        expected, static_cast<u16>(reg_val.value()),
                        std::memory_order_seq_cst, std::memory_order_seq_cst);
                    observed = expected;
                    break;
                }
                case 4: {
                    acc = ctx.cpu.gpr.get32(0);
                    u32 expected = static_cast<u32>(acc);
                    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
                    exchanged = value.compare_exchange_strong(
                        expected, static_cast<u32>(reg_val.value()),
                        std::memory_order_seq_cst, std::memory_order_seq_cst);
                    observed = expected;
                    break;
                }
                default: {
                    acc = ctx.cpu.gpr.get64(0);
                    u64 expected = acc;
                    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
                    exchanged = value.compare_exchange_strong(
                        expected, reg_val.value(),
                        std::memory_order_seq_cst, std::memory_order_seq_cst);
                    observed = expected;
                    break;
                }
            }
            apply_cmpxchg_flags(observed, acc, exchanged);
            // Only the successful exchange stores. A failed CMPXCHG leaves
            // memory untouched and must not be reported as a write.
            if (exchanged) {
                write_watch::record_store(address.value(), size);
            }
            return ExecResult::Continue;
        }

        // Split lock (unaligned) or no host pointer: there is no host atomic
        // for this, so the RMW has to be a plain read/modify/write. Intel SDM
        // Vol. 3A 9.1.2.2 still requires the operation to be atomic with
        // respect to every other agent, so serialize it on the same
        // process-wide mutex the JIT's unaligned LOCK fallback uses -- the
        // guard is re-entrant, so this is correct both under a dispatcher that
        // already holds it and under one that does not.
        if (address) {
            const GuestAddr ea = address.value();
            cpu::LockPrefixGuard guard;
            u64 observed = 0;
            bool read_ok = false;
            switch (size) {
                case 1: {
                    auto r = ctx.mem.read_u8(ea);
                    read_ok = r.is_ok();
                    if (read_ok) observed = r.value();
                    break;
                }
                case 2: {
                    auto r = ctx.mem.read_u16(ea);
                    read_ok = r.is_ok();
                    if (read_ok) observed = r.value();
                    break;
                }
                case 4: {
                    auto r = ctx.mem.read_u32(ea);
                    read_ok = r.is_ok();
                    if (read_ok) observed = r.value();
                    break;
                }
                default: {
                    auto r = ctx.mem.read_u64(ea);
                    read_ok = r.is_ok();
                    if (read_ok) observed = r.value();
                    break;
                }
            }
            if (!read_ok) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto reg_operand = read_operand(ctx, instr, 1);
            if (!reg_operand) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            u64 acc = 0;
            switch (size) {
                case 1: acc = ctx.cpu.gpr.get8l(0); break;
                case 2: acc = ctx.cpu.gpr.get16(0); break;
                case 4: acc = ctx.cpu.gpr.get32(0); break;
                default: acc = ctx.cpu.gpr.get64(0); break;
            }
            const bool equal = observed == acc;
            bool write_ok = true;
            if (equal) {
                const u64 desired = reg_operand.value();
                switch (size) {
                    case 1:
                        write_ok = ctx.mem.write_u8(
                            ea, static_cast<u8>(desired)).is_ok();
                        break;
                    case 2:
                        write_ok = ctx.mem.write_u16(
                            ea, static_cast<u16>(desired)).is_ok();
                        break;
                    case 4:
                        write_ok = ctx.mem.write_u32(
                            ea, static_cast<u32>(desired)).is_ok();
                        break;
                    default:
                        write_ok = ctx.mem.write_u64(ea, desired).is_ok();
                        break;
                }
                if (!write_ok) {
                    ctx.exception = CpuException::GeneralProtection;
                    return ExecResult::Exception;
                }
                write_watch::record_store(ea, size);
            }
            apply_cmpxchg_flags(observed, acc, equal);
            return ExecResult::Continue;
        }
    }

    // Read the r/m operand (operand 0) and the reg operand (operand 1)
    auto rm_val = read_operand(ctx, instr, 0);
    auto reg_val = read_operand(ctx, instr, 1);
    if (!rm_val || !reg_val) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // Read accumulator (AL/AX/EAX/RAX) — always register index 0
    u64 acc;
    switch (size) {
        case 1: acc = ctx.cpu.gpr.get8l(0); break;
        case 2: acc = ctx.cpu.gpr.get16(0); break;
        case 4: acc = ctx.cpu.gpr.get32(0); break;
        default: acc = ctx.cpu.gpr.get64(0); break;
    }

    // CMP accumulator, DEST -- `accumulator - DEST`, measured on real
    // hardware, NOT the other way round.
    //
    // This was `DEST - accumulator` in the interpreter, in the stencil JIT and
    // in every lock helper, each citing an oracle. `tools/oracle_probe/
    // cmpxchg_flag_probe.c` settles it on the Windows box: a hand-encoded
    // `lock cmpxchg byte ptr [ebx], cl` leaves flags identical to `cmp al, bl`
    // and different from `cmp bl, al` in every unequal case, including the
    // 0x7F/0x80 overflow corner. Its recorded output is beside it.
    //
    // ZF is the same either way, which is why a decade of lock loops never
    // noticed: only a CMPXCHG followed by a CF/SF/OF/AF/PF read is affected,
    // and those branch the wrong way.
    u64 cmp_result = acc - rm_val.value();

    switch (size) {
        case 1:
            ctx.cpu.flags.update_flags_sub(
                static_cast<u8>(acc),
                static_cast<u8>(rm_val.value()),
                static_cast<u8>(cmp_result));
            break;
        case 2:
            ctx.cpu.flags.update_flags_sub(
                static_cast<u16>(acc),
                static_cast<u16>(rm_val.value()),
                static_cast<u16>(cmp_result));
            break;
        case 4:
            ctx.cpu.flags.update_flags_sub(
                static_cast<u32>(acc),
                static_cast<u32>(rm_val.value()),
                static_cast<u32>(cmp_result));
            break;
        default:
            ctx.cpu.flags.update_flags_sub(acc, rm_val.value(), cmp_result);
            break;
    }

    // Equality check is direction-independent.
    bool equal = (cmp_result == 0);
    ctx.cpu.flags.set_zf(equal);

    if (ctx.cpu.flags.zf()) {
        // Equal: reg → r/m
        auto r = write_operand(ctx, instr, 0, reg_val.value());
        if (!r) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
    } else {
        // Not equal: r/m → accumulator
        switch (size) {
            case 1: ctx.cpu.gpr.set8l(0, static_cast<u8>(rm_val.value())); break;
            case 2: ctx.cpu.gpr.set16(0, static_cast<u16>(rm_val.value())); break;
            case 4: ctx.cpu.gpr.set32(0, static_cast<u32>(rm_val.value())); break;
            default: ctx.cpu.gpr.set64(0, rm_val.value()); break;
        }
        // Intel SDM Vol. 2 CMPXCHG: `DEST := TEMP` runs on this path too — the
        // destination is written back with its own value. That is not a no-op
        // for a REGISTER destination in 64-bit mode, where a 32-bit write
        // zero-extends and clears bits 63:32. Skipping it left the upper half
        // of the destination intact: measured against hardware, which returns
        // RCX = 0x00000000_4b5a8b22 where Vitreo returned
        // 0x0f1e2d3c_4b5a8b22.
        //
        // Deliberately NOT done for a memory destination. The probe at the top
        // of this function already asks permission without performing the
        // write-back, precisely so write-watch and cache state stay untouched,
        // and re-storing the same bytes would report a store that the guest's
        // GC-style write-watch consumers would act on.
        if (is_register_operand(instr.operands[0].type)) {
            auto wb = write_operand(ctx, instr, 0, rm_val.value());
            if (!wb) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
    }

    return ExecResult::Continue;
}

// ============================================================================
// CMPXCHG8B - Compare and Exchange 8 Bytes
// Compares EDX:EAX with m64. If equal, sets ZF and stores ECX:EBX to m64.
// Otherwise, clears ZF and loads m64 into EDX:EAX.
// ============================================================================

ExecResult exec_cmpxchg8b(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // Operand 0: m64 memory operand
    auto addr = calc_effective_address(ctx, instr.operands[0].mem);
    if (!addr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // Read 8 bytes from memory (little-endian: low dword at addr, high at addr+4)
    auto lo = ctx.mem.read_u32(addr.value());
    auto hi = ctx.mem.read_u32(addr.value() + 4);
    if (!lo || !hi) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u32 mem_lo = lo.value();
    u32 mem_hi = hi.value();
    u32 eax = ctx.cpu.gpr.get32(0); // EAX
    u32 edx = ctx.cpu.gpr.get32(2); // EDX

    if (mem_lo == eax && mem_hi == edx) {
        // Equal: ZF=1, store ECX:EBX to memory
        ctx.cpu.flags.set_zf(true);
        u32 ebx = ctx.cpu.gpr.get32(3); // EBX
        u32 ecx = ctx.cpu.gpr.get32(1); // ECX
        auto r1 = ctx.mem.write_u32(addr.value(), ebx);
        auto r2 = ctx.mem.write_u32(addr.value() + 4, ecx);
        if (!r1 || !r2) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
    } else {
        // Not equal: ZF=0, load m64 into EDX:EAX
        ctx.cpu.flags.set_zf(false);
        ctx.cpu.gpr.set32(0, mem_lo); // EAX
        ctx.cpu.gpr.set32(2, mem_hi); // EDX
    }

    return ExecResult::Continue;
}

// ============================================================================
// CMPXCHG16B - Compare and Exchange 16 Bytes (REX.W 0F C7 /1)
// Compares RDX:RAX with m128. If equal, sets ZF and stores RCX:RBX to m128.
// Otherwise clears ZF and loads m128 into RDX:RAX.
// ============================================================================

ExecResult exec_cmpxchg16b(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    auto addr = calc_effective_address(ctx, instr.operands[0].mem);
    if (!addr) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // Intel SDM Vol. 2 CMPXCHG16B: m128 must be 16-byte aligned (#GP otherwise).
    if ((addr.value() & 0xFu) != 0) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    if (instr.prefixes.lock) {
        // Intel SDM Vol. 2 CMPXCHG16B + LOCK: 128-bit host RMW. Compare
        // RDX:RAX; equal → ZF=1 store RCX:RBX atomically; else ZF=0 load
        // m128 into RDX:RAX. Missing or unaligned host pointer is #GP —
        // never a torn non-atomic store. Interpreter.cpp already holds
        // lock_prefix_mutex around LOCK.
        Byte* host = ctx.mem.host_ptr_store(addr.value(), 16);
        if (host == nullptr ||
            (reinterpret_cast<uintptr_t>(host) & 0xFu) != 0) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }

        const u64 rax = ctx.cpu.gpr.get64(0);
        const u64 rdx = ctx.cpu.gpr.get64(2);
        const u64 rbx = ctx.cpu.gpr.get64(3);
        const u64 rcx = ctx.cpu.gpr.get64(1);
        __uint128_t observed = (static_cast<__uint128_t>(rdx) << 64) | rax;
        const __uint128_t replacement =
            (static_cast<__uint128_t>(rcx) << 64) | rbx;
        const bool exchanged = __atomic_compare_exchange_n(
            reinterpret_cast<__uint128_t*>(host),
            &observed,
            replacement,
            false,
            __ATOMIC_SEQ_CST,
            __ATOMIC_SEQ_CST);
        ctx.cpu.materialize_lazy_flags();
        ctx.cpu.flags.set_zf(exchanged);
        if (exchanged) {
            write_watch::record_store(addr.value(), 16);
        } else {
            ctx.cpu.gpr.set64(0, static_cast<u64>(observed));
            ctx.cpu.gpr.set64(2, static_cast<u64>(observed >> 64));
        }
        // ZF is architectural; clear lazy FlagOp so a later materialize
        // cannot overwrite it (JIT: materialize_lazy_flags then set_zf).
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }

    auto lo = ctx.mem.read_u64(addr.value());
    auto hi = ctx.mem.read_u64(addr.value() + 8);
    if (!lo || !hi) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 mem_lo = lo.value();
    u64 mem_hi = hi.value();
    u64 rax = ctx.cpu.gpr.get64(0);
    u64 rdx = ctx.cpu.gpr.get64(2);

    if (mem_lo == rax && mem_hi == rdx) {
        ctx.cpu.flags.set_zf(true);
        u64 rbx = ctx.cpu.gpr.get64(3);
        u64 rcx = ctx.cpu.gpr.get64(1);
        auto r1 = ctx.mem.write_u64(addr.value(), rbx);
        auto r2 = ctx.mem.write_u64(addr.value() + 8, rcx);
        if (!r1 || !r2) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
    } else {
        ctx.cpu.flags.set_zf(false);
        ctx.cpu.gpr.set64(0, mem_lo); // RAX
        ctx.cpu.gpr.set64(2, mem_hi); // RDX
    }

    return ExecResult::Continue;
}

// ============================================================================
// XADD - Exchange and Add
// ============================================================================

ExecResult exec_xadd(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    if (instr.operand_count < 2) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }

    u8 size = operand_size(instr.operands[0].type);

    auto rm_val = read_operand(ctx, instr, 0);
    auto reg_val = read_operand(ctx, instr, 1);
    if (!rm_val || !reg_val) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    u64 old_rm = rm_val.value();
    u64 sum = old_rm + reg_val.value();
    const u64 rflags_before = ctx.cpu.flags.rflags;

    // Update flags based on addition
    switch (size) {
        case 1: {
            u8 r = static_cast<u8>(sum);
            ctx.cpu.flags.update_flags_add(
                static_cast<u8>(old_rm),
                static_cast<u8>(reg_val.value()), r);
            sum = r;
            break;
        }
        case 2: {
            u16 r = static_cast<u16>(sum);
            ctx.cpu.flags.update_flags_add(
                static_cast<u16>(old_rm),
                static_cast<u16>(reg_val.value()), r);
            sum = r;
            break;
        }
        case 4: {
            u32 r = static_cast<u32>(sum);
            ctx.cpu.flags.update_flags_add(
                static_cast<u32>(old_rm),
                static_cast<u32>(reg_val.value()), r);
            sum = r;
            break;
        }
        default:
            ctx.cpu.flags.update_flags_add(old_rm, reg_val.value(), sum);
            break;
    }

    // Intel SDM Vol. 2 XADD: TEMP := SRC + DEST; SRC := DEST; DEST := TEMP.
    // Register destination: SRC is written first and DEST last, which is
    // observable for `xadd cl, cl` (the sum survives). Memory destination:
    // the store goes FIRST. Its address was formed from the registers as
    // they were before the instruction -- writing SRC first recomputed the
    // EA from an updated register (`xadd [rax], ah` changes RAX) -- and a
    // faulting store must commit nothing, including SRC.
    if (is_memory_operand(instr.operands[0].type)) {
        if (!write_operand(ctx, instr, 0, sum)) {
            ctx.cpu.flags.rflags = rflags_before;
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        if (!write_operand(ctx, instr, 1, old_rm)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }
    // reg ← old r/m value
    auto r1 = write_operand(ctx, instr, 1, old_rm);
    // r/m ← sum
    auto r2 = write_operand(ctx, instr, 0, sum);
    if (!r1 || !r2) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

// ============================================================================
// XCHG r/m, r — Exchange operands (modrm form)
// ============================================================================

ExecResult exec_xchg_rm(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    if (instr.operand_count < 2) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }

    auto val1 = read_operand(ctx, instr, 1);
    if (!val1) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    if (is_memory_operand(instr.operands[0].type)) {
        auto address = calc_effective_address(ctx, instr.operands[0].mem);
        if (!address) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const u8 size = operand_size(instr.operands[0].type);
        Byte* host = ctx.mem.host_ptr_store(address.value(), size);
        u64 previous = 0;
        if (host != nullptr &&
            reinterpret_cast<uintptr_t>(host) % size == 0) {
            switch (size) {
                case 1: {
                    std::atomic_ref<u8> value(*reinterpret_cast<u8*>(host));
                    previous = value.exchange(
                        static_cast<u8>(val1.value()),
                        std::memory_order_seq_cst);
                    break;
                }
                case 2: {
                    std::atomic_ref<u16> value(*reinterpret_cast<u16*>(host));
                    previous = value.exchange(
                        static_cast<u16>(val1.value()),
                        std::memory_order_seq_cst);
                    break;
                }
                case 4: {
                    std::atomic_ref<u32> value(*reinterpret_cast<u32*>(host));
                    previous = value.exchange(
                        static_cast<u32>(val1.value()),
                        std::memory_order_seq_cst);
                    break;
                }
                case 8: {
                    std::atomic_ref<u64> value(*reinterpret_cast<u64*>(host));
                    previous = value.exchange(
                        val1.value(), std::memory_order_seq_cst);
                    break;
                }
                default:
                    ctx.exception = CpuException::InvalidOpcode;
                    return ExecResult::Exception;
            }
            write_watch::record_store(address.value(), size);
        } else {
            auto val0 = read_operand(ctx, instr, 0);
            if (!val0 ||
                !write_operand(ctx, instr, 0, val1.value())) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            previous = val0.value();
        }
        if (!write_operand(ctx, instr, 1, previous)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }

    auto val0 = read_operand(ctx, instr, 0);
    if (!val0) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    // Swap values — no flag changes
    auto r0 = write_operand(ctx, instr, 0, val1.value());
    auto r1 = write_operand(ctx, instr, 1, val0.value());
    if (!r0 || !r1) {
        ctx.exception = CpuException::GeneralProtection;
        return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

// ============================================================================
// Flag manipulation: CLC, STC, CMC, CLD, STD, LAHF, SAHF
// ============================================================================

ExecResult exec_flag_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    // CLD/STD only touch DF. DF is outside STATUS_FLAGS_MASK, so a later
    // materialize of a pending FlagOp cannot clobber it — no settle needed.
    // CLC/STC/CMC write only CF: keep the pending ALU FlagOp for ZF/SF/OF/PF
    // and record CF in the overlay. LAHF/SAHF still settle first.
    switch (instr.opcode) {
        case Opcode::CLD:
            ctx.cpu.flags.set_df(false);
            return ExecResult::Continue;

        case Opcode::STD:
            ctx.cpu.flags.set_df(true);
            return ExecResult::Continue;

        case Opcode::CLC:
            ctx.cpu.write_cf_only(false);
            return ExecResult::Continue;

        case Opcode::STC:
            ctx.cpu.write_cf_only(true);
            return ExecResult::Continue;

        case Opcode::CMC:
            ctx.cpu.write_cf_only(!ctx.cpu.lazy_cf());
            return ExecResult::Continue;

        default:
            break;
    }

    ctx.cpu.materialize_lazy_flags();

    switch (instr.opcode) {

        case Opcode::LAHF: {
            // AH ← low byte of RFLAGS (SF:ZF:0:AF:0:PF:1:CF)
            u8 flags_byte = static_cast<u8>(ctx.cpu.flags.rflags & 0xFF);
            // AH is the high byte of AX (register 0)
            ctx.cpu.gpr.rax = (ctx.cpu.gpr.rax & ~static_cast<u64>(0xFF00))
                            | (static_cast<u64>(flags_byte) << 8);
            return ExecResult::Continue;
        }

        case Opcode::SAHF: {
            // Low byte of RFLAGS ← AH (only bits 7,6,4,2,0 — SF,ZF,AF,PF,CF)
            u8 ah = static_cast<u8>((ctx.cpu.gpr.rax >> 8) & 0xFF);
            constexpr u64 sahf_mask = 0xD5;  // bits 7,6,4,2,0 = 1101_0101
            ctx.cpu.flags.rflags = (ctx.cpu.flags.rflags & ~sahf_mask)
                                 | (static_cast<u64>(ah) & sahf_mask)
                                 | 0x02;  // Bit 1 always set
            return ExecResult::Continue;
        }

        default:
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
