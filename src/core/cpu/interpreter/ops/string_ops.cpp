// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// String instruction execution (MOVS, STOS, LODS, CMPS, SCAS, CLD, STD)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::cpu::ops {

namespace {

u8 string_element_size(Opcode op) noexcept {
    switch (op) {
        case Opcode::MOVSB: case Opcode::STOSB: case Opcode::CMPSB:
        case Opcode::SCASB: case Opcode::LODSB: return 1;
        case Opcode::MOVSW: case Opcode::STOSW: case Opcode::CMPSW:
        case Opcode::SCASW: case Opcode::LODSW: return 2;
        case Opcode::MOVSD_STR: case Opcode::STOSD: case Opcode::CMPSD_STR:
        case Opcode::SCASD: case Opcode::LODSD: return 4;
        case Opcode::MOVSQ: case Opcode::STOSQ: case Opcode::CMPSQ:
        case Opcode::SCASQ: case Opcode::LODSQ: return 8;
        default: return 1;
    }
}

i64 step_direction(ExecContext& ctx, u8 size) noexcept {
    return ctx.cpu.flags.df() ? -static_cast<i64>(size) : static_cast<i64>(size);
}

/// True when a repeat prefix applies to MOVS, STOS or LODS.
///
/// F3 (REP) and F2 (REPNZ) are the same instruction prefix; the SDM gives them
/// separate mnemonics only because CMPS and SCAS additionally terminate on ZF.
/// For MOVS/STOS/LODS there is no comparison to terminate on, so both forms
/// mean the same thing: repeat RCX times. Honouring only F3 here made
/// `REPNE MOVSB` copy nothing at all, which a real core does not do -- see
/// tests/cpu/test_hardware_differential.cpp, which compares against one.
bool string_repeats(const DecodedInstruction& instr) noexcept {
    return instr.prefixes.rep || instr.prefixes.repne;
}

} // anonymous namespace

ExecResult exec_movs(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 size = string_element_size(instr.opcode);
    const i64 step = step_direction(ctx, size);

    constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    if (string_repeats(instr)) {
        const bool wide = ctx.cpu.is_64bit();
        u64 count = wide ? ctx.cpu.gpr.get64(RCX) : ctx.cpu.gpr.get32(RCX);
        if (count == 0) return ExecResult::Continue;

        u64 src_addr = wide ? ctx.cpu.gpr.get64(RSI) : ctx.cpu.gpr.get32(RSI);
        u64 dst_addr = wide ? ctx.cpu.gpr.get64(RDI) : ctx.cpu.gpr.get32(RDI);

        // Bulk fast-path: forward direction. Probe source first so a failed
        // src span does not mark dest write-watch. probe_access accepts a
        // contiguous host mapping that crosses a protection run; host_ptr
        // does not.
        usize total_bytes = static_cast<usize>(count) * size;
        if (!ctx.cpu.flags.df()) {
            const auto src_info = ctx.mem.probe_access(
                static_cast<GuestAddr>(src_addr), total_bytes);
            Byte* dst_host = (src_info.host_ptr && src_info.readable)
                                 ? ctx.mem.host_ptr_store(
                                       static_cast<GuestAddr>(dst_addr),
                                       total_bytes)
                                 : nullptr;
            if (src_info.host_ptr && src_info.readable && dst_host) {
                const bool overlap = (dst_addr > src_addr) && (dst_addr < src_addr + total_bytes);
                if (!overlap) {
                    std::memmove(dst_host, src_info.host_ptr, total_bytes);
                } else {
                    const usize distance = static_cast<usize>(dst_addr - src_addr);
                    // Seed with the initial non-overlapping prefix, then
                    // repeatedly copy from the bytes we just produced.
                    usize done = std::min(distance, total_bytes);
                    std::memcpy(dst_host, src_info.host_ptr, done);
                    while (done < total_bytes) {
                        const usize chunk = std::min(done, total_bytes - done);
                        std::memcpy(dst_host + done, dst_host, chunk);
                        done += chunk;
                    }
                }
                if (wide) {
                    ctx.cpu.gpr.set64(RSI, src_addr + total_bytes);
                    ctx.cpu.gpr.set64(RDI, dst_addr + total_bytes);
                    ctx.cpu.gpr.set64(RCX, 0);
                } else {
                    ctx.cpu.gpr.set32(RSI, static_cast<u32>(src_addr + total_bytes));
                    ctx.cpu.gpr.set32(RDI, static_cast<u32>(dst_addr + total_bytes));
                    ctx.cpu.gpr.set32(RCX, 0);
                }
                return ExecResult::Continue;
            }
        }

        // Element-by-element fallback (backward direction or cross-region)
        while ((wide ? ctx.cpu.gpr.get64(RCX) : ctx.cpu.gpr.get32(RCX)) > 0) {
            src_addr = wide ? ctx.cpu.gpr.get64(RSI) : ctx.cpu.gpr.get32(RSI);
            dst_addr = wide ? ctx.cpu.gpr.get64(RDI) : ctx.cpu.gpr.get32(RDI);

            auto val = read_memory(ctx, src_addr, size);
            if (!val) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto wr = write_memory(ctx, dst_addr, val.value(), size);
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }

            const u64 next_src =
                static_cast<u64>(static_cast<i64>(src_addr) + step);
            const u64 next_dst =
                static_cast<u64>(static_cast<i64>(dst_addr) + step);
            if (wide) {
                ctx.cpu.gpr.set64(RSI, next_src);
                ctx.cpu.gpr.set64(RDI, next_dst);
                ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
            } else {
                ctx.cpu.gpr.set32(RSI, static_cast<u32>(next_src));
                ctx.cpu.gpr.set32(RDI, static_cast<u32>(next_dst));
                ctx.cpu.gpr.set32(RCX, ctx.cpu.gpr.get32(RCX) - 1);
            }
        }
    } else {
        u64 src_addr = ctx.cpu.gpr.get64(RSI);
        u64 dst_addr = ctx.cpu.gpr.get64(RDI);

        auto val = read_memory(ctx, src_addr, size);
        if (!val) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        auto wr = write_memory(ctx, dst_addr, val.value(), size);
        if (!wr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }

        ctx.cpu.gpr.set64(RSI, static_cast<u64>(static_cast<i64>(src_addr) + step));
        ctx.cpu.gpr.set64(RDI, static_cast<u64>(static_cast<i64>(dst_addr) + step));
    }

    return ExecResult::Continue;
}

ExecResult exec_stos(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 size = string_element_size(instr.opcode);
    const i64 step = step_direction(ctx, size);

    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    u64 ax_val = ctx.cpu.gpr.get64(RAX);

    if (string_repeats(instr)) {
        u64 count = ctx.cpu.gpr.get64(RCX);
        if (count == 0) return ExecResult::Continue;

        u64 dst_addr = ctx.cpu.gpr.get64(RDI);

        // Bulk fast-path: forward direction, get host pointer
        if (!ctx.cpu.flags.df()) {
            usize total_bytes = static_cast<usize>(count) * size;
            Byte* dst_host = ctx.mem.host_ptr_store(
                static_cast<GuestAddr>(dst_addr), total_bytes);
            if (dst_host) {
                if (size == 1) {
                    std::memset(dst_host, static_cast<int>(ax_val & 0xFF), total_bytes);
                } else {
                    // Write one element then expand exponentially.
                    std::memcpy(dst_host, &ax_val, size);
                    usize done = size;
                    while (done < total_bytes) {
                        const usize chunk = std::min(done, total_bytes - done);
                        std::memcpy(dst_host + done, dst_host, chunk);
                        done += chunk;
                    }
                }
                ctx.cpu.gpr.set64(RDI, dst_addr + total_bytes);
                ctx.cpu.gpr.set64(RCX, 0);
                return ExecResult::Continue;
            }
        }

        // Element-by-element fallback
        while (ctx.cpu.gpr.get64(RCX) > 0) {
            dst_addr = ctx.cpu.gpr.get64(RDI);

            auto wr = write_memory(ctx, dst_addr, ax_val, size);
            if (!wr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }

            ctx.cpu.gpr.set64(RDI, static_cast<u64>(static_cast<i64>(dst_addr) + step));
            ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
        }
    } else {
        u64 dst_addr = ctx.cpu.gpr.get64(RDI);

        auto wr = write_memory(ctx, dst_addr, ax_val, size);
        if (!wr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }

        ctx.cpu.gpr.set64(RDI, static_cast<u64>(static_cast<i64>(dst_addr) + step));
    }

    return ExecResult::Continue;
}

ExecResult exec_lods(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 size = string_element_size(instr.opcode);
    const i64 step = step_direction(ctx, size);

    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
    constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    if (string_repeats(instr)) {
        while (ctx.cpu.gpr.get64(RCX) > 0) {
            u64 src_addr = ctx.cpu.gpr.get64(RSI);

            auto val = read_memory(ctx, src_addr, size);
            if (!val) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }

            // Write to AL/AX/EAX/RAX based on size
            switch (size) {
                case 1: ctx.cpu.gpr.set8l(RAX, static_cast<u8>(val.value())); break;
                case 2: ctx.cpu.gpr.set16(RAX, static_cast<u16>(val.value())); break;
                case 4: ctx.cpu.gpr.set32(RAX, static_cast<u32>(val.value())); break;
                case 8: ctx.cpu.gpr.set64(RAX, val.value()); break;
                default: break;
            }

            ctx.cpu.gpr.set64(RSI, static_cast<u64>(static_cast<i64>(src_addr) + step));
            ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
        }
    } else {
        u64 src_addr = ctx.cpu.gpr.get64(RSI);

        auto val = read_memory(ctx, src_addr, size);
        if (!val) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }

        switch (size) {
            case 1: ctx.cpu.gpr.set8l(RAX, static_cast<u8>(val.value())); break;
            case 2: ctx.cpu.gpr.set16(RAX, static_cast<u16>(val.value())); break;
            case 4: ctx.cpu.gpr.set32(RAX, static_cast<u32>(val.value())); break;
            case 8: ctx.cpu.gpr.set64(RAX, val.value()); break;
            default: break;
        }

        ctx.cpu.gpr.set64(RSI, static_cast<u64>(static_cast<i64>(src_addr) + step));
    }

    return ExecResult::Continue;
}

ExecResult exec_cmps(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 size = string_element_size(instr.opcode);
    const i64 step = step_direction(ctx, size);

    constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    auto do_one_cmps = [&]() -> bool {
        u64 src_addr = ctx.cpu.gpr.get64(RSI);
        u64 dst_addr = ctx.cpu.gpr.get64(RDI);

        auto a = read_memory(ctx, src_addr, size);
        auto b = read_memory(ctx, dst_addr, size);
        if (!a || !b) {
            ctx.exception = CpuException::GeneralProtection;
            return false;
        }

        u64 va = a.value();
        u64 vb = b.value();
        u64 result = va - vb;

        switch (size) {
            case 1: ctx.cpu.flags.update_flags_sub(static_cast<u8>(va), static_cast<u8>(vb), static_cast<u8>(result)); break;
            case 2: ctx.cpu.flags.update_flags_sub(static_cast<u16>(va), static_cast<u16>(vb), static_cast<u16>(result)); break;
            case 4: ctx.cpu.flags.update_flags_sub(static_cast<u32>(va), static_cast<u32>(vb), static_cast<u32>(result)); break;
            case 8: ctx.cpu.flags.update_flags_sub(va, vb, result); break;
            default: break;
        }

        ctx.cpu.gpr.set64(RSI, static_cast<u64>(static_cast<i64>(src_addr) + step));
        ctx.cpu.gpr.set64(RDI, static_cast<u64>(static_cast<i64>(dst_addr) + step));
        return true;
    };

    if (instr.prefixes.rep) {
        // REPE/REPZ: loop while RCX > 0 AND ZF == 1
        while (ctx.cpu.gpr.get64(RCX) > 0) {
            if (!do_one_cmps()) return ExecResult::Exception;
            ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
            if (!ctx.cpu.flags.zf()) break;  // Stop at first mismatch
        }
    } else if (instr.prefixes.repne) {
        // REPNE/REPNZ: loop while RCX > 0 AND ZF == 0
        while (ctx.cpu.gpr.get64(RCX) > 0) {
            if (!do_one_cmps()) return ExecResult::Exception;
            ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
            if (ctx.cpu.flags.zf()) break;  // Stop at first match
        }
    } else {
        // No prefix: single iteration
        if (!do_one_cmps()) return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

ExecResult exec_scas(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const u8 size = string_element_size(instr.opcode);
    const i64 step = step_direction(ctx, size);

    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    auto do_one_scas = [&]() -> bool {
        u64 dst_addr = ctx.cpu.gpr.get64(RDI);

        auto b = read_memory(ctx, dst_addr, size);
        if (!b) {
            ctx.exception = CpuException::GeneralProtection;
            return false;
        }

        u64 va = 0;
        switch (size) {
            case 1: va = ctx.cpu.gpr.get8l(RAX); break;
            case 2: va = ctx.cpu.gpr.get16(RAX); break;
            case 4: va = ctx.cpu.gpr.get32(RAX); break;
            case 8: va = ctx.cpu.gpr.get64(RAX); break;
            default: break;
        }

        u64 vb = b.value();
        u64 result = va - vb;

        switch (size) {
            case 1: ctx.cpu.flags.update_flags_sub(static_cast<u8>(va), static_cast<u8>(vb), static_cast<u8>(result)); break;
            case 2: ctx.cpu.flags.update_flags_sub(static_cast<u16>(va), static_cast<u16>(vb), static_cast<u16>(result)); break;
            case 4: ctx.cpu.flags.update_flags_sub(static_cast<u32>(va), static_cast<u32>(vb), static_cast<u32>(result)); break;
            case 8: ctx.cpu.flags.update_flags_sub(va, vb, result); break;
            default: break;
        }

        ctx.cpu.gpr.set64(RDI, static_cast<u64>(static_cast<i64>(dst_addr) + step));
        return true;
    };

    if (instr.prefixes.rep) {
        // REPE/REPZ SCAS: loop while RCX > 0 AND ZF == 1
        u64 count = ctx.cpu.gpr.get64(RCX);
        if (count == 0) return ExecResult::Continue;

        // Bulk fast-path for forward REPE SCASB: find first non-match
        if (size == 1 && !ctx.cpu.flags.df()) {
            u64 dst_addr = ctx.cpu.gpr.get64(RDI);
            usize total = static_cast<usize>(count);
            const Byte* dp = ctx.mem.host_ptr(static_cast<GuestAddr>(dst_addr), total);
            if (dp) {
                u8 needle = ctx.cpu.gpr.get8l(RAX);
                usize i = 0;
                for (; i < total; ++i) {
                    if (dp[i] != needle) break;
                }
                if (i == total) {
                    u8 va = needle, vb = dp[total - 1];
                    ctx.cpu.flags.update_flags_sub(va, vb, static_cast<u8>(va - vb));
                    ctx.cpu.gpr.set64(RDI, dst_addr + total);
                    ctx.cpu.gpr.set64(RCX, 0);
                } else {
                    u8 va = needle, vb = dp[i];
                    ctx.cpu.flags.update_flags_sub(va, vb, static_cast<u8>(va - vb));
                    ctx.cpu.gpr.set64(RDI, dst_addr + i + 1);
                    ctx.cpu.gpr.set64(RCX, count - i - 1);
                }
                return ExecResult::Continue;
            }
        }

        while (ctx.cpu.gpr.get64(RCX) > 0) {
            if (!do_one_scas()) return ExecResult::Exception;
            ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
            if (!ctx.cpu.flags.zf()) break;  // Stop at first mismatch
        }
    } else if (instr.prefixes.repne) {
        // REPNE/REPNZ SCAS: loop while RCX > 0 AND ZF == 0
        u64 count = ctx.cpu.gpr.get64(RCX);
        if (count == 0) return ExecResult::Continue;

        // Bulk fast-path for forward REPNE SCASB: find first match (strlen-like)
        if (size == 1 && !ctx.cpu.flags.df()) {
            u64 dst_addr = ctx.cpu.gpr.get64(RDI);
            usize total = static_cast<usize>(count);
            const Byte* dp = ctx.mem.host_ptr(static_cast<GuestAddr>(dst_addr), total);
            if (dp) {
                u8 needle = ctx.cpu.gpr.get8l(RAX);
                const void* found = ::memchr(dp, needle, total);
                if (found) {
                    usize i = static_cast<usize>(static_cast<const Byte*>(found) - dp);
                    u8 va = needle, vb = dp[i];
                    ctx.cpu.flags.update_flags_sub(va, vb, static_cast<u8>(va - vb));
                    ctx.cpu.gpr.set64(RDI, dst_addr + i + 1);
                    ctx.cpu.gpr.set64(RCX, count - i - 1);
                } else {
                    // No match found - compare last element
                    u8 va = needle, vb = dp[total - 1];
                    ctx.cpu.flags.update_flags_sub(va, vb, static_cast<u8>(va - vb));
                    ctx.cpu.gpr.set64(RDI, dst_addr + total);
                    ctx.cpu.gpr.set64(RCX, 0);
                }
                return ExecResult::Continue;
            }
        }

        while (ctx.cpu.gpr.get64(RCX) > 0) {
            if (!do_one_scas()) return ExecResult::Exception;
            ctx.cpu.gpr.set64(RCX, ctx.cpu.gpr.get64(RCX) - 1);
            if (ctx.cpu.flags.zf()) break;  // Stop at first match
        }
    } else {
        // No prefix: single iteration
        if (!do_one_scas()) return ExecResult::Exception;
    }

    return ExecResult::Continue;
}

} // namespace vitreo::cpu::ops
