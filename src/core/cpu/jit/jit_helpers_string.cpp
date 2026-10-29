// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_string.cpp
/// @brief JIT fast paths for REP-prefixed string operations.
///
/// REP MOVS/STOS and friends run a loop the JIT would otherwise step one
/// iteration at a time. These helpers run the whole loop in host code, which is
/// where most of the win is: a guest memcpy becomes one call rather than one
/// dispatch per element.
///
/// What that buys in speed it costs in care about guest memory. Each helper has
/// to cope with a range crossing a page the host cannot address contiguously,
/// and has to leave the direction flag, the counter and both pointers exactly
/// where the interpreter would have — a fast path that diverges is worse than
/// no fast path.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving.

#include "jit_helpers.hpp"
#include "../lock_prefix.hpp"
#include "../interpreter/interpreter.hpp"
#include "../interpreter/ops/exec_ops.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace vitreo::cpu::jit {

namespace {

// Intel SDM Vol. 2 "REP/REPE/REPZ..." with Vol. 3 paging: a string operation
// that touches an unmapped or unreadable page takes #PF on THAT element. The
// architectural contract has two halves and the helpers honoured neither:
//
//   * the fault must be reported — Windows turns it into
//     STATUS_ACCESS_VIOLATION, and a handler (or a probe loop deliberately
//     walking off a mapping) is entitled to see it. Returning quietly is a
//     fake success: the guest continues as though the whole walk completed;
//   * RSI/RDI/RCX must reflect the elements ALREADY processed, so the
//     instruction is restartable at the faulting element.
//
// Mirrors jit_lock_raise_av (jit_helpers_lock_internal.hpp), which took the
// same silent-return bug in the LOCK RMW helpers. Only CpuState is needed —
// pending_raise is drained by emit_exit_if_pending_raise at the call site.
void raise_string_av(CpuState* cpu, u64 addr, bool is_write) noexcept {
    if (cpu == nullptr || cpu->pending_raise.active) return;
    auto& pr = cpu->pending_raise;
    pr.active = true;
    pr.from_helper = true;
    pr.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
    pr.flags = 0;
    pr.num_args = 2;
    pr.args[0] = is_write ? 1u : 0u;
    pr.args[1] = addr;
}


constexpr u8 kRSI = static_cast<u8>(RegIndex::RSI);
constexpr u8 kRDI = static_cast<u8>(RegIndex::RDI);
constexpr u8 kRCX = static_cast<u8>(RegIndex::RCX);

struct RepMovsState {
    u64 src;
    u64 dst;
    u64 count;
    bool wide;
};

RepMovsState load_rep_movs(const CpuState* cpu) {
    RepMovsState s{};
    s.wide = cpu->is_64bit();
    if (s.wide) {
        s.src = cpu->gpr.get64(kRSI);
        s.dst = cpu->gpr.get64(kRDI);
        s.count = cpu->gpr.get64(kRCX);
    } else {
        // Intel SDM Vol. 2 MOVS/REP: AddressSize=32 uses ESI, EDI, ECX.
        s.src = cpu->gpr.get32(kRSI);
        s.dst = cpu->gpr.get32(kRDI);
        s.count = cpu->gpr.get32(kRCX);
    }
    return s;
}

void store_rep_movs(CpuState* cpu, const RepMovsState& s) {
    if (s.wide) {
        cpu->gpr.set64(kRSI, s.src);
        cpu->gpr.set64(kRDI, s.dst);
        cpu->gpr.set64(kRCX, s.count);
    } else {
        cpu->gpr.set32(kRSI, static_cast<u32>(s.src));
        cpu->gpr.set32(kRDI, static_cast<u32>(s.dst));
        cpu->gpr.set32(kRCX, static_cast<u32>(s.count));
    }
}

void copy_forward_overlap(Byte* dst_host, const Byte* src_host, usize total,
                          u64 src, u64 dst) {
    const bool overlap = (dst > src) && (dst < src + total);
    if (!overlap) {
        std::memmove(dst_host, src_host, total);
        return;
    }
    const usize distance = static_cast<usize>(dst - src);
    usize done = std::min(distance, total);
    std::memcpy(dst_host, src_host, done);
    while (done < total) {
        const usize chunk = std::min(done, total - done);
        std::memcpy(dst_host + done, dst_host, chunk);
        done += chunk;
    }
}

bool addr32_wraps(u64 addr, usize total) {
    return total != 0 &&
           addr > (0xFFFFFFFFull - (static_cast<u64>(total) - 1u));
}

// Probe source first. host_ptr rejects a span that crosses a protection run
// even when the host mapping is contiguous; probe_access accepts that range.
// Do not host_ptr_mut the dest until the source is usable — otherwise
// GetWriteWatch is marked for a copy that then falls back to a scalar loop.
bool try_bulk_forward_movs(GuestMemory* mem, RepMovsState& s, u8 elem) {
    if (s.count == 0) {
        return true;
    }
    if (s.count > (std::numeric_limits<usize>::max() / elem)) {
        return false;
    }
    const usize total = static_cast<usize>(s.count) * elem;
    if (!s.wide && (addr32_wraps(s.src, total) || addr32_wraps(s.dst, total))) {
        return false;
    }
    const auto src_info = mem->probe_access(static_cast<GuestAddr>(s.src), total);
    if (!src_info.host_ptr || !src_info.readable) {
        return false;
    }
    Byte* dst_host = mem->host_ptr_store(static_cast<GuestAddr>(s.dst), total);
    if (!dst_host) {
        return false;
    }
    copy_forward_overlap(dst_host, src_info.host_ptr, total, s.src, s.dst);
    if (store_ring_enabled()) mem_watch_bytes(s.dst, dst_host, total, "jit.rep.movs");
    s.src += total;
    s.dst += total;
    s.count = 0;
    return true;
}

bool try_chunked_forward_movs(GuestMemory* mem, RepMovsState& s, u8 elem) {
    while (s.count > 0) {
        const auto src_info =
            mem->probe_access(static_cast<GuestAddr>(s.src), elem);
        if (!src_info.host_ptr || !src_info.readable ||
            src_info.region_end <= s.src) {
            return false;
        }
        const auto dst_info =
            mem->probe_access(static_cast<GuestAddr>(s.dst), elem);
        if (!dst_info.host_ptr || !dst_info.writable ||
            dst_info.region_end <= s.dst) {
            return false;
        }
        u64 avail = std::min(src_info.region_end - s.src,
                             dst_info.region_end - s.dst);
        if (!s.wide) {
            avail = std::min(avail, 0x100000000ull - s.src);
            avail = std::min(avail, 0x100000000ull - s.dst);
        }
        avail -= avail % elem;
        if (avail < elem) {
            return false;
        }
        usize bytes = static_cast<usize>(
            std::min(avail, s.count * static_cast<u64>(elem)));
        bytes -= bytes % elem;
        Byte* dst_host =
            mem->host_ptr_store(static_cast<GuestAddr>(s.dst), bytes);
        if (!dst_host) {
            return false;
        }
        copy_forward_overlap(dst_host, src_info.host_ptr, bytes, s.src, s.dst);
        if (store_ring_enabled()) mem_watch_bytes(s.dst, dst_host, bytes, "jit.rep.movs.chunk");
        s.src += bytes;
        s.dst += bytes;
        s.count -= bytes / elem;
    }
    return true;
}

void scalar_forward_or_back_movs(CpuState* cpu, GuestMemory* mem,
                                 RepMovsState& s, i64 step, u8 elem) {
    while (s.count > 0) {
        bool ok = false;
        if (elem == 8) {
            auto val = mem->read_u64(static_cast<GuestAddr>(s.src));
            if (val.is_ok()) {
                ok = mem->write_u64(static_cast<GuestAddr>(s.dst), val.value())
                         .is_ok();
            }
        } else if (elem == 4) {
            auto val = mem->read_u32(static_cast<GuestAddr>(s.src));
            if (val.is_ok()) {
                ok = mem->write_u32(static_cast<GuestAddr>(s.dst), val.value())
                         .is_ok();
            }
        } else if (elem == 2) {
            auto val = mem->read_u16(static_cast<GuestAddr>(s.src));
            if (val.is_ok()) {
                ok = mem->write_u16(static_cast<GuestAddr>(s.dst), val.value())
                         .is_ok();
            }
        } else {
            auto val = mem->read_u8(static_cast<GuestAddr>(s.src));
            if (val.is_ok()) {
                ok = mem->write_u8(static_cast<GuestAddr>(s.dst), val.value())
                         .is_ok();
            }
        }
        if (!ok) {
            // Commit the elements already moved so the instruction restarts at
            // the faulting one, then report the fault. The old comment claimed
            // this "matched #GP stopping the string op"; stopping is only half
            // of it — an unmapped page is #PF and the guest must see
            // STATUS_ACCESS_VIOLATION, not a short copy that looks complete.
            const bool read_failed =
                !mem->probe_access(static_cast<GuestAddr>(s.src), elem)
                     .readable;
            store_rep_movs(cpu, s);
            raise_string_av(cpu, read_failed ? s.src : s.dst,
                            /*is_write=*/!read_failed);
            return;
        }
        if (s.wide) {
            s.src = static_cast<u64>(static_cast<i64>(s.src) + step);
            s.dst = static_cast<u64>(static_cast<i64>(s.dst) + step);
        } else {
            s.src = static_cast<u32>(static_cast<u32>(s.src) +
                                     static_cast<u32>(step));
            s.dst = static_cast<u32>(static_cast<u32>(s.dst) +
                                     static_cast<u32>(step));
        }
        --s.count;
    }
    store_rep_movs(cpu, s);
}

void run_rep_movs(CpuState* cpu, GuestMemory* mem, u8 elem) {
    const bool df = (cpu->flags.rflags >> 10) & 1;
    RepMovsState s = load_rep_movs(cpu);
    if (s.count == 0) {
        return;
    }
    if (!df) {
        // Probe source first (MOVSD write-watch pattern): do not host_ptr_mut
        // the dest until the source span is usable.
        if (try_bulk_forward_movs(mem, s, elem) ||
            try_chunked_forward_movs(mem, s, elem)) {
            store_rep_movs(cpu, s);
            return;
        }
    } else if (s.count > 0) {
        const usize total = static_cast<usize>(s.count) * elem;
        if (s.wide || (!addr32_wraps(s.src, total) && !addr32_wraps(s.dst, total))) {
            const u64 src_low = s.src - (total - elem);
            const u64 dst_low = s.dst - (total - elem);
            const auto src_info =
                mem->probe_access(static_cast<GuestAddr>(src_low), total);
            Byte* dst_host = (src_info.host_ptr && src_info.readable)
                                 ? mem->host_ptr_store(
                                       static_cast<GuestAddr>(dst_low), total)
                                 : nullptr;
            if (src_info.host_ptr && src_info.readable && dst_host) {
                std::memmove(dst_host, src_info.host_ptr, total);
                if (store_ring_enabled()) mem_watch_bytes(dst_low, dst_host, total, "jit.rep.movs.backward");
                s.src = s.src - total;
                s.dst = s.dst - total;
                s.count = 0;
                store_rep_movs(cpu, s);
                return;
            }
        }
    }
    const i64 step = df ? -static_cast<i64>(elem) : static_cast<i64>(elem);
    scalar_forward_or_back_movs(cpu, mem, s, step, elem);
}

}  // namespace

extern "C" u32 jit_capture_cf(CpuState* cpu) {
    return cpu->lazy_cf() ? 1u : 0u;
}

extern "C" u32 jit_zero_fill_loop(CpuState* cpu, GuestMemory* mem,
                                   u32 dst_reg, u32 count_reg,
                                   u32 branch_rip, u32 fallthrough_rip) {
    if (dst_reg >= 16 || count_reg >= 16 || dst_reg == count_reg) return 0;

    const u32 count = cpu->gpr.get32(static_cast<u8>(count_reg));
    if (count == 0) return 0;

    const u32 dst = cpu->gpr.get32(static_cast<u8>(dst_reg));
    const u32 chunk = std::min(count, MAX_JIT_LOOP_ITERS);
    if (chunk > UINT32_MAX - dst) return 0;

    // Probe the whole chunk before changing memory or CPU state. On any edge
    // case, the generated code falls back to the scalar loop, preserving its
    // first-fault and partial-write behavior. Executable destinations are also
    // left scalar so self-modifying code remains instruction-granular.
    auto info = mem->probe_access(dst, chunk);
    if (!info.host_ptr || !info.writable || info.executable) return 0;

    vitreo::mem_watch_check(dst, chunk, 0, "jit.zero_fill_loop");
    // The fused memset writes straight through the host pointer, bypassing the
    // per-store recording every scalar path performs. Without this the pages a
    // REP STOS cleared read back CLEAN from GetWriteWatch -- the silent lie
    // that makes a guest GC free live objects.
    if (write_watch::any_active()) {
        write_watch::record_store(static_cast<GuestAddr>(dst), chunk);
    }
    std::memset(info.host_ptr, 0, chunk);
    diagnostics::perf_add_fused_memset(chunk);

    const u32 remaining = count - chunk;
    cpu->gpr.set32(static_cast<u8>(dst_reg), dst + chunk);
    cpu->gpr.set32(static_cast<u8>(count_reg), remaining);

    // The final scalar SUB in this chunk was `old_count - 1 = remaining`.
    // Preserve lazy flag representation so later consumers observe exactly
    // the same CF/PF/AF/ZF/SF/OF values as the unfused loop.
    cpu->set_flag_op(CpuState::FlagOp::Sub32);
    cpu->flag_src = 1;
    cpu->flag_dst = remaining;
    cpu->jit_iters = chunk;
    cpu->rip = remaining == 0 ? fallthrough_rip : branch_rip;
    return 1;
}

namespace {
u64 rep_count(const CpuState* cpu) {
    return cpu->is_64bit() ? cpu->gpr.get64(kRCX) : cpu->gpr.get32(kRCX);
}

void note_rep_movs(CpuState* cpu, GuestMemory* mem, u8 elem) {
    const u64 before = rep_count(cpu);
    run_rep_movs(cpu, mem, elem);
    const u64 after = rep_count(cpu);
    const u64 done = before > after ? (before - after) : 0;
    diagnostics::perf_add_rep_movs(done * elem);
}
}  // namespace

extern "C" void jit_rep_movsd(CpuState* cpu, GuestMemory* mem) {
    note_rep_movs(cpu, mem, 4);
}

extern "C" void jit_rep_movsb(CpuState* cpu, GuestMemory* mem) {
    note_rep_movs(cpu, mem, 1);
}

// Intel SDM Vol. 2 MOVS/REP: AddressSize=64 uses RSI/RDI/RCX; copy 8 bytes
// each iteration and adjust RSI/RDI by ±8 according to DF.
extern "C" void jit_rep_movsq(CpuState* cpu, GuestMemory* mem) {
    note_rep_movs(cpu, mem, 8);
}


extern "C" void jit_rep_stosd(CpuState* cpu, GuestMemory* mem) {
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);

    // DF is outside STATUS_FLAGS_MASK — never written by FlagOp materialize.
    bool df = (cpu->flags.rflags >> 10) & 1;
    i64 step = df ? -4 : 4;
    const bool wide = cpu->is_64bit();

    u64 count = wide ? cpu->gpr.get64(RCX) : cpu->gpr.get32(RCX);
    if (count == 0) return;
    diagnostics::perf_add_rep_stos(count * 4);

    u32 val = static_cast<u32>(cpu->gpr.get64(RAX));
    u64 dst = wide ? cpu->gpr.get64(RDI) : cpu->gpr.get32(RDI);

    if (!df) {
        usize total = static_cast<usize>(count) * 4;
        Byte* dst_host = mem->host_ptr_store(
            static_cast<GuestAddr>(dst), total);
        if (dst_host) {
            if (val == 0) {
                std::memset(dst_host, 0, total);
            } else {
                u32* ptr = reinterpret_cast<u32*>(dst_host);
                for (usize i = 0; i < count; ++i)
                    ptr[i] = val;
            }
            if (store_ring_enabled()) mem_watch_bytes(dst, dst_host, total, "jit.rep.stos");
            if (wide) {
                cpu->gpr.set64(RDI, dst + total);
                cpu->gpr.set64(RCX, 0);
            } else {
                cpu->gpr.set32(RDI, static_cast<u32>(dst + total));
                cpu->gpr.set32(RCX, 0);
            }
            return;
        }
    }

    while (count > 0) {
        // The result was discarded here. A REP STOS into an unmapped or
        // read-only page then wrote nothing, raised nothing, and fell through
        // to clear RCX -- indistinguishable from a completed fill. Commit the
        // elements actually stored and report the fault (Intel SDM Vol. 2
        // STOS with Vol. 3 paging: #PF on the faulting element).
        if (!mem->write_u32(static_cast<GuestAddr>(dst), val).is_ok()) {
            if (wide) {
                cpu->gpr.set64(RDI, dst);
                cpu->gpr.set64(RCX, count);
            } else {
                cpu->gpr.set32(RDI, static_cast<u32>(dst));
                cpu->gpr.set32(RCX, static_cast<u32>(count));
            }
            raise_string_av(cpu, dst, /*is_write=*/true);
            return;
        }
        dst = wide ? static_cast<u64>(static_cast<i64>(dst) + step)
                   : static_cast<u32>(static_cast<u32>(dst) +
                                      static_cast<u32>(step));
        --count;
    }
    if (wide) {
        cpu->gpr.set64(RDI, dst);
        cpu->gpr.set64(RCX, 0);
    } else {
        cpu->gpr.set32(RDI, static_cast<u32>(dst));
        cpu->gpr.set32(RCX, 0);
    }
}

// Intel SDM Vol. 2 STOS/REP: AddressSize=64 uses RDI and RCX; store RAX
// (8 bytes) each iteration and adjust RDI by ±8 according to DF.
extern "C" void jit_rep_stosq(CpuState* cpu, GuestMemory* mem) {
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);

    bool df = (cpu->flags.rflags >> 10) & 1;
    i64 step = df ? -8 : 8;

    u64 count = cpu->gpr.get64(RCX);
    if (count == 0) return;
    diagnostics::perf_add_rep_stos(count * 8);

    u64 val = cpu->gpr.get64(RAX);
    u64 dst = cpu->gpr.get64(RDI);

    if (!df) {
        usize total = static_cast<usize>(count) * 8;
        Byte* dst_host = mem->host_ptr_store(
            static_cast<GuestAddr>(dst), total);
        if (dst_host) {
            if (val == 0) {
                std::memset(dst_host, 0, total);
            } else if ((reinterpret_cast<uintptr_t>(dst_host) & 7u) == 0) {
                u64* ptr = reinterpret_cast<u64*>(dst_host);
                for (usize i = 0; i < count; ++i)
                    ptr[i] = val;
            } else {
                for (usize i = 0; i < count; ++i) {
                    std::memcpy(dst_host + i * 8, &val, 8);
                }
            }
            if (store_ring_enabled()) mem_watch_bytes(dst, dst_host, total, "jit.rep.stos");
            cpu->gpr.set64(RDI, dst + total);
            cpu->gpr.set64(RCX, 0);
            return;
        }
    }

    while (count > 0) {
        // The result was discarded here. A REP STOS into an unmapped or
        // read-only page then wrote nothing, raised nothing, and fell through
        // to clear RCX -- indistinguishable from a completed fill. Commit the
        // elements actually stored and report the fault (Intel SDM Vol. 2
        // STOS with Vol. 3 paging: #PF on the faulting element).
        if (!mem->write_u64(static_cast<GuestAddr>(dst), val).is_ok()) {
            cpu->gpr.set64(RDI, dst);
            cpu->gpr.set64(RCX, count);
            raise_string_av(cpu, dst, /*is_write=*/true);
            return;
        }
        dst = static_cast<u64>(static_cast<i64>(dst) + step);
        --count;
    }
    cpu->gpr.set64(RDI, dst);
    cpu->gpr.set64(RCX, 0);
}

// ----------------------------------------------------------------------------
// Additional REP-prefix string-op fast paths.
//
// Like jit_rep_movsb / jit_rep_movsd / jit_rep_stosd above, these pull a
// host pointer for the source/destination region and call the libc
// memset/memcmp/memchr family.  Each falls back to a per-element guest
// memory loop only when host_ptr is unavailable (e.g. MMIO).
//
// Flag semantics for CMPS / SCAS:  After a REP CMPS / REP SCAS the EFLAGS
// must look as if the *last* byte comparison ran a CMP.  We compute the
// canonical EFLAGS via update_flags_sub<u8>() and clear lazy state.
// ----------------------------------------------------------------------------

extern "C" void jit_rep_stosb(CpuState* cpu, GuestMemory* mem) {
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);

    // DF is outside STATUS_FLAGS_MASK — never written by FlagOp materialize.
    bool df = (cpu->flags.rflags >> 10) & 1;

    u64 count = cpu->gpr.get64(RCX);
    if (count == 0) return;
    diagnostics::perf_add_rep_stos(count);

    u8  val = static_cast<u8>(cpu->gpr.get64(RAX));
    u64 dst = cpu->gpr.get64(RDI);

    if (!df) {
        usize total = static_cast<usize>(count);
        Byte* dst_host = mem->host_ptr_store(
            static_cast<GuestAddr>(dst), total);
        if (dst_host) {
            std::memset(dst_host, val, total);
            if (store_ring_enabled()) mem_watch_bytes(dst, dst_host, total, "jit.rep.stos");
            cpu->gpr.set64(RDI, dst + total);
            cpu->gpr.set64(RCX, 0);
            return;
        }
    }

    i64 step = df ? -1 : 1;
    while (count > 0) {
        // The result was discarded here. A REP STOS into an unmapped or
        // read-only page then wrote nothing, raised nothing, and fell through
        // to clear RCX -- indistinguishable from a completed fill. Commit the
        // elements actually stored and report the fault (Intel SDM Vol. 2
        // STOS with Vol. 3 paging: #PF on the faulting element).
        if (!mem->write_u8(static_cast<GuestAddr>(dst), val).is_ok()) {
            cpu->gpr.set64(RDI, dst);
            cpu->gpr.set64(RCX, count);
            raise_string_av(cpu, dst, /*is_write=*/true);
            return;
        }
        dst = static_cast<u64>(static_cast<i64>(dst) + step);
        --count;
    }
    cpu->gpr.set64(RDI, dst);
    cpu->gpr.set64(RCX, 0);
}

extern "C" void jit_rep_stosw(CpuState* cpu, GuestMemory* mem) {
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);

    // DF is outside STATUS_FLAGS_MASK — never written by FlagOp materialize.
    bool df = (cpu->flags.rflags >> 10) & 1;

    u64 count = cpu->gpr.get64(RCX);
    if (count == 0) return;
    diagnostics::perf_add_rep_stos(count * 2);

    u16 val = static_cast<u16>(cpu->gpr.get64(RAX));
    u64 dst = cpu->gpr.get64(RDI);

    if (!df) {
        usize total = static_cast<usize>(count) * 2;
        Byte* dst_host = mem->host_ptr_store(
            static_cast<GuestAddr>(dst), total);
        if (dst_host) {
            // host_ptr_mut may not be aligned; use byte-wise assembly of u16.
            // In practice REP STOSW dst is aligned, but be safe.
            if ((reinterpret_cast<uintptr_t>(dst_host) & 1u) == 0) {
                u16* p = reinterpret_cast<u16*>(dst_host);
                for (usize i = 0; i < count; ++i) p[i] = val;
            } else {
                u8 lo = static_cast<u8>(val & 0xFFu);
                u8 hi = static_cast<u8>((val >> 8) & 0xFFu);
                for (usize i = 0; i < count; ++i) {
                    dst_host[2*i + 0] = lo;
                    dst_host[2*i + 1] = hi;
                }
            }
            if (store_ring_enabled()) mem_watch_bytes(dst, dst_host, total, "jit.rep.stos");
            cpu->gpr.set64(RDI, dst + total);
            cpu->gpr.set64(RCX, 0);
            return;
        }
    }

    i64 step = df ? -2 : 2;
    while (count > 0) {
        // The result was discarded here. A REP STOS into an unmapped or
        // read-only page then wrote nothing, raised nothing, and fell through
        // to clear RCX -- indistinguishable from a completed fill. Commit the
        // elements actually stored and report the fault (Intel SDM Vol. 2
        // STOS with Vol. 3 paging: #PF on the faulting element).
        if (!mem->write_u16(static_cast<GuestAddr>(dst), val).is_ok()) {
            cpu->gpr.set64(RDI, dst);
            cpu->gpr.set64(RCX, count);
            raise_string_av(cpu, dst, /*is_write=*/true);
            return;
        }
        dst = static_cast<u64>(static_cast<i64>(dst) + step);
        --count;
    }
    cpu->gpr.set64(RDI, dst);
    cpu->gpr.set64(RCX, 0);
}

extern "C" void jit_rep_movsw(CpuState* cpu, GuestMemory* mem) {
    run_rep_movs(cpu, mem, 2);
}

namespace {

void advance_str_addr(u64& addr, i64 step, bool wide) {
    if (wide) {
        addr = static_cast<u64>(static_cast<i64>(addr) + step);
    } else {
        // AddressSize=32: ESI/EDI wrap in 32 bits (Intel SDM Vol. 2).
        addr = static_cast<u32>(static_cast<u32>(addr) + static_cast<u32>(step));
    }
}

bool read_str_elem(GuestMemory* mem, u64 addr, u8 elem, u64& out) {
    if (elem == 1) {
        auto v = mem->read_u8(static_cast<GuestAddr>(addr));
        if (!v.is_ok()) return false;
        out = v.value();
        return true;
    }
    if (elem == 2) {
        auto v = mem->read_u16(static_cast<GuestAddr>(addr));
        if (!v.is_ok()) return false;
        out = v.value();
        return true;
    }
    if (elem == 8) {
        auto v = mem->read_u64(static_cast<GuestAddr>(addr));
        if (!v.is_ok()) return false;
        out = v.value();
        return true;
    }
    auto v = mem->read_u32(static_cast<GuestAddr>(addr));
    if (!v.is_ok()) return false;
    out = v.value();
    return true;
}

u64 host_elem_at(const Byte* p, u8 elem) {
    if (elem == 1) return p[0];
    if (elem == 2) {
        u16 v = 0;
        std::memcpy(&v, p, 2);
        return v;
    }
    if (elem == 8) {
        u64 v = 0;
        std::memcpy(&v, p, 8);
        return v;
    }
    u32 v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

void apply_cmps_flags(CpuState* cpu, u64 va, u64 vb, u8 elem) {
    cpu->set_flag_op(CpuState::FlagOp::None);
    if (elem == 1) {
        const u8 a = static_cast<u8>(va);
        const u8 b = static_cast<u8>(vb);
        cpu->flags.update_flags_sub<u8>(a, b, static_cast<u8>(a - b));
    } else if (elem == 2) {
        const u16 a = static_cast<u16>(va);
        const u16 b = static_cast<u16>(vb);
        cpu->flags.update_flags_sub<u16>(a, b, static_cast<u16>(a - b));
    } else if (elem == 8) {
        cpu->flags.update_flags_sub<u64>(va, vb, va - vb);
    } else {
        const u32 a = static_cast<u32>(va);
        const u32 b = static_cast<u32>(vb);
        cpu->flags.update_flags_sub<u32>(a, b, static_cast<u32>(a - b));
    }
}

// REPE: stop when ZF=0 (mismatch). REPNE: stop when ZF=1 (match).
void run_rep_cmps(CpuState* cpu, GuestMemory* mem, u8 elem, bool repe) {
    const bool df = (cpu->flags.rflags >> 10) & 1;
    RepMovsState s = load_rep_movs(cpu);
    if (s.count == 0) return;

    u64 last_a = 0;
    u64 last_b = 0;
    bool finished = false;

    if (!df &&
        (s.wide ||
         (!addr32_wraps(s.src, static_cast<usize>(s.count) * elem) &&
          !addr32_wraps(s.dst, static_cast<usize>(s.count) * elem)))) {
        if (s.count > (std::numeric_limits<usize>::max() / elem)) {
            // Fall through to scalar.
        } else {
        const usize total_bytes = static_cast<usize>(s.count) * elem;
        const Byte* src_host =
            mem->host_ptr(static_cast<GuestAddr>(s.src), total_bytes);
        const Byte* dst_host =
            mem->host_ptr(static_cast<GuestAddr>(s.dst), total_bytes);
        if (src_host && dst_host) {
            usize i = 0;
            for (; i < s.count; ++i) {
                const u64 a = host_elem_at(src_host + i * elem, elem);
                const u64 b = host_elem_at(dst_host + i * elem, elem);
                last_a = a;
                last_b = b;
                const bool eq = (a == b);
                if (repe ? !eq : eq) {
                    ++i; // advance past the terminating element
                    break;
                }
            }
            s.src += i * elem;
            s.dst += i * elem;
            s.count -= i;
            store_rep_movs(cpu, s);
            finished = true;
        }
        }
    }

    if (!finished) {
        const i64 step = df ? -static_cast<i64>(elem) : static_cast<i64>(elem);
        while (s.count > 0) {
            u64 a = 0;
            u64 b = 0;
            // A failed read is a FAULT, not a loop terminator. Committing the
            // walk so far and raising is what makes the instruction
            // restartable at the faulting element.
            if (!read_str_elem(mem, s.src, elem, a)) {
                store_rep_movs(cpu, s);
                raise_string_av(cpu, s.src, /*is_write=*/false);
                apply_cmps_flags(cpu, last_a, last_b, elem);
                return;
            }
            if (!read_str_elem(mem, s.dst, elem, b)) {
                store_rep_movs(cpu, s);
                raise_string_av(cpu, s.dst, /*is_write=*/false);
                apply_cmps_flags(cpu, last_a, last_b, elem);
                return;
            }
            last_a = a;
            last_b = b;
            advance_str_addr(s.src, step, s.wide);
            advance_str_addr(s.dst, step, s.wide);
            --s.count;
            const bool eq = (a == b);
            if (repe ? !eq : eq) break;
        }
        store_rep_movs(cpu, s);
    }

    apply_cmps_flags(cpu, last_a, last_b, elem);
}

u64 scas_needle(const CpuState* cpu, u8 elem) {
    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
    if (elem == 1) return cpu->gpr.get8l(RAX);
    if (elem == 2) return cpu->gpr.get16(RAX);
    if (elem == 8) return cpu->gpr.get64(RAX);
    return cpu->gpr.get32(RAX);
}

void store_scas_state(CpuState* cpu, u64 dst, u64 count, bool wide) {
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
    if (wide) {
        cpu->gpr.set64(RDI, dst);
        cpu->gpr.set64(RCX, count);
    } else {
        cpu->gpr.set32(RDI, static_cast<u32>(dst));
        cpu->gpr.set32(RCX, static_cast<u32>(count));
    }
}

void run_rep_scas(CpuState* cpu, GuestMemory* mem, u8 elem, bool repe) {
    constexpr u8 RDI = static_cast<u8>(RegIndex::RDI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    const bool df = (cpu->flags.rflags >> 10) & 1;
    const bool wide = cpu->is_64bit();
    u64 count = wide ? cpu->gpr.get64(RCX) : cpu->gpr.get32(RCX);
    if (count == 0) return;

    const u64 needle = scas_needle(cpu, elem);
    u64 dst = wide ? cpu->gpr.get64(RDI) : cpu->gpr.get32(RDI);
    u64 last = 0;
    bool finished = false;

    if (!df) {
        // Chunked host scan — strlen-style sites often pass ECX=0xFFFFFFFF.
        constexpr usize CHUNK_ELEMS = 16384;
        u64 scanned = 0;
        while (scanned < count) {
            const usize this_n = static_cast<usize>(
                std::min<u64>(CHUNK_ELEMS, count - scanned));
            const usize this_bytes = this_n * elem;
            if (!wide && addr32_wraps(dst + scanned * elem, this_bytes)) {
                break;
            }
            const Byte* host = mem->host_ptr(
                static_cast<GuestAddr>(dst + scanned * elem), this_bytes);
            if (!host) break;

            usize i = 0;
            for (; i < this_n; ++i) {
                last = host_elem_at(host + i * elem, elem);
                const bool eq = (last == needle);
                if (repe ? !eq : eq) {
                    scanned += i + 1;
                    store_scas_state(cpu, dst + scanned * elem,
                                     count - scanned, wide);
                    finished = true;
                    break;
                }
            }
            if (finished) break;
            scanned += this_n;
            if (scanned == count) {
                store_scas_state(cpu, dst + scanned * elem, 0, wide);
                finished = true;
                break;
            }
        }
        if (!finished && scanned > 0) {
            dst += scanned * elem;
            count -= scanned;
        }
    }

    if (!finished) {
        const i64 step = df ? -static_cast<i64>(elem) : static_cast<i64>(elem);
        while (count > 0) {
            u64 v = 0;
            if (!read_str_elem(mem, dst, elem, v)) {
                store_scas_state(cpu, dst, count, wide);
                raise_string_av(cpu, dst, /*is_write=*/false);
                apply_cmps_flags(cpu, needle, last, elem);
                return;
            }
            last = v;
            advance_str_addr(dst, step, wide);
            --count;
            const bool eq = (v == needle);
            if (repe ? !eq : eq) break;
        }
        store_scas_state(cpu, dst, count, wide);
    }

    apply_cmps_flags(cpu, needle, last, elem);
}

// ---------------------------------------------------------------------------
// PDEP / PEXT (BMI2)
//
// Intel SDM Vol. 2: PDEP scatters the low bits of SRC into the bit positions
// selected by MASK; PEXT gathers the SRC bits at those positions down into the
// low bits of the result. ARM64 has no equivalent of either, so a call is the
// right lowering — an open-coded 64-iteration loop in generated code would be
// both larger and slower. Neither instruction affects EFLAGS.
//
// The 32-bit forms reach these with zero-extended operands and keep the low 32
// bits, which is why only the 64-bit width is implemented.
// ---------------------------------------------------------------------------

extern "C" u64 jit_pdep64(u64 src, u64 mask) {
    u64 result = 0;
    u64 bit = 1;
    while (mask != 0) {
        // Lowest set bit of the mask consumes the next source bit.
        const u64 low = mask & (~mask + 1);
        if (src & bit) result |= low;
        mask &= mask - 1;
        bit <<= 1;
    }
    return result;
}

extern "C" u64 jit_pext64(u64 src, u64 mask) {
    u64 result = 0;
    u64 bit = 1;
    while (mask != 0) {
        const u64 low = mask & (~mask + 1);
        if (src & low) result |= bit;
        mask &= mask - 1;
        bit <<= 1;
    }
    return result;
}

}  // namespace

extern "C" void jit_repe_cmpsb(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 1, true);
}
extern "C" void jit_repe_cmpsw(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 2, true);
}
extern "C" void jit_repe_cmpsd(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 4, true);
}
extern "C" void jit_repe_cmpsq(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 8, true);
}
extern "C" void jit_repne_cmpsb(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 1, false);
}
extern "C" void jit_repne_cmpsw(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 2, false);
}
extern "C" void jit_repne_cmpsd(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 4, false);
}
extern "C" void jit_repne_cmpsq(CpuState* cpu, GuestMemory* mem) {
    run_rep_cmps(cpu, mem, 8, false);
}

extern "C" void jit_repne_scasb(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 1, false);
}
extern "C" void jit_repe_scasb(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 1, true);
}
extern "C" void jit_repe_scasw(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 2, true);
}
extern "C" void jit_repe_scasd(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 4, true);
}
extern "C" void jit_repe_scasq(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 8, true);
}
extern "C" void jit_repne_scasw(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 2, false);
}
extern "C" void jit_repne_scasd(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 4, false);
}
extern "C" void jit_repne_scasq(CpuState* cpu, GuestMemory* mem) {
    run_rep_scas(cpu, mem, 8, false);
}

extern "C" u32 jit_eval_condition(CpuState* cpu, u32 cond_code) {
    cpu->materialize_lazy_flags();
    u64 f = cpu->flags.rflags;
    bool cf = (f >> 0) & 1;
    bool pf = (f >> 2) & 1;
    bool zf = (f >> 6) & 1;
    bool sf = (f >> 7) & 1;
    bool of = (f >> 11) & 1;
    switch (cond_code) {
    case 0x0: return of;
    case 0x1: return !of;
    case 0x2: return cf;
    case 0x3: return !cf;
    case 0x4: return zf;
    case 0x5: return !zf;
    case 0x6: return cf || zf;
    case 0x7: return !cf && !zf;
    case 0x8: return sf;
    case 0x9: return !sf;
    case 0xA: return pf;
    case 0xB: return !pf;
    case 0xC: return sf != of;
    case 0xD: return sf == of;
    case 0xE: return zf || (sf != of);
    case 0xF: return !zf && (sf == of);
    default: return 0;
    }
}

// Intel SDM Vol. 2 LODS/REP: load [ESI] into AL/AX/EAX, adjust ESI by ±size,
// repeat ECX times. PE32 AddressSize=32 uses ESI/ECX (not RSI/RCX). No flags.
namespace {

void run_rep_lods(CpuState* cpu, GuestMemory* mem, u8 size) {
    constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
    constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);
    constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);

    const bool df = (cpu->flags.rflags >> 10) & 1;
    const i64 step = df ? -static_cast<i64>(size) : static_cast<i64>(size);
    // PE32 JIT string path is AddressSize=32; keep wide for long-mode reuse.
    const bool wide = cpu->is_64bit();

    u64 count = wide ? cpu->gpr.get64(RCX) : cpu->gpr.get32(RCX);
    if (count == 0) return;

    u64 src = wide ? cpu->gpr.get64(RSI) : cpu->gpr.get32(RSI);

    auto commit = [&]() {
        if (wide) {
            cpu->gpr.set64(RSI, src);
            cpu->gpr.set64(RCX, count);
        } else {
            cpu->gpr.set32(RSI, static_cast<u32>(src));
            cpu->gpr.set32(RCX, static_cast<u32>(count));
        }
    };

    while (count > 0) {
        bool ok = false;
        switch (size) {
        case 1: {
            auto v = mem->read_u8(static_cast<GuestAddr>(src));
            if ((ok = v.is_ok())) cpu->gpr.set8l(RAX, v.value());
            break;
        }
        case 2: {
            auto v = mem->read_u16(static_cast<GuestAddr>(src));
            if ((ok = v.is_ok())) cpu->gpr.set16(RAX, v.value());
            break;
        }
        default: {
            auto v = mem->read_u32(static_cast<GuestAddr>(src));
            if ((ok = v.is_ok())) cpu->gpr.set32(RAX, v.value());
            break;
        }
        }
        if (!ok) {
            // Was a bare `return`, which left RSI and RCX at their ENTRY
            // values: the guest saw a REP LODS that had consumed nothing and
            // faulted nothing. Every sibling helper already committed here.
            commit();
            raise_string_av(cpu, src, /*is_write=*/false);
            return;
        }
        src = wide ? static_cast<u64>(static_cast<i64>(src) + step)
                   : static_cast<u32>(static_cast<u32>(src) +
                                      static_cast<u32>(step));
        --count;
    }

    commit();
}

} // namespace

extern "C" void jit_rep_lodsb(CpuState* cpu, GuestMemory* mem) {
    run_rep_lods(cpu, mem, 1);
}
extern "C" void jit_rep_lodsw(CpuState* cpu, GuestMemory* mem) {
    run_rep_lods(cpu, mem, 2);
}
extern "C" void jit_rep_lodsd(CpuState* cpu, GuestMemory* mem) {
    run_rep_lods(cpu, mem, 4);
}

} // namespace vitreo::cpu::jit
