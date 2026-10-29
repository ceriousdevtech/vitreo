// SPDX-License-Identifier: LGPL-2.1-only

#if defined(VITREO_ARCH_X86_64)

#include "runtime/execution/native64_loop.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/x64/native64.hpp"
#include "core/cpu/x64/segment_override_rewrite.hpp"
#include "core/cpu/x64/x64_exception_bridge.hpp"
#include "core/cpu/x64/x64_guest_thread.hpp"
#include "core/cpu/x64/x64_segments.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>
#include <atomic>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace vitreo::cli {

namespace {

using cpu::x64::GuestTebThread;
using cpu::x64::GuestHostHop;
using cpu::x64::Native64Exit;
using cpu::x64::Native64Transfer;

bool g_prepared = false;
std::mutex g_segment_rewrite_mutex;
std::set<std::pair<GuestAddr, GuestAddr>> g_segment_rewritten_extents;
GuestMemory* g_segment_rewrite_memory = nullptr;
u64 g_segment_rewrite_map_generation = 0;
u64 g_segment_rewrite_protect_sequence = 0;

void dump_native64_pointer_chain(GuestMemory& memory) noexcept {
    const char* configured = std::getenv("VITREO_NATIVE64_DUMP_CHAIN");
    if (configured == nullptr || *configured == '\0') return;

    char* end = nullptr;
    GuestAddr address = static_cast<GuestAddr>(
        std::strtoull(configured, &end, 0));
    if (end == configured || address == 0) return;

    for (unsigned depth = 0; depth < 4; ++depth) {
        std::fprintf(stderr,
                     "[native64-chain] depth=%u address=0x%llx values:",
                     depth, static_cast<unsigned long long>(address));
        GuestAddr next = 0;
        for (usize index = 0; index < 8; ++index) {
            const auto value = memory.read_u64(
                address + index * sizeof(u64));
            if (value.is_err()) {
                std::fprintf(stderr, " <unmapped>");
                break;
            }
            if (index == 0) next = value.value();
            std::fprintf(stderr, " 0x%llx",
                         static_cast<unsigned long long>(value.value()));
        }
        std::fputc('\n', stderr);
        if (next == 0 || next == address) break;
        address = next;
    }
}

void dump_native64_tls_chain(GuestMemory& memory,
                             const cpu::CpuState& cpu) noexcept {
    const char* configured = std::getenv("VITREO_NATIVE64_DUMP_TLS_INDEX");
    if (configured == nullptr || *configured == '\0') return;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(configured, &end, 0);
    if (end == configured || parsed >= 64) return;

    GuestAddr address = static_cast<GuestAddr>(cpu.seg.gs_desc.base) +
        0x1480u + static_cast<GuestAddr>(parsed) * sizeof(u64);
    std::fprintf(stderr,
                 "[native64-tls-chain] index=%lu teb=0x%llx slot=0x%llx\n",
                 parsed,
                 static_cast<unsigned long long>(cpu.seg.gs_desc.base),
                 static_cast<unsigned long long>(address));
    for (unsigned depth = 0; depth < 3; ++depth) {
        std::fprintf(stderr,
                     "[native64-tls-chain] depth=%u address=0x%llx values:",
                     depth, static_cast<unsigned long long>(address));
        GuestAddr next = 0;
        for (usize index = 0; index < 16; ++index) {
            const auto value = memory.read_u64(address + index * sizeof(u64));
            if (value.is_err()) {
                std::fprintf(stderr, " <unmapped>");
                break;
            }
            if (index == 0) next = value.value();
            std::fprintf(stderr, " 0x%llx",
                         static_cast<unsigned long long>(value.value()));
        }
        std::fputc('\n', stderr);
        if (depth == 1) {
            const GuestAddr cache = address + 0x300u;
            std::fprintf(stderr,
                         "[native64-tls-chain] cache+0x300 address=0x%llx values:",
                         static_cast<unsigned long long>(cache));
            for (usize index = 0; index < 32; ++index) {
                const auto value = memory.read_u64(
                    cache + index * sizeof(u64));
                if (value.is_err()) break;
                std::fprintf(stderr, " 0x%llx",
                             static_cast<unsigned long long>(value.value()));
            }
            std::fputc('\n', stderr);

            const char* follow_config =
                std::getenv("VITREO_NATIVE64_DUMP_TLS_FOLLOW_OFFSET");
            if (follow_config != nullptr && *follow_config != '\0') {
                char* follow_end = nullptr;
                const unsigned long offset =
                    std::strtoul(follow_config, &follow_end, 0);
                if (follow_end != follow_config && offset < 0x10000u) {
                    const auto target = memory.read_u64(address + offset);
                    if (target.is_ok()) {
                        std::fprintf(stderr,
                                     "[native64-tls-chain] follow+0x%lx "
                                     "target=0x%llx values:",
                                     offset,
                                     static_cast<unsigned long long>(
                                         target.value()));
                        for (usize index = 0; index < 16; ++index) {
                            const auto value = memory.read_u64(
                                target.value() + index * sizeof(u64));
                            if (value.is_err()) break;
                            std::fprintf(stderr, " 0x%llx",
                                         static_cast<unsigned long long>(
                                             value.value()));
                        }
                        std::fputc('\n', stderr);
                    }
                }
            }
        }
        if (next == 0 || next == address) break;
        address = next;
    }
}

/// Translate newly loaded Win64 code before it can execute natively. Dynamic
/// DLL loads happen inside API thunks, so this runs both during prepare and at
/// each host boundary. Exact extents are never scanned twice: a guest GS byte
/// becomes host FS, and a second pass would otherwise mistake it for guest FS.
/// Instruction streams inside [begin, end) named by the containing module's
/// x64 exception directory.
///
/// Every function an x64 PE contains is required to carry unwind data, so
/// .pdata is an authoritative list of instruction boundaries -- the one thing
/// a linear sweep of .text cannot recover once it desynchronises on embedded
/// data. Returns false when no module or no exception directory covers the
/// extent, in which case the caller must fall back to the linear sweep.
[[nodiscard]] bool pdata_code_ranges(GuestMemory& memory,
                                     const winapi::ModuleManager* modules,
                                     GuestAddr begin, GuestAddr end,
                                     std::vector<cpu::x64::CodeRange>& out) {
    if (modules == nullptr) return false;
    const auto at = modules->module_for_address(begin);
    if (at.info == nullptr || at.info->bitness != 64) return false;
    if (at.info->exception_rva == 0 || at.info->exception_size < 12) {
        return false;
    }
    const GuestAddr base = at.info->base_address;
    const GuestAddr table = base + at.info->exception_rva;
    const u32 entries = at.info->exception_size / 12;

    out.clear();
    for (u32 i = 0; i < entries; ++i) {
        const auto fn_begin = memory.read_u32(table + i * 12);
        const auto fn_end = memory.read_u32(table + i * 12 + 4);
        if (fn_begin.is_err() || fn_end.is_err()) break;
        if (fn_end.value() <= fn_begin.value()) continue;
        const GuestAddr lo = base + fn_begin.value();
        const GuestAddr hi = base + fn_end.value();
        if (hi <= begin || lo >= end) continue;
        const GuestAddr clipped_lo = std::max(lo, begin);
        const GuestAddr clipped_hi = std::min(hi, end);
        out.push_back({static_cast<usize>(clipped_lo - begin),
                       static_cast<usize>(clipped_hi - clipped_lo)});
    }
    return !out.empty();
}

usize rewrite_new_segment_extents(GuestMemory& memory, bool rewrite,
                                  const winapi::ModuleManager* modules) {
    std::lock_guard<std::mutex> lock(g_segment_rewrite_mutex);
    const u64 map_generation = memory.generation();
    const u64 protect_sequence = memory.protect_sequence();
    if (g_segment_rewrite_memory != &memory) {
        g_segment_rewritten_extents.clear();
        g_segment_rewrite_memory = &memory;
    } else if (g_segment_rewrite_map_generation == map_generation &&
               g_segment_rewrite_protect_sequence == protect_sequence) {
        return 0;
    }

    usize count = 0;
    for (const auto& extent : memory.executable_extents()) {
        if (!g_segment_rewritten_extents.insert(extent).second) continue;
        if (rewrite && extent.second > extent.first) {
            const auto size = static_cast<usize>(extent.second - extent.first);
            std::vector<cpu::x64::CodeRange> ranges;
            const bool guided = pdata_code_ranges(memory, modules,
                                                  extent.first, extent.second,
                                                  ranges);
            // An extent .pdata does not describe still gets swept linearly,
            // but a MODULE's bytes are not a pure instruction stream: the
            // sweep desynchronises on its data and writes over it. A large
            // guest DLL lands here (two extents, every process) and
            // access-violates on
            // Intel where ARM never faults at all. Vitreo-owned regions (the
            // thunk region, guest-generated code) are genuinely all code and
            // keep the unrestricted sweep.
            const bool module_backed =
                modules != nullptr &&
                modules->module_for_address(extent.first).info != nullptr;
            const usize rewrote = cpu::x64::rewrite_segment_overrides_in_range(
                memory, extent.first, size,
                guided ? ranges.data() : nullptr,
                guided ? ranges.size() : 0,
                /*teb_shape_only=*/!guided && module_backed);
            count += rewrote;
            if (std::getenv("VITREO_NATIVE64_TRACE_SEGMENTS") != nullptr) {
                const auto at = modules != nullptr
                    ? modules->module_for_address(extent.first)
                    : winapi::ModuleManager::ModuleAtAddr{};
                std::fprintf(stderr,
                             "[native64-seg] %s extent=0x%llx size=0x%zx "
                             "guided=%d ranges=%zu rewrote=%zu\n",
                             at.info != nullptr ? at.info->name.c_str()
                                                : "<no module>",
                             static_cast<unsigned long long>(extent.first),
                             size, guided ? 1 : 0, ranges.size(), rewrote);
            }
            if (!guided) {
                // Vitreo-owned regions (the thunk region, guest-generated
                // code) have no .pdata and are swept linearly, which is what
                // this path has always done. A *module* landing here means
                // its data bytes are at risk, so say so once.
                const auto at = modules != nullptr
                    ? modules->module_for_address(extent.first)
                    : winapi::ModuleManager::ModuleAtAddr{};
                if (at.info != nullptr) {
                    std::fprintf(stderr,
                                 "[native64] no .pdata for %s at 0x%llx; "
                                 "falling back to a linear segment sweep\n",
                                 at.info->name.c_str(),
                                 static_cast<unsigned long long>(
                                     extent.first));
                }
            }
        }
    }
    g_segment_rewrite_map_generation = map_generation;
    g_segment_rewrite_protect_sequence = protect_sequence;
    return count;
}

/// Swap a logical return sentinel on the guest stack for a real code address.
///
/// Vitreo's sentinels are magic values the interpreter compares RIP against;
/// 0xDEADC0DEFFFF0000 is not even a canonical address, so it can neither be
/// mapped nor jumped to. Native guest code genuinely executes its `ret`, so
/// the value on the stack has to be something executable: the shared sentinel
/// stub, which captures the guest register file and hands control back.
///
/// Only the frame this run owns is touched -- the return address at RSP --
/// and the caller still sees `cpu.rip == sentinel` on return, so nothing
/// above this layer learns that the substitution happened.
void substitute_return_sentinel(GuestMemory& memory, cpu::CpuState& cpu,
                                u64 sentinel) noexcept {
    const auto sp = static_cast<GuestAddr>(cpu.gpr.get64(4));
    const auto on_stack = memory.read_u64(sp);
    if (on_stack.is_ok() && on_stack.value() == sentinel) {
        (void)memory.write_u64(sp, cpu::x64::sentinel_return_address());
    }
}

[[nodiscard]] bool env_flag(const char* name, bool dflt) noexcept {
    const char* v = std::getenv(name);
    if (v == nullptr) return dflt;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y';
}

// ---------------------------------------------------------------------------
// CpuState <-> transfer block
//
// Under native execution the guest registers live in real hardware registers,
// and CpuState becomes a save area populated only at boundaries. These two
// functions are where that shift is made explicit.
// ---------------------------------------------------------------------------

void pack(cpu::CpuState& cpu, Native64Transfer& t) {
    cpu.materialize_lazy_flags();
    t.rax = cpu.gpr.get64(0);  t.rcx = cpu.gpr.get64(1);
    t.rdx = cpu.gpr.get64(2);  t.rbx = cpu.gpr.get64(3);
    t.rsp = cpu.gpr.get64(4);  t.rbp = cpu.gpr.get64(5);
    t.rsi = cpu.gpr.get64(6);  t.rdi = cpu.gpr.get64(7);
    t.r8  = cpu.gpr.get64(8);  t.r9  = cpu.gpr.get64(9);
    t.r10 = cpu.gpr.get64(10); t.r11 = cpu.gpr.get64(11);
    t.r12 = cpu.gpr.get64(12); t.r13 = cpu.gpr.get64(13);
    t.r14 = cpu.gpr.get64(14); t.r15 = cpu.gpr.get64(15);
    t.rip = cpu.rip;
    // Force the reserved-1 bit and drop TF: resuming with TF set would single
    // step the guest into a trap this backend has no handler for.
    t.flags = (cpu.flags.rflags | 0x2ull) & ~0x100ull;
}

void unpack(const Native64Transfer& t, cpu::CpuState& cpu) {
    cpu.gpr.set64(0, t.rax);  cpu.gpr.set64(1, t.rcx);
    cpu.gpr.set64(2, t.rdx);  cpu.gpr.set64(3, t.rbx);
    cpu.gpr.set64(4, t.rsp);  cpu.gpr.set64(5, t.rbp);
    cpu.gpr.set64(6, t.rsi);  cpu.gpr.set64(7, t.rdi);
    cpu.gpr.set64(8, t.r8);   cpu.gpr.set64(9, t.r9);
    cpu.gpr.set64(10, t.r10); cpu.gpr.set64(11, t.r11);
    cpu.gpr.set64(12, t.r12); cpu.gpr.set64(13, t.r13);
    cpu.gpr.set64(14, t.r14); cpu.gpr.set64(15, t.r15);
    cpu.rip = t.rip;
    cpu.flags.rflags = t.flags;
    // The hardware just produced these; nothing is pending.
    cpu.flag_op = cpu::CpuState::FlagOp::None;
    cpu.cf_overlay = 0;
    cpu.of_overlay = 0;
}

// ---------------------------------------------------------------------------
// Guest side
// ---------------------------------------------------------------------------

/// Shared between one guest thread and its servicer. Plain data: the guest
/// side may not allocate.
struct GuestControl {
    volatile bool stop{false};
    /// Selector reloaded into the FS TEB carrier on every entry. Captured by the thread
    /// itself, so there is no race with start() publishing it.
    volatile u32 fs_selector{0};
    /// TEB the thread actually sees through FS, and the one it should. Entering
    /// with the wrong FS loads the register file out of low memory and jumps
    /// to whatever it finds, so it is checked once rather than diagnosed again.
    volatile u64 teb_seen{0};
    u64 teb_expected{0};
    volatile bool fs_bad{false};
    volatile u64 entered{0};
};

/// Runs on the FS-carrier thread for its whole life. LIBC-FREE: `enter()` is
/// assembly plus two plain memory reads, and the hop is atomics.
///
/// The thread parks in `guest_request()` between pieces of work, so one guest
/// thread serves the initial entry, every thunk resume, and every nested
/// host->guest callback on that thread. Creating a new one per callback would
/// burn an LDT descriptor each time and lose the guest's stack.
void guest_driver(GuestHostHop& hop, void* user) {
    auto* ctl = static_cast<GuestControl*>(user);

    u64 teb = 0;
    __asm__ volatile("movq %%fs:0x30, %0" : "=r"(teb));
    ctl->teb_seen = teb;
    u16 sel = 0;
    __asm__ volatile("movw %%fs, %0" : "=r"(sel));
    ctl->fs_selector = sel;

    if (teb != ctl->teb_expected) {
        ctl->fs_bad = true;
        ctl->stop = true;
        hop.guest_finish();
        return;
    }

    // Liveness handshake: the servicer waits for this before its first entry,
    // so a thread that died during setup is reported rather than waited on.
    hop.guest_request();

    while (!ctl->stop) {
        ctl->entered = ctl->entered + 1;
        (void)cpu::x64::enter(static_cast<u16>(ctl->fs_selector));
        hop.guest_request();
    }
    hop.guest_finish();
}

/// Guest<->host round trips on this process. Every Windows API call on this
/// backend costs one, because the FS-carrier thread cannot call libc itself.
/// See VITREO_NATIVE64_HOP_STATS.
std::atomic<u64> g_hop_round_trips{0};

[[nodiscard]] bool hop_stats_enabled() noexcept {
    static const bool on =
        std::getenv("VITREO_NATIVE64_HOP_STATS") != nullptr;
    return on;
}

// ---------------------------------------------------------------------------
// Per-host-thread native64 state
//
// One GuestTebThread per guest thread, owned by the host thread that services
// it. Held in thread_local storage on the *host* side -- host threads still
// have TLS, which is the whole point of the split.
// ---------------------------------------------------------------------------
struct Native64Thread {
    GuestTebThread guest;
    GuestControl ctl;
    Native64Transfer* xfer{nullptr};
    u64 teb{0};
    bool handshaken{false};

    ~Native64Thread() {
        ctl.stop = true;
        // Release the parked guest thread so it can observe stop and leave.
        hop_release();
        (void)guest.wait_until_exited();
    }

    void hop_release() noexcept { guest.hop().host_complete(); }
};

thread_local Native64Thread* t_native64 = nullptr;

/// The native64 thread serving the calling host thread, created on first use.
///
/// @param teb Guest TEB for this thread. Must be below 4GB: an LDT descriptor
///            base is 32 bits wide, and that is a placement rule for every
///            native64 guest thread rather than a property of the first one.
[[nodiscard]] Native64Thread* native64_thread_for_current(GuestMemory& memory,
                                                          u64 teb) {
    if (t_native64 != nullptr) return t_native64;

    if (teb == 0 || teb >= cpu::x64::kSegmentBaseLimit) {
        std::fprintf(stderr,
                     "[native64] TEB 0x%llx is not below 4GB; a segment "
                     "descriptor cannot reach it\n",
                     static_cast<unsigned long long>(teb));
        return nullptr;
    }
    u8* teb_host = memory.linear_host(static_cast<GuestAddr>(teb));
    if (teb_host == nullptr) {
        std::fprintf(stderr, "[native64] TEB 0x%llx has no host mapping\n",
                     static_cast<unsigned long long>(teb));
        return nullptr;
    }

    auto* st = new Native64Thread();
    st->teb = teb;
    st->xfer = Native64Transfer::at(teb_host);
    st->ctl.teb_expected = teb;
    cpu::x64::publish_gs_hooks(teb_host);

    if (st->guest.start(teb, &guest_driver, &st->ctl).is_err()) {
        delete st;
        return nullptr;
    }
    t_native64 = st;
    return st;
}

/// Why a run_until() stopped.
enum class Native64Stop { Sentinel, ProcessExit, ControlTransferred, Unresolved,
                          Unhandled, Fatal, Wedged };

[[nodiscard]] const char* native64_stop_name(Native64Stop stop) noexcept {
    switch (stop) {
        case Native64Stop::Sentinel: return "sentinel";
        case Native64Stop::ProcessExit: return "process-exit";
        case Native64Stop::ControlTransferred: return "control-transferred";
        case Native64Stop::Unresolved: return "unresolved";
        case Native64Stop::Unhandled: return "unhandled";
        case Native64Stop::Fatal: return "fatal";
        case Native64Stop::Wedged: return "wedged";
    }
    return "unknown";
}

/// Drive the guest until it reaches @p sentinel, the process exits, or a thunk
/// cannot be dispatched.
///
/// The guest thread is left parked in `guest_request()` on return, so the next
/// call resumes the same thread with the same stack.
[[nodiscard]] Native64Stop run_until(cpu::CpuState& cpu,
                                     GuestMemory& memory,
                                     winapi::ThunkRegistry& thunk_registry,
                                     winapi::ProcessManager& process_mgr,
                                     Native64Thread& st,
                                     u64 sentinel,
                                     EventPumpCallback event_pump,
                                     u64* thunks_out,
                                     bool trace) {
    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto suspend_guard = winapi::ThreadSuspendRegistry::instance().publish(
        winapi::ThreadManager::get_current_thread_id_static(), &cpu);
    constexpr usize kFaultHistorySize = 64;
    std::array<std::pair<GuestAddr, GuestAddr>, kFaultHistorySize>
        fault_history{};
    usize fault_history_count = 0;
    const bool fault_history_enabled =
        std::getenv("VITREO_NATIVE64_FAULT_RING") != nullptr;
    const char* trace_thunk_filter =
        std::getenv("VITREO_NATIVE64_TRACE_THUNK");
    const char* trace_after_filter =
        std::getenv("VITREO_NATIVE64_TRACE_AFTER_THUNK");
    const u32 trace_after_count = [] {
        const char* configured =
            std::getenv("VITREO_NATIVE64_TRACE_AFTER_COUNT");
        if (configured == nullptr || *configured == '\0') return 32u;
        const unsigned long parsed = std::strtoul(configured, nullptr, 0);
        return static_cast<u32>(std::min(parsed, 1024ul));
    }();
    u32 trace_after_remaining = 0;

    constexpr u64 kHandshakeTimeoutNs = 5'000'000'000ULL;
    if (!st.handshaken) {
        if (st.guest.hop().host_wait(kHandshakeTimeoutNs) !=
            GuestHostHop::Wait::Request) {
            return Native64Stop::Wedged;
        }
        if (st.ctl.fs_bad) {
            std::fprintf(stderr,
                         "[native64] guest FS carrier wrong: reads TEB 0x%llx, "
                         "expected 0x%llx\n",
                         static_cast<unsigned long long>(st.ctl.teb_seen),
                         static_cast<unsigned long long>(st.ctl.teb_expected));
            return Native64Stop::Fatal;
        }
        st.handshaken = true;
        if (trace) {
            std::fprintf(stderr,
                         "[native64] guest thread live: teb=0x%llx fs=0x%04x "
                         "mach=%u\n",
                         static_cast<unsigned long long>(st.ctl.teb_seen),
                         static_cast<unsigned>(st.ctl.fs_selector),
                         st.guest.mach_port());
        }
    }

    substitute_return_sentinel(memory, cpu, sentinel);

    u64 pumps = 0;
    for (;;) {
        pack(cpu, *st.xfer);
        // VITREO_NATIVE64_HOP_STATS: every Windows API call on this backend
        // costs a round trip to the guest's dedicated FS-carrier thread,
        // because that thread's GS is swapped and it cannot call libc. The
        // ARM backend calls the same thunk inline. Counting the trips is the
        // only way to tell an emulation-cost problem from a latency one.
        // The guest now services GetLastError/SetLastError out of its own TEB,
        // so the TEB and the host's thread_local must agree across every
        // boundary. Only one side runs at a time, so a plain copy is enough.
        {
            auto* teb_bytes = reinterpret_cast<u8*>(st.xfer) -
                              cpu::x64::kN64BlockBegin;
            std::memcpy(teb_bytes + cpu::x64::kTebLastErrorValue,
                        &winapi::g_last_error, sizeof(u32));
        }
        {
            const u64 trips =
                g_hop_round_trips.fetch_add(1, std::memory_order_relaxed) + 1;
            if (hop_stats_enabled() && (trips % 250000ULL) == 0) {
                std::fprintf(stderr,
                             "[native64-hops] pid=%d round_trips=%llu\n",
                             static_cast<int>(::getpid()),
                             static_cast<unsigned long long>(trips));
                std::fflush(stderr);
            }
        }
        st.guest.hop().host_complete();     // release the guest

        if (st.guest.hop().host_wait(0) != GuestHostHop::Wait::Request) {
            return Native64Stop::Wedged;
        }

        {
            const auto* teb_bytes = reinterpret_cast<const u8*>(st.xfer) -
                                    cpu::x64::kN64BlockBegin;
            std::memcpy(&winapi::g_last_error,
                        teb_bytes + cpu::x64::kTebLastErrorValue, sizeof(u32));
        }

        const auto why = static_cast<Native64Exit>(st.xfer->reason);
        unpack(*st.xfer, cpu);

        // TerminateThread: leave guest code at the next host hop, matching
        // the interpreter/callback loops. A target that never thunks is still
        // interrupted by ntsync::interrupt_thread when it is parked in a wait.
        suspend_guard.checkpoint();
        if (suspend_guard.terminate_requested()) {
            cpu.rip = sentinel;
            return Native64Stop::Sentinel;
        }

        if (why == Native64Exit::ReturnedToSentinel || cpu.rip == sentinel) {
            cpu.rip = sentinel;
            return Native64Stop::Sentinel;
        }
        if (why == Native64Exit::Fault) {
            if (fault_history_enabled) {
                const auto* modules = process_mgr.module_manager();
                const auto fault_module = modules != nullptr
                    ? modules->module_for_address(cpu.rip)
                    : winapi::ModuleManager::ModuleAtAddr{};
                std::fprintf(
                    stderr,
                    "[native64-fault] first-chance status=0x%08x "
                    "rip=0x%llx address=0x%llx write=%u "
                    "rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx "
                    "r9=0x%llx rsp=0x%llx module=%s rva=0x%x\n",
                    static_cast<unsigned>(st.xfer->fault_status),
                    static_cast<unsigned long long>(cpu.rip),
                    static_cast<unsigned long long>(st.xfer->fault_address),
                    static_cast<unsigned>(st.xfer->fault_write),
                    static_cast<unsigned long long>(cpu.gpr.get64(0)),
                    static_cast<unsigned long long>(cpu.gpr.get64(1)),
                    static_cast<unsigned long long>(cpu.gpr.get64(2)),
                    static_cast<unsigned long long>(cpu.gpr.get64(8)),
                    static_cast<unsigned long long>(cpu.gpr.get64(9)),
                    static_cast<unsigned long long>(cpu.gpr.get64(4)),
                    fault_module.info != nullptr
                        ? fault_module.info->path.c_str() : "<none>",
                    fault_module.rva);
                // The four argument registers alone have repeatedly proved
                // too narrow: a bogus `this` usually arrives in a
                // callee-saved register several frames up. Print the whole
                // file so one repro carries the whole picture.
                std::fprintf(
                    stderr,
                    "[native64-fault] gpr rbx=0x%llx rsp=0x%llx rbp=0x%llx "
                    "rsi=0x%llx rdi=0x%llx r10=0x%llx r11=0x%llx r12=0x%llx "
                    "r13=0x%llx r14=0x%llx r15=0x%llx\n",
                    static_cast<unsigned long long>(cpu.gpr.get64(3)),
                    static_cast<unsigned long long>(cpu.gpr.get64(4)),
                    static_cast<unsigned long long>(cpu.gpr.get64(5)),
                    static_cast<unsigned long long>(cpu.gpr.get64(6)),
                    static_cast<unsigned long long>(cpu.gpr.get64(7)),
                    static_cast<unsigned long long>(cpu.gpr.get64(10)),
                    static_cast<unsigned long long>(cpu.gpr.get64(11)),
                    static_cast<unsigned long long>(cpu.gpr.get64(12)),
                    static_cast<unsigned long long>(cpu.gpr.get64(13)),
                    static_cast<unsigned long long>(cpu.gpr.get64(14)),
                    static_cast<unsigned long long>(cpu.gpr.get64(15)));
                // "module=<none>" leaves the three diagnoses that matter
                // indistinguishable: unmapped, mapped but not executable, or
                // executable holding the wrong bytes. Name the region and
                // show what is actually there.
                {
                    const auto fault_at =
                        static_cast<GuestAddr>(st.xfer->fault_address);
                    GuestAddr region_base = 0;
                    usize region_size = 0;
                    MemoryProtection region_prot{};
                    if (memory.query_region(fault_at, region_base, region_size,
                                            region_prot)) {
                        std::fprintf(
                            stderr,
                            "[native64-fault] region base=0x%llx size=0x%zx "
                            "prot=0x%x exec=%d\n",
                            static_cast<unsigned long long>(region_base),
                            region_size,
                            static_cast<unsigned>(region_prot),
                            memory.has_protection(fault_at, 1,
                                                  MemoryProtection::Execute)
                                ? 1 : 0);
                        std::fprintf(stderr, "[native64-fault] bytes @0x%llx:",
                                     static_cast<unsigned long long>(fault_at));
                        for (usize i = 0; i < 16; ++i) {
                            const auto b = memory.read_u8(fault_at + i);
                            if (b.is_err()) { std::fprintf(stderr, " ??"); break; }
                            std::fprintf(stderr, " %02x",
                                         static_cast<unsigned>(b.value()));
                        }
                        std::fprintf(stderr, "\n");
                    } else {
                        std::fprintf(stderr,
                                     "[native64-fault] region: UNMAPPED "
                                     "(next mapped base=0x%llx)\n",
                                     static_cast<unsigned long long>(
                                         memory.next_region_base(fault_at)));
                    }
                }
                dump_native64_pointer_chain(memory);
                dump_native64_tls_chain(memory, cpu);
                for (usize offset = 0; offset < 16; ++offset) {
                    const auto value = memory.read_u64(
                        static_cast<GuestAddr>(cpu.gpr.get64(4)) +
                        offset * sizeof(u64));
                    if (value.is_err()) break;
                    const auto frame_module = modules != nullptr
                        ? modules->module_for_address(value.value())
                        : winapi::ModuleManager::ModuleAtAddr{};
                    std::fprintf(
                        stderr,
                        "[native64-fault] frame[+0x%zx]=0x%llx "
                        "module=%s rva=0x%x\n",
                        offset * sizeof(u64),
                        static_cast<unsigned long long>(value.value()),
                        frame_module.info != nullptr
                            ? frame_module.info->path.c_str() : "<none>",
                    frame_module.rva);
                }
                const usize retained =
                    std::min(fault_history_count, kFaultHistorySize);
                const usize first = fault_history_count - retained;
                std::fprintf(stderr,
                             "[native64-fault] teb=0x%llx retained_thunks=%zu\n",
                             static_cast<unsigned long long>(
                                 cpu.seg.gs_desc.base),
                             retained);
                for (usize index = first; index < fault_history_count;
                     ++index) {
                    const auto [thunk, caller] =
                        fault_history[index % kFaultHistorySize];
                    std::fprintf(
                        stderr, "[native64-fault] prior-thunk[-%zu] "
                        "0x%llx caller=0x%llx %s\n",
                        fault_history_count - index,
                        static_cast<unsigned long long>(thunk),
                        static_cast<unsigned long long>(caller),
                        thunk_registry.resolve_name(thunk).c_str());
                }
            }
            // Same dispatch the interpreter runs, so a guest's __try/__except
            // and vectored handlers behave identically on both backends.
            if (!dispatch_guest_fault_native(
                    cpu, memory, thunk_registry, process_mgr,
                    static_cast<u32>(st.xfer->fault_status),
                    st.xfer->fault_address,
                    st.xfer->fault_write != 0)) {
                if (fault_history_enabled) {
                    GuestAddr region_base = 0;
                    usize region_size = 0;
                    MemoryProtection region_protection =
                        MemoryProtection::None;
                    const bool has_region = memory.query_region(
                        cpu.rip, region_base, region_size,
                        region_protection);
                    const auto* modules = process_mgr.module_manager();
                    const auto module = modules != nullptr
                        ? modules->module_for_address(cpu.rip)
                        : winapi::ModuleManager::ModuleAtAddr{};
                    std::fprintf(
                        stderr,
                        "[native64-fault] rip=0x%llx rsp=0x%llx "
                        "rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx "
                        "r9=0x%llx\n",
                        static_cast<unsigned long long>(cpu.rip),
                        static_cast<unsigned long long>(cpu.gpr.get64(4)),
                        static_cast<unsigned long long>(cpu.gpr.get64(0)),
                        static_cast<unsigned long long>(cpu.gpr.get64(1)),
                        static_cast<unsigned long long>(cpu.gpr.get64(2)),
                        static_cast<unsigned long long>(cpu.gpr.get64(8)),
                        static_cast<unsigned long long>(cpu.gpr.get64(9)));
                    std::fprintf(
                        stderr,
                        "[native64-fault] rbx=0x%llx rbp=0x%llx "
                        "rsi=0x%llx rdi=0x%llx r12=0x%llx r13=0x%llx "
                        "r14=0x%llx r15=0x%llx\n",
                        static_cast<unsigned long long>(cpu.gpr.get64(3)),
                        static_cast<unsigned long long>(cpu.gpr.get64(5)),
                        static_cast<unsigned long long>(cpu.gpr.get64(6)),
                        static_cast<unsigned long long>(cpu.gpr.get64(7)),
                        static_cast<unsigned long long>(cpu.gpr.get64(12)),
                        static_cast<unsigned long long>(cpu.gpr.get64(13)),
                        static_cast<unsigned long long>(cpu.gpr.get64(14)),
                        static_cast<unsigned long long>(cpu.gpr.get64(15)));
                    std::fprintf(
                        stderr,
                        "[native64-fault] region=%s base=0x%llx size=0x%zx "
                        "prot=0x%x module=%s rva=0x%x\n",
                        has_region ? "mapped" : "unmapped",
                        static_cast<unsigned long long>(region_base),
                        region_size,
                        static_cast<unsigned>(region_protection),
                        module.info != nullptr ? module.info->path.c_str()
                                               : "<none>",
                        module.rva);
                    std::fprintf(stderr, "[native64-fault] bytes:");
                    for (usize offset = 0; offset < 16; ++offset) {
                        const auto byte = memory.read_u8(cpu.rip + offset);
                        if (byte.is_err()) break;
                        std::fprintf(stderr, " %02x",
                                     static_cast<unsigned>(byte.value()));
                    }
                    std::fputc('\n', stderr);
                    const usize retained =
                        std::min(fault_history_count, kFaultHistorySize);
                    const usize first = fault_history_count - retained;
                    for (usize offset = 0; offset < 12; ++offset) {
                        const GuestAddr address =
                            static_cast<GuestAddr>(cpu.gpr.get64(4)) +
                            offset * sizeof(u64);
                        const auto value = memory.read_u64(address);
                        if (value.is_err()) break;
                        std::fprintf(
                            stderr,
                            "[native64-fault] stack[+0x%zx]=0x%llx\n",
                            offset * sizeof(u64),
                            static_cast<unsigned long long>(value.value()));
                    }
                    for (usize index = first; index < fault_history_count;
                         ++index) {
                        const auto [thunk, caller] =
                            fault_history[index % kFaultHistorySize];
                        std::fprintf(
                            stderr, "[native64-fault] thunk[-%zu] "
                            "0x%llx caller=0x%llx %s\n",
                            fault_history_count - index,
                            static_cast<unsigned long long>(thunk),
                            static_cast<unsigned long long>(caller),
                            thunk_registry.resolve_name(thunk).c_str());
                    }
                }
                // An unhandled exception ends the process, so this is the
                // last chance to see what the guest was doing. The ring is
                // opt-in (VITREO_HOSTCALL_RING) and says nothing otherwise.
                vitreo::diagnostics::dump_hostcalls(
                    "native64-unhandled-exception");
                std::fprintf(stderr,
                             "[native64] unhandled guest exception 0x%08x at "
                             "0x%llx (fault addr 0x%llx)\n",
                             static_cast<unsigned>(st.xfer->fault_status),
                             static_cast<unsigned long long>(cpu.rip),
                             static_cast<unsigned long long>(
                                 st.xfer->fault_address));
                return Native64Stop::Unhandled;
            }
            if (process_mgr.has_exited()) return Native64Stop::ProcessExit;
            continue;
        }
        if (why != Native64Exit::ThunkCall) {
            return Native64Stop::Fatal;
        }

        if (thunks_out) ++*thunks_out;
        if (fault_history_enabled) {
            const auto caller = memory.read_u64(
                static_cast<GuestAddr>(cpu.gpr.get64(4)));
            fault_history[fault_history_count % kFaultHistorySize] = {
                static_cast<GuestAddr>(cpu.rip), caller.value_or(0)};
            ++fault_history_count;
        }
        if (trace && thunks_out != nullptr && *thunks_out <= 5000) {
            std::fprintf(stderr, "[native64] thunk #%llu @0x%llx %s\n",
                         static_cast<unsigned long long>(*thunks_out),
                         static_cast<unsigned long long>(cpu.rip),
                         thunk_registry.resolve_name(
                             static_cast<GuestAddr>(cpu.rip)).c_str());
        }

        const std::string traced_name =
            (trace || (trace_thunk_filter != nullptr &&
                       *trace_thunk_filter != '\0') ||
             (trace_after_filter != nullptr && *trace_after_filter != '\0'))
                ? thunk_registry.resolve_name(static_cast<GuestAddr>(cpu.rip))
                : std::string{};
        const bool direct_trace_match = trace_thunk_filter != nullptr &&
            *trace_thunk_filter != '\0' &&
            traced_name.find(trace_thunk_filter) != std::string::npos;
        if (trace_after_filter != nullptr && *trace_after_filter != '\0' &&
            traced_name.find(trace_after_filter) != std::string::npos) {
            trace_after_remaining = trace_after_count;
        }
        const bool trace_this_thunk = direct_trace_match ||
            trace_after_remaining != 0;
        const u64 traced_caller = trace_this_thunk
            ? memory.read_u64(static_cast<GuestAddr>(cpu.gpr.get64(4)))
                  .value_or(0)
            : 0;
        const u64 traced_rbx = cpu.gpr.get64(3);
        const u64 traced_rdi = cpu.gpr.get64(7);
        const u64 arg0 = cpu.gpr.get64(1);   // RCX
        const u64 arg2 = cpu.gpr.get64(2);   // RDX
        const u64 arg3 = cpu.gpr.get64(8);   // R8

        const auto d = dispatch_api_thunk_native(
            cpu, memory, thunk_registry, marshaler, suspend_guard);

        const usize translated = rewrite_new_segment_extents(memory, true, process_mgr.module_manager());
        if (translated != 0 && trace) {
            std::fprintf(stderr,
                         "[native64] translated %zu Win64 segment override(s) "
                         "in newly executable code\n",
                         translated);
        }

        if (trace || trace_this_thunk) {
            std::fprintf(stderr,
                         "[native64]   %s(0x%llx, 0x%llx, 0x%llx) -> 0x%llx "
                         "resume=0x%llx caller=0x%llx teb=0x%llx "
                         "rbx=0x%llx->0x%llx rdi=0x%llx->0x%llx\n",
                         traced_name.c_str(),
                         static_cast<unsigned long long>(arg0),
                         static_cast<unsigned long long>(arg2),
                         static_cast<unsigned long long>(arg3),
                         static_cast<unsigned long long>(cpu.gpr.get64(0)),
                         static_cast<unsigned long long>(cpu.rip),
                         static_cast<unsigned long long>(traced_caller),
                         static_cast<unsigned long long>(cpu.seg.gs_desc.base),
                         static_cast<unsigned long long>(traced_rbx),
                         static_cast<unsigned long long>(cpu.gpr.get64(3)),
                         static_cast<unsigned long long>(traced_rdi),
                         static_cast<unsigned long long>(cpu.gpr.get64(7)));
        }
        if (trace_after_remaining != 0) --trace_after_remaining;
        if (d == NativeThunkDispatch::Unresolved) return Native64Stop::Unresolved;
        if (d == NativeThunkDispatch::Fatal) return Native64Stop::Fatal;

        // ExitProcess and friends are noreturn: the handler has already torn
        // the process down and the guest's return address points at the int3
        // the CRT parks after such a call. Resuming would execute it.
        if (process_mgr.has_exited()) return Native64Stop::ProcessExit;

        // RtlUnwindEx and the other non-local transfer contracts deliberately
        // leave RIP at the dispatch gateway: their enclosing exception or
        // callback dispatcher owns the next instruction. Re-entering native
        // code here would execute the gateway's guard INT3 instead.
        if (d == NativeThunkDispatch::ControlTransferred) {
            return Native64Stop::ControlTransferred;
        }

        if (event_pump && (++pumps & 0x3FFu) == 0) event_pump();
    }
}

} // namespace

bool native64_wanted() noexcept {
    // Native64 has guest-thread/callback and first-chance fault dispatch. CPUID
    // deliberately reports the real Intel CPU (documented in the backend
    // contract), because these instructions execute on that CPU. Keep the
    // environment switch as an explicit diagnostic opt-out, but do not make
    // every Win64 process silently fall back to the interpreter by default.
    return env_flag("VITREO_NATIVE64", true);
}

bool native64_is_prepared() noexcept { return g_prepared; }


void native64_report_hop_stats(const char* when) noexcept {
    if (std::getenv("VITREO_NATIVE64_HOP_STATS") == nullptr) return;
    std::fprintf(stderr, "[native64-hops] %s pid=%d round_trips=%llu\n",
                 when, static_cast<int>(::getpid()),
                 static_cast<unsigned long long>(
                     g_hop_round_trips.load(std::memory_order_relaxed)));
    std::fflush(stderr);
}

/// Registry the installed dispatch-alias resolver reads. Set once by
/// native64_prepare, before any guest thread can fault.
std::atomic<const winapi::ThunkRegistry*> g_alias_registry{nullptr};

VoidResult native64_prepare(GuestMemory& memory,
                            winapi::ThunkRegistry& thunk_registry,
                            const winapi::ModuleManager* modules) {
    if (g_prepared) return VoidResult::ok();

    const GuestAddr thunk_base = thunk_registry.thunk_base();
    if (thunk_base == 0) {
        std::fprintf(stderr, "[native64] no thunk region\n");
        return VoidResult::err(Error::InvalidArgument);
    }

    // The fault bridge lives in core/ and must not reach into winapi/, so the
    // registry lookup is installed from here instead. One registry per
    // process; the guest address space it names is this one.
    g_alias_registry.store(&thunk_registry, std::memory_order_release);
    cpu::x64::native64_set_dispatch_alias_resolver(
        [](u64 rip) noexcept -> u64 {
            const auto* registry =
                g_alias_registry.load(std::memory_order_acquire);
            return registry != nullptr
                ? static_cast<u64>(registry->alias_dispatch_gateway(
                      static_cast<GuestAddr>(rip)))
                : 0;
        });
    cpu::x64::native64_set_service_resolver(
        [](u32 service_id) noexcept -> u64 {
            const auto* registry =
                g_alias_registry.load(std::memory_order_acquire);
            if (registry == nullptr ||
                service_id > std::numeric_limits<u16>::max()) {
                return 0;
            }
            const auto thunk = registry->get_thunk_address(
                "ntdll.dll", static_cast<u16>(service_id));
            return thunk.is_ok() ? static_cast<u64>(thunk.value()) : 0;
        });

    const usize translated = rewrite_new_segment_extents(memory, true, modules);

    (void)memory.protect_region(thunk_base, cpu::x64::kN64ThunkRegionMapSize,
                                MemoryProtection::ReadWrite);
    u8* host = memory.linear_host(thunk_base);
    if (host == nullptr) {
        std::fprintf(stderr, "[native64] thunk region has no host mapping\n");
        return VoidResult::err(Error::OutOfMemory);
    }
    for (u32 off = 0; off < cpu::x64::kN64ThunkRegionMapSize; off += 16) {
        cpu::x64::emit_thunk_stub(host + off);
    }

    // AMD64 CFG dispatch is not an ordinary API call. The Windows loader's
    // fallback contract takes the guarded target in RAX and tail-jumps to it.
    // Keeping this one slot native avoids a guest/host round trip on every
    // C++ virtual/indirect call while preserving the original CALL's return
    // address and all final-target argument registers.
    const auto guard_dispatch = thunk_registry.get_thunk_address(
        "vitreo-cfg.dll", "__guard_dispatch_icall");
    if (guard_dispatch.is_err() || guard_dispatch.value() < thunk_base ||
        guard_dispatch.value() + cpu::x64::thunk_stub_size() >
            thunk_base + cpu::x64::kN64ThunkRegionMapSize) {
        std::fprintf(stderr, "[native64] no valid CFG dispatch thunk\n");
        return VoidResult::err(Error::NotFound);
    }
    cpu::x64::emit_guard_dispatch_stub(
        host + static_cast<usize>(guard_dispatch.value() - thunk_base));
    // GetLastError/SetLastError are just TEB.LastErrorValue on Windows, and
    // FS already carries the guest TEB here, so the guest can service them
    // itself. Measured on launcher startup: those two were ~2.1M calls against
    // 2,061,916 total guest<->host round trips, i.e. essentially all of them.
    // Every trip is two context switches, which is why the Intel backend
    // loaded that launcher 5.8x slower than ARM despite running guest code
    // natively.
    const auto emit_teb_last_error = [&](const char* name, bool getter) {
        const auto slot = thunk_registry.get_thunk_address("kernel32.dll", name);
        if (slot.is_err() || slot.value() < thunk_base ||
            slot.value() + 9u > thunk_base + cpu::x64::kN64ThunkRegionMapSize) {
            std::fprintf(stderr,
                         "[native64] no valid thunk slot for %s; it keeps the "
                         "round-trip path\n", name);
            return;
        }
        u8* at = host + static_cast<usize>(slot.value() - thunk_base);
        if (getter) cpu::x64::emit_get_last_error_stub(at);
        else        cpu::x64::emit_set_last_error_stub(at);
    };
    emit_teb_last_error("GetLastError", true);
    emit_teb_last_error("SetLastError", false);

    (void)memory.protect_region(thunk_base, cpu::x64::kN64ThunkRegionMapSize,
                                MemoryProtection::ReadExecute);

    // The thunk region is Vitreo-owned and deliberately uses host FS. Record
    // its final executable extent without translating it as guest code.
    (void)rewrite_new_segment_extents(memory, false, nullptr);

    // Deliberately NO install_guest_crt_accelerators() here, unlike
    // native32_prepare. The accelerators emit 32-bit x86 and reach the TEB
    // through fs:[0x24]/fs:[0], which is the 32-bit TEB layout; run.cpp gates
    // them on !pe.is_64bit for that reason. Re-applying them here sends the
    // guest into the 0xCC fill. Reproduced: a launcher died 22 thunks in at a
    // wild thunk-region address, the same shape as the 64-bit helper failure
    // recorded at that gate.
    if (std::getenv("VITREO_NATIVE64_HOP_STATS") != nullptr) {
        static const int armed = std::atexit([] {
            std::fprintf(stderr,
                         "[native64-hops] pid=%d round_trips=%llu\n",
                         static_cast<int>(::getpid()),
                         static_cast<unsigned long long>(
                             g_hop_round_trips.load(
                                 std::memory_order_relaxed)));
            std::fflush(stderr);
        });
        (void)armed;
    }
    g_prepared = true;
    std::fprintf(stderr,
                 "[native64] thunk + sentinel stubs installed "
                 "(shared, FS-relative); translated %zu guest segment "
                 "override(s)\n", translated);
    return VoidResult::ok();
}

RunResult run_native64_loop(cpu::CpuState& cpu,
                            GuestMemory& memory,
                            winapi::ThunkRegistry& thunk_registry,
                            winapi::ProcessManager& process_mgr,
                            const RunConfig& config,
                            EventPumpCallback event_pump) {
    (void)config;
    RunResult result{};
    const bool trace = env_flag("VITREO_NATIVE64_TRACE", false);

    Native64Thread* st =
        native64_thread_for_current(memory, cpu.seg.gs_desc.base);
    if (st == nullptr) {
        result.exit_code = 1;
        result.error_message = "native64 thread setup failed";
        return result;
    }

    if (trace) {
        std::fprintf(stderr,
                     "[native64] entering guest: rip=0x%llx rsp=0x%llx "
                     "teb=0x%llx tid=%u\n",
                     static_cast<unsigned long long>(cpu.rip),
                     static_cast<unsigned long long>(cpu.gpr.get64(4)),
                     static_cast<unsigned long long>(st->teb),
                     winapi::ThreadManager::get_current_thread_id_static());
    }

    u64 thunks = 0;
    const Native64Stop stop =
        run_until(cpu, memory, thunk_registry, process_mgr, *st,
                  kProcessReturnSentinel, event_pump, &thunks, trace);
    result.instructions_executed = thunks;

    switch (stop) {
        case Native64Stop::ProcessExit:
            result.exit_code = static_cast<int>(process_mgr.exit_code());
            result.success = true;
            return result;
        case Native64Stop::Sentinel:
            result.exit_code =
                static_cast<int>(cpu.gpr.get64(0) & 0xFFFFFFFFull);
            result.success = true;
            return result;
        case Native64Stop::Unresolved:
            result.exit_code = -1;
            result.error_message =
                "Unresolved API call at thunk address 0x" +
                std::to_string(cpu.rip);
            return result;
        case Native64Stop::Wedged:
            result.exit_code = 1;
            result.error_message = "native64 guest thread wedged";
            return result;
        case Native64Stop::Unhandled:
            result.exit_code = -1;
            result.error_message = "unhandled guest exception";
            return result;
        case Native64Stop::ControlTransferred:
            result.exit_code = -1;
            result.error_message =
                "native64 control transfer escaped its dispatcher";
            return result;
        case Native64Stop::Fatal:
            break;
    }
    result.exit_code = -1;
    result.error_message = "native64 guest stopped without a handler";
    return result;
}

bool native64_run_until_sentinel(cpu::CpuState& cpu,
                                 GuestMemory& memory,
                                 winapi::ThunkRegistry& thunk_registry,
                                 winapi::ProcessManager& process_mgr,
                                 u64 sentinel,
                                 u64* result_rax,
                                 bool* completed_normally) {
    if (!g_prepared) return false;

    // Every guest thread must go native together. The thunk region holds one
    // encoding, and native64_prepare filled it with `call gs:[collect]` stubs
    // the interpreter cannot decode -- so a thread left on the interpreter
    // would mis-execute every import call rather than merely run slowly.
    Native64Thread* st =
        native64_thread_for_current(memory, cpu.seg.gs_desc.base);
    if (st == nullptr) {
        static thread_local bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[native64] no guest thread for tid=%u (TEB 0x%llx); "
                         "this thread cannot run natively and the thunk region "
                         "is not interpretable\n",
                         winapi::ThreadManager::get_current_thread_id_static(),
                         static_cast<unsigned long long>(cpu.seg.gs_desc.base));
        }
        return false;
    }

    const bool trace = env_flag("VITREO_NATIVE64_TRACE", false);
    const Native64Stop stop =
        run_until(cpu, memory, thunk_registry, process_mgr, *st, sentinel,
                  nullptr, nullptr, trace);

    if (stop != Native64Stop::Sentinel) {
        std::fprintf(
            stderr,
            "[native64-callback] abnormal stop=%s tid=%u teb=0x%llx "
            "rip=0x%llx rsp=0x%llx rax=0x%llx pending_unwind=%u\n",
            native64_stop_name(stop),
            winapi::ThreadManager::get_current_thread_id_static(),
            static_cast<unsigned long long>(st->teb),
            static_cast<unsigned long long>(cpu.rip),
            static_cast<unsigned long long>(cpu.gpr.get64(4)),
            static_cast<unsigned long long>(cpu.gpr.get64(0)),
            cpu.pending_unwind.active ? 1u : 0u);
    }

    if (result_rax) *result_rax = cpu.gpr.get64(0);
    if (completed_normally) {
        *completed_normally = (stop == Native64Stop::Sentinel);
    }
    return true;
}

} // namespace vitreo::cli

#endif // VITREO_ARCH_X86_64
