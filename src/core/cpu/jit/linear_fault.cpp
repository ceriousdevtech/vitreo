// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The JIT's recoverable-fault signal handler and its installation
// (linear_fault.hpp). Split as pure code motion toward the 500-line
// guideline (.docs/architecture.md); the rest lives in its siblings:
//   linear_fault_patch.cpp     site backpatching and its trampoline
//   linear_fault_registry.cpp  reservation, JIT-view, code-range and
//                              shared-helper tables, and their writers
//   linear_fault_rip_map.cpp   precise-RIP and flag-recovery metadata
//   linear_fault_internal.hpp  what more than one part shares

#include <type_traits>
#include <new>
#include "linear_fault.hpp"
#include "linear_fault_internal.hpp"
#include "jit_compiler.hpp"
#include "jit_helpers.hpp"
#include "stencils.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <mach/mach_vm.h>
#include <mach/mach.h>
#include <mach/vm_param.h>

namespace vitreo::cpu::jit {

namespace {

constexpr u32 kStatusAccessViolation = 0xC0000005u;

thread_local LinearFaultDetail g_last_linear_fault{};

} // namespace

// Shared fault-exit trampoline (one for all blocks — every block has the same
// STR X30,[SP,#-16]! prologue). Undoes the prologue and returns
// JitExecResult::Continue (0); the dispatcher then acts on pending_raise.
//   LDR X30,[SP],#16 ; MOVZ W0,#0 ; RET
std::atomic<void*> g_trampoline{nullptr};
static_assert(decltype(g_trampoline)::is_always_lock_free,
              "signal-handler trampoline pointer must be lock-free");
std::atomic<bool> g_installed{false};

namespace {

// jit_cold_stub_legacy(). A namespace-scope constant, initialized before main,
// because the signal handler reads it (tso_crossing_variant_words()).
const bool g_cold_stub_legacy = [] {
    const char* v = std::getenv("VITREO_JIT_COLD_STUB_LEGACY");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}();

// Previously installed handlers, chained for faults that aren't ours.
struct sigaction g_prev_segv;
struct sigaction g_prev_bus;
std::mutex g_install_mutex;

void chain_to_prev(int sig, siginfo_t* info, void* uc) {
    const struct sigaction& prev = (sig == SIGBUS) ? g_prev_bus : g_prev_segv;
    if (prev.sa_flags & SA_SIGINFO) {
        if (prev.sa_sigaction) { prev.sa_sigaction(sig, info, uc); return; }
    } else if (prev.sa_handler == SIG_IGN) {
        return;
    } else if (prev.sa_handler && prev.sa_handler != SIG_DFL) {
        prev.sa_handler(sig);
        return;
    }
    // No usable previous handler — restore default and re-raise so we crash.
    signal(sig, SIG_DFL);
    raise(sig);
}

// Decode whether the faulting ARM64 instruction is a store (write) or a load
// (read). This is ExceptionInformation[0] of the guest access violation, so a
// guest SEH filter that distinguishes a read fault from a write fault reads it
// directly.
//
// "Bit 22 is the L bit" is true for the ordered (LDAR/STLR) family and for the
// plain zero-extending LDR/STR, and WRONG for three encodings Vitreo emits:
//
//   * LDAPR  (RCpc acquire load, FEAT_LRCPC) has opc = 10, so the bit-22 rule
//     calls every ordered guest load a store.
//   * LDRSW / LDRSB-to-X (sign-extending loads) also carry opc = 10.
//   * STR/LDR Q (128-bit SIMD) puts the direction in opc = 10 vs 11.
//
// So decode the family instead of one bit. Anything not recognised is reported
// as a read: an unrecognised encoding is a bug in this table, and mislabelling
// a genuine store as a read is the less harmful of the two errors (a guest
// filter sees the fault either way, at the correct address).
bool fault_is_write(uintptr_t pc) {
    u32 insn = 0;
    __builtin_memcpy(&insn, reinterpret_cast<const void*>(pc), sizeof(insn));

    // LDAPR/LDAPRB/LDAPRH — always a load. Checked first: it lives inside the
    // load/store-register family's encoding space below.
    if ((insn & 0x3FFFFC00u) == 0x38BFC000u) return false;

    // FEAT_LSE atomic memory operations -- LDADD/LDCLR/LDEOR/LDSET/SWP in all
    // their A/L/AL forms: size 111000 A R 1 Rs o3 opc 00 Rn Rt. LDAPR shares
    // this space (o3:opc = 1100) and was answered above. A read-modify-write
    // is permission-checked as a store, and x86 reports a faulting LOCK RMW as
    // a write (the JIT helpers always did), so these are writes.
    if ((insn & 0x3F200C00u) == 0x38200000u) return true;

    // CAS/CASA/CASL/CASAL: size 001000 1 L 1 Rs o0 11111 Rn Rt. Checked before
    // the exclusive/ordered family below, whose bit-22 rule would call CASAL
    // (L = 1) a load. CAS is a store even when its compare fails.
    if ((insn & 0x3FA07C00u) == 0x08A07C00u) return true;

    // LDAPUR / STLUR (FEAT_LRCPC2 unscaled ordered): opc 00 = store.
    if ((insn & 0x3F200C00u) == 0x19000000u) {
        return ((insn >> 22) & 0x3u) == 0u;
    }

    // Load/store exclusive + LDAR/STLR family: bit 22 is the L bit.
    if ((insn & 0x3F000000u) == 0x08000000u) {
        return ((insn >> 22) & 1u) == 0u;
    }

    // Load/store pair, scalar and SIMD, every indexing form (STP/LDP,
    // STNP/LDNP): opc 101 V 0 type L. Bit 22 is the L bit. A 32-byte vector
    // store is one STP Q.
    if ((insn & 0x3A000000u) == 0x28000000u) {
        return ((insn >> 22) & 1u) == 0u;
    }

    // Load/store register (unsigned offset, register offset, unscaled).
    // Bit 24 selects unsigned-offset versus unscaled/register-offset forms;
    // it is not part of the load/store direction family. Masking it in (the
    // old 0x3B mask) excluded every LDR/STR unsigned-offset instruction.
    if ((insn & 0x3A000000u) == 0x38000000u) {
        const u32 opc = (insn >> 22) & 0x3u;
        const bool simd = ((insn >> 26) & 1u) != 0u;   // V bit
        // Scalar: 00 = store, 01 = zero-extending load, 10/11 = sign-extending
        // load (10 with size==11 is PRFM, which never faults through here).
        // SIMD: 00 = store, 01 = load, 10 = 128-bit store, 11 = 128-bit load.
        return simd ? (opc == 0u || opc == 2u) : (opc == 0u);
    }

    return false;
}

std::atomic<LinearFaultTestHook> g_linear_fault_test_hook{nullptr};

// The instruction word at @p pc as it is NOW. Atomic, so a read made to see
// whether the word changed is a fresh one the compiler can neither fold into
// an earlier read nor split: the patchers rewrite these words by CAS.
[[nodiscard]] inline u32 code_word_now(uintptr_t pc) noexcept {
    return __atomic_load_n(reinterpret_cast<const u32*>(pc), __ATOMIC_ACQUIRE);
}

// B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ -- every shape a site patch leaves.
[[nodiscard]] constexpr bool direct_branch(u32 w) noexcept {
    return (w & 0x7C000000u) == 0x14000000u ||  // B, BL
           (w & 0xFF000010u) == 0x54000000u ||  // B.cond
           (w & 0x7E000000u) == 0x34000000u ||  // CBZ/CBNZ
           (w & 0x7E000000u) == 0x36000000u;    // TBZ/TBNZ
}

// What a fault was, from the ESR the kernel saved in the signal context. The
// signal cannot say: XNU's sendsig() sets si_code = BUS_ADRALN for EVERY
// SIGBUS (bsd/dev/arm/unix_signal.c), so a permission fault on the
// reservation's PROT_NONE reads as an alignment fault. Arm ARM, ESR_EL1: EC
// [31:26] 0b100100 / 0b100101 is a Data Abort (from a lower / the same
// Exception level), and its ISS DFSC [5:0] is 0b0000LL address size,
// 0b0001LL translation, 0b0010LL access flag, 0b0011LL permission (LL the
// level; FEAT_LPA2/D128 add 0b101001/0b101100 and 0b101011/0b101010 for the
// negative levels), 0b010000 synchronous External abort, 0b010001 Tag Check,
// 0b100001 alignment, 0b110000 TLB conflict.
enum class DataAbortClass : u8 {
    NotDataAbort, Alignment, Translation, Permission, AccessFlag, AddressSize,
    External, TagCheck, TlbConflict, Other,
};

[[nodiscard]] constexpr DataAbortClass data_abort_class(u64 esr) noexcept {
    const u32 ec = static_cast<u32>(esr >> 26) & 0x3Fu;
    if (ec != 0x24u && ec != 0x25u) return DataAbortClass::NotDataAbort;
    const u32 dfsc = static_cast<u32>(esr) & 0x3Fu;
    switch (dfsc) {
        case 0x21u: return DataAbortClass::Alignment;
        case 0x10u: return DataAbortClass::External;
        case 0x11u: return DataAbortClass::TagCheck;
        case 0x30u: return DataAbortClass::TlbConflict;
        case 0x29u: case 0x2Cu: return DataAbortClass::AddressSize;
        case 0x2Au: case 0x2Bu: return DataAbortClass::Translation;
        default: break;
    }
    switch (dfsc >> 2) {
        case 0u: return DataAbortClass::AddressSize;
        case 1u: return DataAbortClass::Translation;
        case 2u: return DataAbortClass::AccessFlag;
        case 3u: return DataAbortClass::Permission;
        default: return DataAbortClass::Other;
    }
}
static_assert(data_abort_class(0x92000061u) == DataAbortClass::Alignment);
static_assert(data_abort_class(0x9200004Fu) == DataAbortClass::Permission);
static_assert(data_abort_class(0x92000047u) == DataAbortClass::Translation);
static_assert(data_abort_class(0x96000061u) == DataAbortClass::Alignment);
static_assert(data_abort_class(0x82000061u) == DataAbortClass::NotDataAbort);

void linear_fault_handler(int sig, siginfo_t* info, void* uc_raw) {
    const uintptr_t fault = reinterpret_cast<uintptr_t>(info->si_addr);
    if (const LinearFaultTestHook hook =
            g_linear_fault_test_hook.load(std::memory_order_acquire)) [[unlikely]] {
        hook(LinearFaultTestPoint::Entry,
             static_cast<ucontext_t*>(uc_raw)->uc_mcontext->__ss.__pc, uc_raw);
    }

    // Ordered AArch64 scalar accesses signal on the x86-legal case that
    // crosses a 16-byte granule. The handler itself only recognizes the
    // self-describing two-word site and redirects. MAP_JIT writes, and the
    // cache maintenance that publishes them, happen after sigreturn in the
    // preservation trampoline.
    auto* uc = static_cast<ucontext_t*>(uc_raw);
    const uintptr_t fault_pc = uc->uc_mcontext->__ss.__pc;
    // VITREO_TSO_FAULT_DIAG=1: one signal-safe line per fault this handler
    // sees, BEFORE any classification. The bogus-AV bug class this hunts is
    // invisible afterwards — by then the fault has become a plausible guest
    // exception — so the raw signal, pc, both candidate site words and the
    // arena answer are recorded at the door. write(2) only; no printf in a
    // signal handler.
    {
        static const bool diag = getenv("VITREO_TSO_FAULT_DIAG") != nullptr;
        if (diag) {
            char buf[160];
            u32 w0 = 0, wp = 0, wn = 0;
            const bool in_jit = jit_code_range_contains(fault_pc);
            if (in_jit) {
                __builtin_memcpy(&w0, reinterpret_cast<void*>(fault_pc), 4);
                __builtin_memcpy(&wp, reinterpret_cast<void*>(fault_pc - 4), 4);
                __builtin_memcpy(&wn, reinterpret_cast<void*>(fault_pc + 4), 4);
            }
            int n = snprintf(buf, sizeof buf,
                             "[tso-diag] sig=%d code=%d addr=%lx pc=%lx "
                             "insn=%08x prev=%08x next=%08x jit=%d\n",
                             sig, info->si_code,
                             reinterpret_cast<uintptr_t>(info->si_addr),
                             fault_pc, w0, wp, wn, in_jit ? 1 : 0);
            (void)!write(2, buf, static_cast<size_t>(n));
        }
    }
    // The stale-fault rule. A Data Abort returns to the instruction that took
    // it (Arm ARM, AArch64.DataAbort: preferred_exception_return =
    // ThisInstrAddr()), and only an instruction that loads or stores a
    // register can take one -- the Alignment fault of SCTLR_ELx.A and .nAA
    // (LDAPR/LDAR/STLR and kin, bytes not within one naturally aligned
    // 16-byte quantity) included. So when the word at a data fault's PC is no
    // longer the access that faulted, that access is gone: another thread's
    // retry rewrote the site (`B variant`, `B slot`, [DMB, STR]) after this
    // thread executed it, and re-executing from the PC takes the patched
    // path. Both tells are taken against ONE read of the word, made here:
    //  * the word is a BRANCH, which cannot data-abort: the patch landed
    //    between this thread's fault and this read (title run r9, tier 1,
    //    2026-10-20);
    //  * the word CHANGES while the recognizers below classify the fault.
    //    Each reads it for itself, so an ordered access seen as one by the
    //    patched-site tests, then as the B it became by the site recognizer,
    //    matched neither, and the fault was raised as the guest's. A 64-bit
    //    title run r4 and a title run r9, 2026-10-20, logged
    //    'ADRALN raised as a guest AV ... insn=14xxxxxx ... meta=1 site=1'
    //    at valid addresses. Tested just before a fault becomes the guest's,
    //    below.
    // An instruction fetch fault (si_addr == PC) is neither.
    const bool jit_data_fault =
        fault != fault_pc && jit_code_range_contains(fault_pc);
    const u32 fault_word = jit_data_fault ? code_word_now(fault_pc) : 0u;
    if (jit_data_fault && direct_branch(fault_word)) {
        // This PE's data side sees the branch; its instruction fetch need not
        // until the patcher's cache maintenance runs, and a patcher preempted
        // between its CAS and its sys_icache_invalidate leaves this thread
        // re-fetching the old access. So clean and invalidate the line to the
        // PoU from here (a read-side operation: no MAP_JIT write); the signal
        // return is the context synchronization event, so the re-fetch is
        // the branch. The same access re-faulting at a branch after that
        // re-fetch was not patched under us: no guest access exists to raise
        // an exception for, so it is a host bug -- crash, never a guest AV.
        static thread_local uintptr_t t_patched_pc = 0;
        static thread_local uintptr_t t_patched_fault = 0;
        static thread_local u32 t_patched_count = 0;
        if (t_patched_pc != fault_pc || t_patched_fault != fault) {
            t_patched_pc = fault_pc;
            t_patched_fault = fault;
            t_patched_count = 0;
        }
        if (++t_patched_count <= 64) {
            sys_icache_invalidate(reinterpret_cast<void*>(fault_pc), 4);
            return;
        }
        t_patched_pc = 0;
        t_patched_fault = 0;
        t_patched_count = 0;
        char buf[192];
        const int n = snprintf(
            buf, sizeof buf,
            "[linear-fault] data fault recurs at a branch after its I-cache "
            "line was invalidated: pc=%lx insn=%08x addr=%lx sig=%d code=%d\n",
            static_cast<unsigned long>(fault_pc), fault_word,
            static_cast<unsigned long>(fault), sig, info->si_code);
        if (n > 0) (void)!write(2, buf, static_cast<size_t>(n));
        chain_to_prev(sig, info, uc_raw);
        return;
    }
    // A JIT-view lean site the enforcing view refused (a guest violation, a
    // mixed page, translated code, a write watch, a window edge, a granule
    // crossing): rewrite its guard to B its checked variant, outside signal
    // context, and run from the guard -- the variant decides the access from
    // the guest registers, which the faulting access left untouched. A stale
    // fetch of an already-patched site, or a thread caught past the guard,
    // simply restarts at it, now a B. First: the ADRALN retry below must
    // never see a lean site.
    if (jit_code_range_contains(fault_pc) && jit_view_envelope_contains(fault)) {
        uintptr_t guard = 0, variant = 0;
        if (lean_site(fault_pc, guard, variant)) {
            u32 w = 0;
            __builtin_memcpy(&w, reinterpret_cast<const void*>(guard), 4);
            const i64 disp = static_cast<i64>(variant) - static_cast<i64>(guard);
            if (w == arm64::b_imm(static_cast<i32>(disp))) {
                uc->uc_mcontext->__ss.__pc = guard;
                return;
            }
            uintptr_t sp = uc->uc_mcontext->__ss.__sp - 16;
            *reinterpret_cast<uintptr_t*>(sp) = guard;
            *reinterpret_cast<uintptr_t*>(sp + 8) = fault;  // for the patch log
            uc->uc_mcontext->__ss.__sp = sp;
            uc->uc_mcontext->__ss.__pc = reinterpret_cast<uintptr_t>(
                &vitreo_jit_tso_patch_trampoline);
            return;
        }
        // Only lean sites address the JIT view (their variants, the cold
        // stubs and every helper use the host view), so anything else that
        // faults here is a JIT bug: crash, never a fabricated guest AV at a
        // meaningless address.
        chain_to_prev(sig, info, uc_raw);
        return;
    }
    // A FULLY patched site can still fault once more: the patcher (this
    // thread or another) rewrote [marker, STLR] to [DMB, STR], but this CPU's
    // instruction fetch may not have converged yet, so it executes the STALE
    // ordered image and takes its alignment SIGBUS — while this handler reads
    // the NEW bytes, finds no recognizable site, and (measured, live) raised a
    // bogus guest ACCESS_VIOLATION that crashed the GPU and network-service
    // processes at stable hot sites. The tell is unmistakable in the fault
    // record: BUS_ADRALN at a PLAIN load/store, which cannot alignment-fault
    // on Normal memory at all. Returning without touching PC retries the
    // instruction; the signal return is context-synchronizing, so the
    // re-fetch converges (re-faulting harmlessly through here until it does).
    if (sig == SIGBUS && info->si_code == BUS_ADRALN &&
        jit_code_range_contains(fault_pc)) {
        // The same stale fetch at a marker-free site: its one word is now a B
        // to the site's crossing variant, which cannot fault at all. Retry.
        if (patched_crossing_variant_site(fault_pc)) return;
        u32 w = 0, prev = 0, next = 0;
        __builtin_memcpy(&w, reinterpret_cast<const void*>(fault_pc), 4);
        // Three exclusions carved by measured infinite loops, each a case
        // where a plain-looking access next to a DMB alignment-faults
        // LEGITIMATELY and forever:
        //  * LDAPR shares the 0x38 encoding space; a mid-patch [LDAPR, DMB]
        //    site belongs to the recognizer below, not a blind retry.
        //  * SIMD (V bit): LDR/STR Q next to a vector fence is never a patch
        //    site, and an UNMAPPED unaligned access also reports BUS_ADRALN
        //    on this host — a 128-bit load from unmapped guest memory span
        //    at 100% CPU until this exclusion.
        //  * A retry budget, because the two exclusions above were both found
        //    the hard way: any persistent ADRALN this heuristic keeps
        //    retrying must eventually fall through to real classification.
        //    Stale-fetch convergence takes a handful of iterations at most.
        const bool scalar_plain =
            (w & 0x3A000000u) == 0x38000000u &&
            ((w >> 26) & 1u) == 0u &&                 // V=0: not SIMD
            (w & 0x3FFFFC00u) != 0x38BFC000u;         // not LDAPR
        if (scalar_plain) {
            constexpr u32 kDmb = 0xD5033BBFu;
            if (fault_pc >= 4) {
                __builtin_memcpy(&prev,
                                 reinterpret_cast<const void*>(fault_pc - 4), 4);
            }
            __builtin_memcpy(&next,
                             reinterpret_cast<const void*>(fault_pc + 4), 4);
            // Store sites carry their DMB before the access, load sites after.
            if (prev == kDmb || next == kDmb) {
                static thread_local uintptr_t t_retry_pc = 0;
                static thread_local u32 t_retry_count = 0;
                if (t_retry_pc != fault_pc) {
                    t_retry_pc = fault_pc;
                    t_retry_count = 0;
                }
                if (++t_retry_count <= 64) {
                    return;
                }
                // Budget exhausted: not a stale fetch. Reset so an eventual
                // genuine stale fetch at this pc later still gets retries.
                t_retry_pc = 0;
                t_retry_count = 0;
            }
        }
    }
    // A range-by-fault site's map load past the VA limit (linear_fault.hpp):
    // the EA is outside the reservation. Redirect the site to its range
    // variant, outside signal context, and retry from the site. A stale fetch
    // of an already-patched site faults here too: re-execute its branch.
    if (fault_pc >= 4 && fault >= MACH_VM_MAX_ADDRESS &&
        jit_code_range_contains(fault_pc)) {
        u32 w = 0;
        __builtin_memcpy(&w, reinterpret_cast<const void*>(fault_pc), 4);
        if (w == arm64::ldrb_reg(3, 15, 3)) {
            const uintptr_t site = fault_pc - 4;
            if (patched_range_variant_site(site)) {
                uc->uc_mcontext->__ss.__pc = site;
                return;
            }
            if (uintptr_t variant = 0; range_variant_site(site, variant)) {
                uintptr_t sp = uc->uc_mcontext->__ss.__sp - 16;
                *reinterpret_cast<uintptr_t*>(sp) = site;
                *reinterpret_cast<uintptr_t*>(sp + 8) = 0;
                uc->uc_mcontext->__ss.__sp = sp;
                uc->uc_mcontext->__ss.__pc = reinterpret_cast<uintptr_t>(
                    &vitreo_jit_tso_patch_trampoline);
                return;
            }
        }
    }

    if (const LinearFaultTestHook hook =
            g_linear_fault_test_hook.load(std::memory_order_acquire)) [[unlikely]] {
        hook(LinearFaultTestPoint::BeforePatchSite, fault_pc, uc_raw);
    }
    uintptr_t tso_site = 0;
    if (jit_code_range_contains(fault_pc) &&
        ordered_tso_site(fault_pc, tso_site)) {
        uintptr_t sp = uc->uc_mcontext->__ss.__sp - 16;
        *reinterpret_cast<uintptr_t*>(sp) = tso_site;
        *reinterpret_cast<uintptr_t*>(sp + 8) = 0;
        uc->uc_mcontext->__ss.__sp = sp;
        uc->uc_mcontext->__ss.__pc = reinterpret_cast<uintptr_t>(
            &vitreo_jit_tso_patch_trampoline);
        return;
    }
    // Every recognizer declined. The stale-fault rule's second tell: the
    // faulting word is no longer the one read on entry, so the access that
    // faulted is gone -- re-execute it (a rewrite to a branch whose fetch
    // lags re-faults into the first tell). Unchanged, it was this same word
    // in every read above, because a site word only ever moves from its
    // access to its patched form, never back: the fault is decided on one
    // consistent view, and no patch can follow for a word no recognizer
    // accepted. Before anything below acts on the guest address.
    if (jit_data_fault && code_word_now(fault_pc) != fault_word) return;

    // Is the fault inside a registered reservation?
    u8* win_base = nullptr;
    for (int i = 0; i < kMaxWindows; ++i) {
        u8* b = g_windows[i].base.load(std::memory_order_acquire);
        if (!b) continue;
        usize s = g_windows[i].size.load(std::memory_order_acquire);
        if (fault >= reinterpret_cast<uintptr_t>(b) &&
            fault < reinterpret_cast<uintptr_t>(b) + s) {
            win_base = b;
            break;
        }
    }
    const bool inside_reservation = win_base != nullptr;
    if (!inside_reservation) {
        // A guest address that lands OUTSIDE the reservation is still an
        // ordinary guest access violation: Windows reports
        // STATUS_ACCESS_VIOLATION to the thread's SEH for any unmapped
        // address, and the guest's own address space is not bounded by the
        // size of our host window. Killing the host instead loses the guest's
        // handler entirely -- measured on a launcher, where an unchecked JIT load
        // through a corrupt pointer took down a renderer helper rather than
        // raising the AV an embedded browser is built to survive.
        //
        // The emitted access is ADD Xaddr, LINEAR_BASE, Xguest, so the guest
        // address is `fault - base` in wrapping 64-bit arithmetic even when
        // the sum left the window. That reconstruction needs to know WHICH
        // base the block used, so it is taken only when exactly one
        // reservation is registered; with none or several, the address cannot
        // be attributed and the fault stays a host crash.
        if (!jit_code_range_contains(fault_pc)) {
            chain_to_prev(sig, info, uc_raw);
            return;
        }
        int registered = 0;
        for (int i = 0; i < kMaxWindows; ++i) {
            u8* b = g_windows[i].base.load(std::memory_order_acquire);
            if (b == nullptr) continue;
            ++registered;
            win_base = b;
        }
        if (registered != 1) {
            chain_to_prev(sig, info, uc_raw);
            return;
        }
        // Never silent: an out-of-window access means a guest pointer we
        // cannot prove came from the guest, so the recovery is reported even
        // though it is contract-correct. Bounded and write(2)-only, because
        // this is a signal handler.
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            char buf[192];
            const int n = snprintf(
                buf, sizeof buf,
                "[linear-fault] guest address outside the reservation: "
                "addr=%lx pc=%lx guest=%llx -> raising guest "
                "STATUS_ACCESS_VIOLATION\n",
                fault, fault_pc,
                static_cast<unsigned long long>(
                    fault - reinterpret_cast<uintptr_t>(win_base)));
            (void)!write(2, buf, static_cast<size_t>(n));
        }
    }

    const u64 guest_addr =
        static_cast<u64>(fault - reinterpret_cast<uintptr_t>(win_base));
    // MEM_WRITE_WATCH: an armed PROT_READ page takes a store from JIT or
    // from a C++ helper (CRT memset). Mark dirty, restore write, and retry.
    // Do this before the trampoline check — CRT can fault before JIT init.
    // Only inside the reservation: outside it there is no guest page to arm,
    // and `guest_addr` is a reconstruction, not a mapped address.
    if (inside_reservation &&
        guard_page::handle_fault(static_cast<GuestAddr>(guest_addr))) {
        // User-mode access: raise STATUS_GUARD_PAGE_VIOLATION then let
        // CONTINUE_EXECUTION retry (page already restored). Fall through to
        // the trampoline path below when we have a JIT CpuState*; otherwise
        // retry in-place (host C++ / interpreter soft path via signal return).
        void* const trampoline = g_trampoline.load(std::memory_order_acquire);
        if (trampoline != nullptr && jit_code_range_contains(fault_pc)) {
            auto* cpu = reinterpret_cast<CpuState*>(
                uc->uc_mcontext->__ss.__x[19]);
            if (cpu != nullptr) {
                auto& pr = cpu->pending_raise;
                if (!pr.active) {
                    pr.active = true;
                    pr.from_helper = true;
                    pr.code = guard_page::kStatusGuardPageViolation;
                    pr.flags = 0;
                    pr.num_args = 2;
                    // ExceptionInformation[0]: 0 read, 1 write, 8 execute.
                    pr.args[0] = fault_is_write(fault_pc) ? 1u : 0u;
                    pr.args[1] = guest_addr;
                }
                // Continue into the shared AV trampoline setup below by not
                // returning; fall through after harvesting pins.
            } else {
                return;  // retry host access after clear
            }
        } else {
            return;  // retry
        }
    }
    if (inside_reservation &&
        write_watch::handle_fault(static_cast<GuestAddr>(guest_addr))) {
        return;
    }
    void* const trampoline = g_trampoline.load(std::memory_order_acquire);
    if (trampoline == nullptr) {
        chain_to_prev(sig, info, uc_raw);
        return;
    }

    // Only recover faults from JIT-emitted code (PC in the arena) — there X19
    // is a valid CpuState*. A reservation fault from C++ code (loader writes,
    // helpers) has garbage in X19; recovering it would corrupt memory, so chain
    // it to the crash handler instead.
    if (!jit_code_range_contains(fault_pc)) {
        chain_to_prev(sig, info, uc_raw);
        return;
    }
    // X19 is the pinned CpuState* while a JIT block runs (see JitCompiler::
    // execute). If it's null we're not in a block — not our fault to recover.
    auto* cpu = reinterpret_cast<CpuState*>(uc->uc_mcontext->__ss.__x[19]);
    if (!cpu) { chain_to_prev(sig, info, uc_raw); return; }

    // Recover precise guest RIP from the faulting native PC. During rollout,
    // instructions still publish cpu->rip before memory accesses, so missing
    // metadata (notably shared cold stubs) retains the established fallback.
    JitPinMap pin_map{{23, 24, 25, 26, 12, 13, 27, 28,
                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    // A fault in a shared memory helper (its sub-page or region-slot direct
    // access) belongs to the block whose cold stub called it: X30. The helper
    // parks that return address in X3 across its access, because the
    // ordered-access patch trampoline returns to a retried access with X30 =
    // the retry PC (jit_memory_emit.cpp, emit_shared_subpage). An X30 inside
    // the helper page is such a retry PC, never a caller: the caller is X3.
    uintptr_t metadata_pc = fault_pc;
    const uintptr_t helper_lo = g_shared_helper_lo.load(std::memory_order_acquire);
    const uintptr_t helper_hi = g_shared_helper_hi.load(std::memory_order_acquire);
    if (fault_pc >= helper_lo && fault_pc < helper_hi) {
        metadata_pc = uc->uc_mcontext->__ss.__lr;
        if (metadata_pc >= helper_lo && metadata_pc < helper_hi) {
            metadata_pc = uc->uc_mcontext->__ss.__x[3];
        }
    }
    JitFaultMetadata metadata{};
    const bool have_metadata = jit_fault_metadata_for_pc(metadata_pc, metadata);
    if (have_metadata) {
        pin_map = metadata.pins;
        if (metadata.has_guest_rip) cpu->rip = metadata.guest_rip;
    }
    // Record before the pins are harvested: whether a site described this PC
    // decides whether the guest RIP and register file below are attributed or
    // reconstructed from a fixed layout guess.
    g_last_linear_fault = LinearFaultDetail{
        static_cast<u64>(fault_pc),
        static_cast<u64>(fault),
        static_cast<u64>(reinterpret_cast<uintptr_t>(win_base)),
        guest_addr,
        metadata.has_guest_rip ? static_cast<u64>(metadata.guest_rip) : 0,
        have_metadata,
        fault_is_write(fault_pc),
        true};

    // Blocks keep the low eight GPRs in X23-X28 / X12 / X13 and only flush
    // CpuState at helper BLR / epilogue, so a mid-block fault must harvest the
    // live pins. A 32-bit block's pin carries bits 31:0 only; a 64-bit block's
    // carries the whole register.
    {
        const bool wide = cpu->is_64bit();
        for (u8 i = 0; i < 16; ++i) {
            const u8 host = pin_map.host_reg[i];
            if (host == 0xFF) continue;
            const u64 pin = uc->uc_mcontext->__ss.__x[host];
            if (wide) {
                cpu->gpr.set64(i, pin);
            } else {
                cpu->gpr.set32(i, static_cast<u32>(pin));
            }
        }
    }

    // Rebuild an elided lazy flag record from the interrupted register file.
    //
    // A Tier 1 block may skip the three CpuState stores that publish a flag
    // definition when `JitFlagRecovery` can name where the record's operands
    // would be at every point the record could be observed. This is the only
    // place that promise is redeemed on the hardware-fault path, and it runs
    // before the thread is redirected to the trampoline, so the guest cannot
    // observe EFLAGS between the fault and the reconstruction.
    //
    // Async-signal-safe: bounded loads from the interrupted context and plain
    // stores into CpuState, no allocation and no locks.
    // A fault in block-owned cold code -- a crossing or range variant, a
    // checked slow path -- has no instruction interval of its own, but that
    // code published the faulting instruction's RIP before its access, so the
    // descriptor is the one the block keeps for that RIP.
    JitFlagRecovery recovery =
        metadata.has_guest_rip ? metadata.flags : JitFlagRecovery{};
    if (have_metadata && !metadata.has_guest_rip) {
        JitFaultMetadata by_rip{};
        if (jit_flag_recovery_for_guest_rip(metadata_pc, cpu->rip, by_rip) &&
            by_rip.has_guest_rip) {
            recovery = by_rip.flags;
        }
    }
    if (recovery.active()) {
        const JitFlagRecovery& fr = recovery;
        const auto host = [&](u8 reg) -> u64 {
            // Publication rejects an out-of-range register, but the bound is
            // re-checked here: this indexes the interrupted register file
            // inside a signal handler.
            if (JitFlagRecovery::is_cpu_state_gpr(reg)) {
                // Unpinned guest register: pins were harvested above, and an
                // unpinned one was written through to CpuState when defined.
                return cpu->gpr.get64(reg - JitFlagRecovery::kCpuStateGpr);
            }
            if (reg == JitFlagRecovery::kImmediate || reg >= 29) return 0;
            return uc->uc_mcontext->__ss.__x[reg];
        };
        (void)jit_rebuild_flag_record(fr, host(fr.src_reg), host(fr.dst_reg),
                                      *cpu);
    }
    // An instruction that moved RSP before its faulting access (see
    // JitFlagRecovery::rsp_undo): the harvested RSP is the moved one, and the
    // trampoline returns to the dispatcher from CpuState, so undo it there.
    if (recovery.rsp_undo != 0) {
        if (cpu->is_64bit()) {
            cpu->gpr.set64(4, cpu->gpr.get64(4) +
                                  static_cast<u64>(static_cast<i64>(recovery.rsp_undo)));
        } else {
            cpu->gpr.set32(4, cpu->gpr.get32(4) +
                                  static_cast<u32>(static_cast<i32>(recovery.rsp_undo)));
        }
    }

    const uintptr_t pc = fault_pc;

    // An alignment fault in JIT code that reaches here becomes a guest AV.
    // Every legitimate crossing is served above (a tier-1 patch site, a
    // tier-O deopt, the helper's retry); an AV from one is only right for
    // unmapped memory. A 64-bit title run r7 raised one at a valid stack slot
    // (Renderer RVA 0x87d9ac), so name what the attribution saw: whether a
    // site described the PC, whether it is tier-O code, and whether the
    // code's arena chunk is still live (generation 0: retired, its
    // metadata already withdrawn). One signal-safe line, the first 64 times.
    // The fault's class is the saved ESR's (data_abort_class), not the
    // signal's: an ordinary guest AV on the reservation's PROT_NONE also
    // arrives as SIGBUS/BUS_ADRALN, and printed this line for every one of
    // them (a browser renderer, 2026-10-22, a byte LDAPRB "alignment" AV).
    const u64 esr = uc->uc_mcontext->__es.__esr;
    const DataAbortClass abort_class = data_abort_class(esr);
    if (abort_class == DataAbortClass::Alignment ||
        (abort_class == DataAbortClass::NotDataAbort && sig == SIGBUS)) {
        static std::atomic<u32> s_adraln_av_lines{0};
        if (s_adraln_av_lines.fetch_add(1, std::memory_order_relaxed) < 64) {
            u32 insn = 0;
            __builtin_memcpy(&insn, reinterpret_cast<const void*>(fault_pc), 4);
            char buf[288];
            const int n = snprintf(
                buf, sizeof buf,
                "[linear-fault] %s raised as a guest AV: esr=%llx pc=%lx "
                "insn=%08x guest=%llx rip=%llx meta=%d site=%d "
                "chunk_gen=%llu helper=%d lr=%lx\n",
                abort_class == DataAbortClass::Alignment
                    ? "alignment fault"
                    : "SIGBUS that is not a data abort",
                static_cast<unsigned long long>(esr),
                static_cast<unsigned long>(fault_pc), insn,
                static_cast<unsigned long long>(guest_addr),
                static_cast<unsigned long long>(cpu->rip), have_metadata ? 1 : 0,
                metadata.has_guest_rip ? 1 : 0,
                static_cast<unsigned long long>(
                    JitCodeArena::arena_live_generation(reinterpret_cast<const void*>(fault_pc))),
                metadata_pc != fault_pc ? 1 : 0,
                static_cast<unsigned long>(uc->uc_mcontext->__ss.__lr));
            if (n > 0) (void)!write(2, buf, static_cast<size_t>(n));
        }
    }

    auto& pr = cpu->pending_raise;
    if (!pr.active) {  // first AV wins
        pr.active = true;
        pr.from_helper = true;   // rip/sp already reflect the block; no thunk RET
        pr.code = kStatusAccessViolation;
        pr.flags = 0;
        pr.num_args = 2;
        pr.args[0] = fault_is_write(pc) ? 1u : 0u;
        pr.args[1] = guest_addr;
    }

    // A native guest-CALL chain may have continuation frames between SP and
    // this entry's dispatcher LR frame (kNativeCallFrameSize each, at most
    // kNativeCallMaxFrames). The shared trampoline knows only how to pop that
    // LR frame, so restore SP to the base execute() published first. Any
    // other gap is not a state JIT code creates.
    // X20 is the pinned JitContext* for every executable JIT entry.
    auto* jit_ctx = reinterpret_cast<JitContext*>(
        uc->uc_mcontext->__ss.__x[20]);
    if (jit_ctx != nullptr && jit_ctx->native_sp_base != 0) {
        const uintptr_t sp = uc->uc_mcontext->__ss.__sp;
        const uintptr_t base = jit_ctx->native_sp_base;
        if (base != sp) {
            constexpr uintptr_t span =
                uintptr_t{kNativeCallMaxFrames} * kNativeCallFrameSize;
            if (base < sp || base - sp > span ||
                (base - sp) % kNativeCallFrameSize != 0) {
                chain_to_prev(sig, info, uc_raw);
                return;
            }
            uc->uc_mcontext->__ss.__sp = base;
        }
    }

    // A fault inside a native LOCK read-modify-write happens with this
    // thread's LOCK reader slot published (compile_locked). Clear it, or a
    // split-lock fallback on another thread waits on it forever. No JIT code
    // runs inside a C++ LockPrefixGuard, so on this path the slot can only
    // be the emitted code's, and clearing an already-clear slot is harmless.
    if (jit_ctx != nullptr && jit_ctx->lock_reader_active != nullptr) {
        jit_ctx->lock_reader_active->store(0, std::memory_order_release);
    }

    // Resume in the trampoline: it pops the block's 16-byte LR frame and
    // returns Continue to the dispatcher, which raises the guest exception.
    uc->uc_mcontext->__ss.__pc = reinterpret_cast<uintptr_t>(trampoline);
}

void* build_trampoline() {
    void* mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    static const u32 code[] = {
        0xF84107FEu,  // LDR X30, [SP], #16
        0x52800000u,  // MOVZ W0, #0   (JitExecResult::Continue)
        0xD65F03C0u,  // RET
    };
    pthread_jit_write_protect_np(0);
    __builtin_memcpy(mem, code, sizeof(code));
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache(reinterpret_cast<char*>(mem),
                            reinterpret_cast<char*>(mem) + sizeof(code));
    return mem;
}

} // namespace

bool jit_last_linear_fault(LinearFaultDetail& out) {
    if (!g_last_linear_fault.valid) return false;
    out = g_last_linear_fault;
    return true;
}

bool jit_cold_stub_legacy() noexcept { return g_cold_stub_legacy; }

void set_linear_fault_test_hook_for_test(LinearFaultTestHook hook) noexcept {
    g_linear_fault_test_hook.store(hook, std::memory_order_release);
}

bool jit_code_range_contains(uintptr_t pc) noexcept {
    const JitCodeRangeSnapshot* snapshot =
        g_jit_ranges.load(std::memory_order_acquire);
    for (usize i = 0; i < snapshot->count; ++i) {
        if (pc >= snapshot->ranges[i].lo && pc < snapshot->ranges[i].hi) {
            return true;
        }
    }
    return false;
}

bool install_linear_fault_handler() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lock(g_install_mutex);
    if (g_installed.load(std::memory_order_relaxed)) return true;

    void* const trampoline = build_trampoline();
    if (trampoline == nullptr) {
        std::fprintf(stderr, "[linear-fault] trampoline alloc failed; "
                             "linear faults will not be recoverable\n");
        return false;
    }

    struct sigaction sa{};
    sa.sa_sigaction = linear_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_prev_segv) != 0) {
        munmap(trampoline, 4096);
        return false;
    }
    if (sigaction(SIGBUS, &sa, &g_prev_bus) != 0) {
        (void)sigaction(SIGSEGV, &g_prev_segv, nullptr);
        munmap(trampoline, 4096);
        return false;
    }
    g_trampoline.store(trampoline, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return true;
}

} // namespace vitreo::cpu::jit
