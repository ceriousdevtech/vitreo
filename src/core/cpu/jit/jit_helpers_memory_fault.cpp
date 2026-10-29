// SPDX-License-Identifier: LGPL-2.1-only

/// @file jit_helpers_memory_fault.cpp
/// @brief How a failed checked guest access becomes STATUS_ACCESS_VIOLATION.
///
/// jit_raise_memory_av() fills cpu->pending_raise with the exception record
/// Windows reports (direction and first inaccessible byte, or the #GP form
/// for a non-canonical address), and jit_flush_pending_av_dump() prints the
/// deferred [jit-av] register dump once CpuState is authoritative again.
///
/// Split out of jit_helpers.cpp. Closed in both directions, checked with
/// tools/architecture/plan_extraction.py before moving; the cut is open only
/// by the two promotions declared in jit_helpers_internal.hpp.

#include "jit_helpers.hpp"
#include "jit_helpers_internal.hpp"
#include "core/memory/mem_watch.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <unistd.h>

namespace vitreo::cpu::jit {

// Raise STATUS_ACCESS_VIOLATION on the active CPU when guest code tries to
// read or write an unmapped/non-readable/non-writable address. This matches
// real Windows behavior — the dispatcher (after the JIT block finishes the
// current iteration) walks the SEH chain via cpu->pending_raise. Without
// this, the JIT silently returns 0 from unmapped reads, allowing guest code
// to dereference NULL/garbage pointers without ever signaling failure.
//
// Notes on contract:
//   - args[0] = type: 0 = read attempted, 1 = write attempted, 8 = DEP/exec.
//   - args[1] = the bad guest virtual address.
//   - flags  = 0 (continuable; SEH may handle and resume — though we don't
//              currently support resume after AV from a JIT helper).
//   - from_helper = true so the dispatcher does NOT simulate a RET-from-
//                   thunk on cpu.sp; cpu.rip is already at the faulting
//                   block start.
// The [jit-av] dump is DEFERRED, not emitted from inside the helper.
//
// emit_memory_helper_blr() no longer spills RAX/RCX/RDX/RBX/RSI/RDI to
// CpuState before a pure memory helper: those six live in X23-X28, which
// AAPCS64 makes callee-saved, so they are correct in the ARM registers but
// STALE in CpuState for as long as the helper is running. Printing the
// register dump here would therefore quietly report six wrong values on every
// access violation -- a diagnostic that lies is worse than no diagnostic.
//
// Instead the raise records what it wants dumped, and JitCompiler::execute()
// prints it after the block has returned through the raise-exit stub, whose
// emit_store_pins() has by then made CpuState authoritative for all sixteen
// GPRs. Correct for both the narrowed and the full spill.
namespace {
struct PendingAvDump {
    JitContext* ctx = nullptr;
    GuestAddr addr = 0;
    bool is_write = false;
    bool armed = false;
};
thread_local PendingAvDump g_pending_av_dump;
}  // namespace

static void jit_print_av_dump(JitContext* ctx, GuestAddr addr, bool is_write);

/// The address Windows reports for an access violation raised by a checked
/// access of `width` bytes at `addr` (EXCEPTION_RECORD.ExceptionInformation[1],
/// "the virtual address of the inaccessible data"): the first byte that lies
/// in an inaccessible 4 KiB page. For an access that starts in an accessible
/// page and runs into a denied one, that is the denied page's first byte, not
/// the access's -- measured for 2-, 4- and 8-byte loads and stores at every
/// straddling offset (tools/oracle_probe/split_access_fault_address_probe).
static GuestAddr jit_av_address(JitContext* ctx, GuestAddr addr, usize width,
                                bool is_write) {
    const GuestAddr next_page = (addr | GuestAddr{0xFFF}) + 1;
    if (width <= 1 || next_page == 0 || next_page - addr >= width) return addr;
    GuestAddr base = 0;
    usize size = 0;
    MemoryProtection prot{};
    const bool mapped = ctx->memory->query_region(addr, base, size, prot);
    const bool first_page_ok =
        mapped && has_flag(prot, is_write ? MemoryProtection::Write
                                          : MemoryProtection::Read);
    return first_page_ok ? next_page : addr;
}

static inline void jit_raise_access_violation(JitContext* ctx,
                                              GuestAddr addr,
                                              bool is_write) {
    if (!ctx || !ctx->cpu) return;
    auto& pr = ctx->cpu->pending_raise;
    if (pr.active) return;  // first AV wins; don't clobber pending raise
    pr.active = true;
    pr.from_helper = true;
    pr.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
    pr.flags = 0;
    pr.num_args = 2;
    pr.args[0] = is_write ? 1u : 0u;
    pr.args[1] = static_cast<u64>(addr);
    g_pending_av_dump = PendingAvDump{ctx, addr, is_write, true};
}

/// A checked guest access of @p width bytes at @p addr failed. x64 raises
/// #GP(0) when any byte's address is NON-CANONICAL (bits 63:47 not all equal,
/// Intel SDM Vol. 3A 6.15) and #PF otherwise; Windows reports the first as a
/// READ of 0xFFFFFFFFFFFFFFFF whatever the access direction, and the second
/// with the direction and the first inaccessible byte
/// (tools/oracle_probe/high_address_fault_probe.expected,
/// split_access_fault_address_probe.expected). A 32-bit guest's addresses are
/// all canonical.
static bool x64_canonical(GuestAddr a) noexcept {
    return static_cast<GuestAddr>(static_cast<i64>(a << 16) >> 16) == a;
}
void jit_raise_memory_av(JitContext* ctx, GuestAddr addr, usize width,
                                bool is_write) {
    const GuestAddr last = addr + static_cast<GuestAddr>(width - 1);
    if (!x64_canonical(addr) || !x64_canonical(last)) {
        jit_raise_access_violation(ctx, ~GuestAddr{0}, /*is_write=*/false);
        return;
    }
    jit_raise_access_violation(ctx, jit_av_address(ctx, addr, width, is_write),
                               is_write);
}

void jit_flush_pending_av_dump() {
    PendingAvDump d = g_pending_av_dump;
    g_pending_av_dump.armed = false;
    if (!d.armed || d.ctx == nullptr || d.ctx->cpu == nullptr) return;
    jit_print_av_dump(d.ctx, d.addr, d.is_write);
}

static void jit_print_av_dump(JitContext* ctx, GuestAddr addr,
                              bool is_write) {
    auto* c = ctx->cpu;
    std::fprintf(stderr,
        "[jit-av pid=%d htid=%llu] %s at 0x%016llx (rip=0x%016llx) raised STATUS_ACCESS_VIOLATION\n"
        "[jit-av]   RAX=%016llx RCX=%016llx RDX=%016llx RBX=%016llx\n"
        "[jit-av]   RSP=%016llx RBP=%016llx RSI=%016llx RDI=%016llx\n"
        "[jit-av]   R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n"
        "[jit-av]   R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
        static_cast<int>(::getpid()),
        static_cast<unsigned long long>(mem_watch_host_tid()),
        is_write ? "WRITE" : "READ",
        static_cast<unsigned long long>(addr),
        static_cast<unsigned long long>(c->rip),
        (unsigned long long)c->gpr.get64(0), (unsigned long long)c->gpr.get64(1),
        (unsigned long long)c->gpr.get64(2), (unsigned long long)c->gpr.get64(3),
        (unsigned long long)c->gpr.get64(4), (unsigned long long)c->gpr.get64(5),
        (unsigned long long)c->gpr.get64(6), (unsigned long long)c->gpr.get64(7),
        (unsigned long long)c->gpr.get64(8), (unsigned long long)c->gpr.get64(9),
        (unsigned long long)c->gpr.get64(10),(unsigned long long)c->gpr.get64(11),
        (unsigned long long)c->gpr.get64(12),(unsigned long long)c->gpr.get64(13),
        (unsigned long long)c->gpr.get64(14),(unsigned long long)c->gpr.get64(15));
    // Dump 16 x 8-byte slots from RSP — return addresses are somewhere in here.
    {
        GuestAddr rsp = static_cast<GuestAddr>(c->gpr.get64(4));
        auto sd = ctx->memory->probe_access(rsp, 128);
        if (sd.host_ptr && sd.readable) {
            const u64* q = reinterpret_cast<const u64*>(sd.host_ptr);
            for (int row = 0; row < 2; ++row) {
                std::fprintf(stderr, "[jit-av]   stack @ rsp+0x%02x:", row*64);
                for (int i = 0; i < 8; ++i)
                    std::fprintf(stderr, " %016llx",
                                 (unsigned long long)q[row*8 + i]);
                std::fprintf(stderr, "\n");
            }
        }
    }
    // Dump 16 bytes at faulting RIP for offline disassembly.
    auto dump = ctx->memory->probe_access(static_cast<GuestAddr>(c->rip), 16);
    if (dump.host_ptr && dump.readable) {
        const u8* p = static_cast<const u8*>(dump.host_ptr);
        std::fprintf(stderr,
            "[jit-av]   bytes @ rip: %02x %02x %02x %02x %02x %02x %02x %02x"
            " %02x %02x %02x %02x %02x %02x %02x %02x\n",
            p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
            p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
    }
    // Dump 64 bytes starting 32 bytes before rip so we see the instruction
    // stream leading up to the fault.
    GuestAddr base = (c->rip > 32) ? static_cast<GuestAddr>(c->rip - 32) : 0;
    auto pre = ctx->memory->probe_access(base, 64);
    if (pre.host_ptr && pre.readable) {
        const u8* p = static_cast<const u8*>(pre.host_ptr);
        std::fprintf(stderr, "[jit-av]   stream @ 0x%08x:", static_cast<unsigned>(base));
        for (int i = 0; i < 64; ++i) std::fprintf(stderr, " %02x", p[i]);
        std::fprintf(stderr, "\n");
    }
    // Dump the hostcall ring so we can see which Win32 API returned the
    // wrong value immediately before the AV. The crashing dereference is
    // almost always the symptom of an upstream contract bug.
    vitreo::diagnostics::dump_hostcalls("jit-av");
}

} // namespace vitreo::cpu::jit
