// SPDX-License-Identifier: LGPL-2.1-only

/// @file native32_loop.cpp
/// @brief Native 32-bit guest run loop. See native32_loop.hpp.

#include "runtime/execution/native32_loop.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "graphics/common/autorelease_scope.hpp"
#include "core/cpu/x64/cpuid_intercept.hpp"

#include "runtime/execution/exception_dispatch32.hpp"
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include "core/memory/write_watch_signal.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "core/cpu/x64/compat32.hpp"
#include "core/cpu/x64/x64_exception_bridge.hpp"
#include "core/cpu/x64/x64_segments.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/state/fx_image.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/guest_accel.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "core/math/host_fenv.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/guest_sentinels.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <vector>

#if defined(VITREO_ARCH_X86_64)

namespace vitreo::cli {
namespace {

using vitreo::cpu::x64::Compat32Executor;
using vitreo::cpu::x64::Compat32Exit;
using vitreo::cpu::x64::Compat32State;
using vitreo::cpu::x64::SegmentTable;
using vitreo::cpu::x64::X64ExceptionBridge;

bool g_prepared = false;

constexpr u32 kThunkRegionMapSize = 0x200000;
// The page holding a sentinel is DERIVED from the sentinel, never written
// alongside it. A hand-maintained pair drifts: kCallbackSentinelPage was
// 0xDEAD0000 while the callback sentinel is 0xDEADC0DE, putting the stub
// 0xC0DE bytes into a 0x1000-byte mapping and faulting on the write.

u16 fs_selector_for(u64 teb_base) {
    if (teb_base == 0 || teb_base >= cpu::x64::kSegmentBaseLimit) return 0;
    static std::mutex mu;
    static std::unordered_map<u64, u16> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(teb_base);
    if (it != cache.end()) return it->second;
    auto r = SegmentTable::allocate_data_segment(teb_base);
    if (r.is_err()) return 0;
    cache.emplace(teb_base, r.value());
    return r.value();
}

// The guest's x87/MMX/SSE file lives in the hardware registers while guest code
// runs, and in the trampoline's FXSAVE image whenever it does not: `ret64`
// FXSAVEs it on the way out and the entry paths FXRSTOR it on the way back in.
// CpuState carries the *host-side* view of that same file, and anything that
// emulates a guest instruction -- the interpreter fallback, an instruction
// stepped after a write-watch fault -- reads and writes CpuState.
//
// Without a marshal in both directions those two views drift: the interpreter
// computes from a stale FpuState/SseState, and the FXRSTOR on re-entry then
// discards whatever it produced. A codec inner loop that faults mid-computation
// (write-watch fires on every decoded output page) resumes with the
// wrong MM/XMM contents and computes garbage with no fault of its own.
//
// The trampoline uses FXSAVE64/FXRSTOR64, so the image is always the wide
// (64-bit FIP/FDP) layout. All 16 XMM slots are carried even though
// compatibility mode can only address XMM0-7: the image is the same buffer the
// hardware FXSAVE64/FXRSTOR64 pair uses, so transferring 8 would zero the
// upper half on every entry -- and Vitreo's own MMX decode aliases some
// MMX-encoded ops onto xmm8-15, which would then be destroyed rather than
// preserved. A real context switch preserves the whole file.
constexpr bool kFxWide = true;
constexpr u8 kFxXmmCount = 16;

void fx_to_guest(cpu::CpuState& cpu, Compat32Executor& exec) {
    cpu::fx_image_save(cpu, static_cast<u8*>(exec.fxsave_area()),
                       kFxWide, kFxXmmCount);
}

void fx_from_guest(cpu::CpuState& cpu, Compat32Executor& exec) {
    cpu::fx_image_restore(cpu, static_cast<const u8*>(exec.fxsave_area()),
                          kFxWide, kFxXmmCount);
}

// Wall-time attribution for the native32 loop, gated on VITREO_NATIVE32_PROFILE.
//
// This path deliberately bypasses guest_run_loop.cpp, so VITREO_PROFILE reports
// nothing for it and there was no way to tell guest execution apart from
// Vitreo's own cost. That distinction is the whole reason to run on Intel:
// native32 executes the guest's instructions on the CPU with no translation, so
// whatever the guest is NOT spending its time in is the API/runtime layer.
//
// Three buckets, measured where the loop already changes mode:
//   guest   -- inside compatibility mode, i.e. real x86 running at hardware speed
//   thunks  -- inside a Vitreo Win32 implementation
//   faults  -- the Mach exception round trip plus guest SEH dispatch
// Anything left over is the loop's own bookkeeping.
struct Native32Profile {
    using Clock = std::chrono::steady_clock;
    struct Site { u64 calls = 0; u64 ns = 0; };

    bool enabled = false;
    Clock::time_point start{};
    Clock::time_point last_dump{};
    u64 guest_ns = 0, thunk_ns = 0, fault_ns = 0;
    u64 thunk_calls = 0, fault_calls = 0;
    std::unordered_map<u32, Site> sites;

    static Native32Profile& current() {
        static thread_local Native32Profile p = [] {
            Native32Profile v;
            v.enabled = std::getenv("VITREO_NATIVE32_PROFILE") != nullptr;
            v.start = v.last_dump = Clock::now();
            return v;
        }();
        return p;
    }

    void dump(const winapi::ThunkRegistry& registry) {
        const auto now = Clock::now();
        const u64 wall = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
        if (wall == 0) return;
        auto pct = [wall](u64 v) { return 100.0 * static_cast<double>(v) / static_cast<double>(wall); };
        const u64 accounted = guest_ns + thunk_ns + fault_ns;
        std::fprintf(stderr,
            "[n32-prof] tid=%u wall=%.1fs guest=%.1fs (%.1f%%) thunks=%.1fs (%.1f%%, n=%llu) "
            "faults=%.1fs (%.1f%%, n=%llu) other=%.1f%%\n",
            winapi::ThreadManager::get_current_thread_id_static(),
            wall / 1e9, guest_ns / 1e9, pct(guest_ns), thunk_ns / 1e9, pct(thunk_ns),
            static_cast<unsigned long long>(thunk_calls),
            fault_ns / 1e9, pct(fault_ns),
            static_cast<unsigned long long>(fault_calls),
            pct(wall > accounted ? wall - accounted : 0));

        std::vector<std::pair<u32, Site>> top(sites.begin(), sites.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
        const usize show = top.size() < 12 ? top.size() : 12;
        for (usize i = 0; i < show; ++i) {
            const auto& [addr, site] = top[i];
            std::fprintf(stderr,
                "[n32-prof]   %-34s n=%-8llu total=%7.2fs avg=%8.1fus (%.1f%% wall)\n",
                registry.resolve_name(static_cast<GuestAddr>(addr)).c_str(),
                static_cast<unsigned long long>(site.calls),
                site.ns / 1e9,
                site.calls ? (site.ns / 1e3) / static_cast<double>(site.calls) : 0.0,
                pct(site.ns));
        }
        last_dump = now;
    }

    void maybe_dump(const winapi::ThunkRegistry& registry) {
        if (Clock::now() - last_dump >= std::chrono::seconds(15)) dump(registry);
    }
};

void pack(cpu::CpuState& cpu, Compat32State& st) {
    cpu.materialize_lazy_flags();
    st.eax = cpu.gpr.get32(0);
    st.ecx = cpu.gpr.get32(1);
    st.edx = cpu.gpr.get32(2);
    st.ebx = cpu.gpr.get32(3);
    st.esp = cpu.gpr.get32(4);
    st.ebp = cpu.gpr.get32(5);
    st.esi = cpu.gpr.get32(6);
    st.edi = cpu.gpr.get32(7);
    st.eip = static_cast<u32>(cpu.rip);
    st.eflags = cpu.flags.eflags();
}

void unpack(const Compat32State& st, cpu::CpuState& cpu) {
    cpu.gpr.set32(0, st.eax);
    cpu.gpr.set32(1, st.ecx);
    cpu.gpr.set32(2, st.edx);
    cpu.gpr.set32(3, st.ebx);
    cpu.gpr.set32(4, st.esp);
    cpu.gpr.set32(5, st.ebp);
    cpu.gpr.set32(6, st.esi);
    cpu.gpr.set32(7, st.edi);
    cpu.rip = st.eip;
    cpu.flags.rflags = st.eflags;
    cpu.flag_op = cpu::CpuState::FlagOp::None;
    cpu.cf_overlay = 0;
    cpu.of_overlay = 0;
}

bool map_sentinel_page(u32 stub_addr) {
    const u32 page = stub_addr & ~0xFFFu;
    // Two pages, so a sentinel sitting within a stub's length of the page end
    // still has room for the whole stub.
    constexpr usize kSpan = 0x2000;
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(page)),
                   kSpan, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return false;
    // int3 fill: any stray control flow into this page traps rather than
    // running whatever happened to be here.
    std::memset(p, 0xCC, kSpan);
    Compat32Executor::emit_exit_stub(static_cast<u8*>(p) + (stub_addr - page),
                                     stub_addr);
    return true;
}

void dispatch_thunk(cpu::CpuState& cpu,
                    GuestMemory& memory,
                    winapi::ThunkRegistry& thunk_registry,
                    winapi::Marshaler& marshaler,
                    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard,
                    const FastThunkAddrs& fast_thunks) {
    if (maybe_fast_thunk_call(cpu, memory, fast_thunks)) {
        (void)finish_thunk_ret(cpu, memory, thunk_registry,
                               static_cast<GuestAddr>(cpu.rip));
        return;
    }

    maybe_clobber_x86_thunk_callee_stack(cpu, memory);

    auto handler_result = thunk_registry.resolve_handler(
        static_cast<GuestAddr>(cpu.rip));
    if (handler_result.is_err()) {
        std::fprintf(stderr,
                     "[native32] unresolved thunk at 0x%08x\n",
                     static_cast<u32>(cpu.rip));
        return;
    }

    GuestAddr fs_base = cpu.seg.fs_desc.base;
    if (fs_base != 0) {
        auto teb_err = memory.read_u32(fs_base + 0x34);
        if (teb_err.is_ok()) {
            winapi::set_last_error(teb_err.value());
        }
    }

    const u64 saved_edi = cpu.gpr.rdi;
    const u64 saved_esi = cpu.gpr.rsi;
    const u64 saved_ebx = cpu.gpr.rbx;
    const u64 saved_ebp = cpu.gpr.rbp;
    const GuestAddr dispatched = static_cast<GuestAddr>(cpu.rip);

    // Record into the hostcall ring, as the translated run loop does. Without
    // this a native32 run answers VITREO_DUMP_ON_EXIT with an empty ring, so a
    // guest that exits cleanly leaves no account of the API calls that led
    // there. Costs nothing unless the ring is enabled.
    const bool capture_hostcall = cli::hostcall_capture_enabled();
    cli::HostcallEntry hc{};
    if (capture_hostcall) {
        // No ModuleManager is threaded into the native32 dispatch path; the
        // ring documents a null `modules` as supported (caller module/RVA are
        // simply left unresolved).
        cli::hostcall_fill_pre(hc, cpu, memory, thunk_registry, nullptr);
    }

    // Whole-program call stream: capture the arguments BEFORE the handler
    // runs (it may overwrite the stack it was passed) and pair them with the
    // result afterwards. Generic -- the thunk address alone yields the name
    // and the argument size, so every export is covered without a schema.
    const bool capture_callstream = diagnostics::callstream_enabled();
    std::string cs_name;
    u64 cs_args[12] = {};
    u32 cs_argc = 0;
    u64 cs_caller = 0;
    if (capture_callstream) {
        cs_name = thunk_registry.resolve_name(dispatched);
        const u16 arg_bytes = thunk_registry.resolve_stdcall_bytes(dispatched);
        cs_argc = static_cast<u32>(arg_bytes / 4u);
        if (cs_argc > 12) cs_argc = 12;
        const GuestAddr sp = static_cast<GuestAddr>(cpu.gpr.rsp);
        cs_caller = memory.read_u32(sp).value_or(0);   // guest return address
        for (u32 i = 0; i < cs_argc; ++i) {
            cs_args[i] = memory.read_u32(sp + 4 + i * 4).value_or(0);
        }
    }

    suspend_guard.enter_host_call();
    {
        // The host call's own autorelease pool (dispatch_slow_api_thunk).
        graphics::AutoreleaseScope host_call_pool;
        // ...and the host FP environment: native code runs under the
        // platform default, the guest's mode is restored after it.
        math::HostFpEnvironmentScope host_call_fp(cpu.fpu.control_word, cpu.sse.mxcsr);
        (*handler_result.value())(marshaler);
    }
    suspend_guard.leave_host_call();

    if (capture_callstream) {
        diagnostics::callstream_record(
            cs_name, winapi::ThreadManager::get_current_thread_id_static(),
            cs_args, cs_argc, cpu.gpr.rax, memory, cpu, cs_caller);
    }

    if (capture_hostcall) {
        cli::hostcall_fill_post(hc, cpu, memory);
        cli::hostcall_record(hc);
    }

    if (fs_base != 0) {
        (void)memory.write_u32(fs_base + 0x34, winapi::get_last_error());
    }

    if (cpu.pending_unwind.active || cpu.thunk_control_transfer) {
        cpu.thunk_control_transfer = false;
        return;
    }

    cpu.gpr.rdi = saved_edi;
    cpu.gpr.rsi = saved_esi;
    cpu.gpr.rbx = saved_ebx;
    cpu.gpr.rbp = saved_ebp;

    if (cpu.pending_raise.active) {
        return;   // SEH path simulates RET if needed
    }
    (void)finish_thunk_ret(cpu, memory, thunk_registry, dispatched);
}

/// Walk the 32-bit SEH chain for a pending_raise. Returns true if a handler
/// resumed the guest (caller should re-enter native execution).
bool dispatch_pending_raise32(cpu::CpuState& cpu,
                              GuestMemory& memory,
                              winapi::ThunkRegistry& thunk_registry,
                              winapi::ProcessManager& process_mgr) {
    const bool from_helper = cpu.pending_raise.from_helper;
    if (!from_helper) {
        auto thunk_addr = cpu.rip;
        auto ret = memory.read_u32(cpu.sp());
        if (ret.is_ok()) {
            cpu.rip = ret.value();
            u16 cleanup = thunk_registry.resolve_stdcall_bytes(
                static_cast<GuestAddr>(thunk_addr));
            cpu.set_sp(cpu.sp() + 4 + cleanup);
        }
    }

    const u32 raise_code = cpu.pending_raise.code;
    const u32 raise_flags = cpu.pending_raise.flags;
    u32 raise_nargs = cpu.pending_raise.num_args;
    u32 raise_exception_addr = static_cast<u32>(cpu.rip);
    u32 raise_args[15] = {};
    for (u32 i = 0; i < raise_nargs && i < 15; ++i) {
        raise_args[i] = static_cast<u32>(cpu.pending_raise.args[i]);
    }
    cpu.pending_raise.clear();
    // A #BP trap leaves EIP past the instruction; Windows reports the
    // breakpoint address (end - 1) as ExceptionAddress and as the handlers'
    // EIP, with one parameter, 0 (int3_record_probe.x86.expected).
    if (from_helper && raise_code == cpu::STATUS_BREAKPOINT &&
        raise_exception_addr > 0) {
        --raise_exception_addr;
        cpu.rip = raise_exception_addr;
        if (raise_nargs == 0) {
            raise_nargs = 1;
            raise_args[0] = 0;
        }
    }

    // First-chance VEH, before the fs:[0] walk and independent of it.
    if (invoke_vectored_handlers_32(cpu, memory, thunk_registry, process_mgr,
                                    raise_code, raise_flags, raise_nargs,
                                    raise_args, raise_exception_addr)) {
        return true;
    }

    auto chain_head = cpu::read_seh_chain_head(memory, cpu);
    if (chain_head.is_err() || chain_head.value() == 0xFFFFFFFFu ||
        chain_head.value() == 0) {
        return false;
    }

    const u32 orig_esp = static_cast<u32>(cpu.gpr.rsp);
    u32 scratch_esp = orig_esp - (cpu::CTX32_SIZE + cpu::EREC32_SIZE + 16);
    scratch_esp &= ~0xFu;
    const u32 ctx_addr = scratch_esp;
    const u32 erec_addr = scratch_esp + cpu::CTX32_SIZE;
    const u32 disp_ctx_addr = erec_addr + cpu::EREC32_SIZE;

    (void)winapi::resolve_cxx_rethrow_32(memory, raise_code, raise_nargs,
                                         raise_args);
    cpu::write_exception_record_32(memory, erec_addr, raise_code, raise_flags,
                                   raise_exception_addr, raise_nargs,
                                   raise_args);
    cpu::write_context_32(memory, ctx_addr, cpu);
    (void)memory.write_u32(disp_ctx_addr, 0);

    u32 frame_addr = chain_head.value();
    u32 depth = 0;
    while (frame_addr != 0xFFFFFFFFu && frame_addr != 0 && depth < 256) {
        if (!cpu::seh32_registration_dispatchable(memory, cpu, frame_addr)) {
            break;
        }
        auto handler_info = cpu::read_seh_handler(memory, frame_addr);
        if (handler_info.is_err()) break;
        auto info = handler_info.value();
        if (!seh_handler_looks_valid(memory, info.handler_addr)) {
            frame_addr = info.next_frame;
            ++depth;
            continue;
        }

        if (raise_code == 0xE06D7363u) {
            u32 fi = cxx_ehhandler_uses_host_personality(
                                memory, thunk_registry, info.handler_addr)
                                ? detect_cxx_ehhandler_funcinfo(memory, info.handler_addr)
                                : 0;
            if (fi != 0) {
                auto r = winapi::dispatch_cxx_exception_32(
                    memory, fi, info.frame_addr, erec_addr, ctx_addr);
                if (r.match) {
                    if (enter_cxx_catch_32(cpu, memory, thunk_registry,
                                           process_mgr, r, info.frame_addr,
                                           ctx_addr, erec_addr, "native32")) {
                        return true;
                    }
                    break;
                }
                frame_addr = info.next_frame;
                ++depth;
                continue;
            }
        }

        u64 disposition = invoke_seh32_frame_handler(
            cpu, memory, thunk_registry, process_mgr,
            info.handler_addr, info.frame_addr,
            erec_addr, ctx_addr, disp_ctx_addr, scratch_esp);

        if (process_mgr.has_exited()) return true;
        if (apply_pending_unwind_32(cpu, memory, ctx_addr, "native32")) {
            return true;
        }
        if (disposition == cpu::ExceptionContinueExecution) {
            (void)cli::resume_from_context_32(cpu, memory, thunk_registry, process_mgr,
                                         erec_addr, ctx_addr, scratch_esp);
            return true;
        }
        frame_addr = info.next_frame;
        ++depth;
    }

    if (invoke_unhandled_exception_filter_32(
            cpu, memory, thunk_registry, process_mgr,
            erec_addr, ctx_addr, scratch_esp)) {
        return true;
    }
    return false;
}

enum class NativeStop : u8 {
    Sentinel, Abandoned, ProcessExit, Unhandled, SetupFailed
};

NativeStop run_until(cpu::CpuState& cpu,
                     GuestMemory& memory,
                     winapi::ThunkRegistry& thunk_registry,
                     winapi::ProcessManager& process_mgr,
                     u32 sentinel,
                     u32 seh_abandon_ctx_addr,
                     u32 seh_abandon_esp_min,
                     u64 func_addr,
                     EventPumpCallback event_pump,
                     u64* thunks_dispatched) {
    auto got = Compat32Executor::for_current_thread();
    if (got.is_err()) {
        std::fprintf(stderr, "[native32] no trampoline for this thread\n");
        return NativeStop::SetupFailed;
    }
    Compat32Executor* exec = got.value();

    thread_local bool ports_installed = false;
    if (!ports_installed) {
        (void)X64ExceptionBridge::install_on_current_thread();
        ports_installed = true;
    }

    winapi::Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    FastThunkAddrs fast_thunks;
    resolve_fast_thunks(fast_thunks, thunk_registry);

    auto suspend_guard = winapi::ThreadSuspendRegistry::instance().publish(
        winapi::ThreadManager::get_current_thread_id_static(), &cpu);

    u64 pumps = 0;
    for (;;) {
        suspend_guard.checkpoint();
        if (suspend_guard.terminate_requested()) {
            cpu.rip = sentinel;
            return NativeStop::Sentinel;
        }
        if (process_mgr.has_exited()) return NativeStop::ProcessExit;
        if (cpu.rip == sentinel) return NativeStop::Sentinel;

        if (seh_abandon_ctx_addr != 0 && seh_abandon_esp_min != 0 &&
            cpu.sp() > seh_abandon_esp_min &&
            (cpu.rip & ~0xFFFull) != (func_addr & ~0xFFFull) &&
            cpu.rip != sentinel && cpu.rip != 0) {
            mark_seh32_handler_left_dispatcher();
            return NativeStop::Abandoned;
        }

        const u64 teb = cpu.seg.fs_desc.base;
        if (teb == 0 || teb >= cpu::x64::kSegmentBaseLimit) {
            std::fprintf(stderr,
                         "[native32] thread has no 32-bit TEB (base=0x%llx)\n",
                         static_cast<unsigned long long>(teb));
            return NativeStop::SetupFailed;
        }
        const u16 fs = fs_selector_for(teb);
        if (fs == 0) {
            std::fprintf(stderr, "[native32] FS selector for TEB 0x%llx failed\n",
                         static_cast<unsigned long long>(teb));
            return NativeStop::SetupFailed;
        }
        exec->set_fs_selector(fs);
        if (memory.write_u32(static_cast<GuestAddr>(teb) +
                                 cpu::x64::kTebNative32Collect32,
                             exec->collect32_address()).is_err() ||
            memory.write_u32(static_cast<GuestAddr>(teb) +
                                 cpu::x64::kTebNative32Sentinel,
                             exec->sentinel_return_address()).is_err()) {
            std::fprintf(stderr,
                         "[native32] TEB FS-hook write failed (teb=0x%llx)\n",
                         static_cast<unsigned long long>(teb));
            return NativeStop::SetupFailed;
        }

        auto& prof = Native32Profile::current();
        const auto prof_t0 = prof.enabled ? Native32Profile::Clock::now()
                                          : Native32Profile::Clock::time_point{};

        Compat32State st{};
        pack(cpu, st);
        fx_to_guest(cpu, *exec);
        const Compat32Exit why = exec->run(st);
        unpack(st, cpu);
        fx_from_guest(cpu, *exec);

        if (prof.enabled) {
            prof.guest_ns += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Native32Profile::Clock::now() - prof_t0).count());
            prof.maybe_dump(thunk_registry);
        }

        if (why == Compat32Exit::SetupFailed) return NativeStop::SetupFailed;

        if (why == Compat32Exit::ReturnedToSentinel ||
            cpu.rip == sentinel ||
            cpu.rip == exec->sentinel_return_address()) {
            cpu.rip = sentinel;
            return NativeStop::Sentinel;
        }

        if (why == Compat32Exit::ThunkCall) {
            if (thunks_dispatched) ++*thunks_dispatched;
            const u32 thunk_site = static_cast<u32>(cpu.rip);
            const auto thunk_t0 = prof.enabled ? Native32Profile::Clock::now()
                                               : Native32Profile::Clock::time_point{};
            dispatch_thunk(cpu, memory, thunk_registry, marshaler,
                           suspend_guard, fast_thunks);
            if (prof.enabled) {
                const u64 d = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Native32Profile::Clock::now() - thunk_t0).count());
                prof.thunk_ns += d;
                ++prof.thunk_calls;
                auto& site = prof.sites[thunk_site];
                ++site.calls;
                site.ns += d;
            }
            if (cpu.pending_raise.active) {
                if (!dispatch_pending_raise32(cpu, memory, thunk_registry,
                                              process_mgr)) {
                    return NativeStop::Unhandled;
                }
            }
            if (event_pump && (++pumps & 0x3FFu) == 0) {
                event_pump();
            }
            continue;
        }

        if (why == Compat32Exit::Fault) {
            const auto fault_t0 = prof.enabled ? Native32Profile::Clock::now()
                                               : Native32Profile::Clock::time_point{};
            struct FaultTimer {
                Native32Profile& p; Native32Profile::Clock::time_point t0;
                ~FaultTimer() {
                    if (!p.enabled) return;
                    p.fault_ns += static_cast<u64>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Native32Profile::Clock::now() - t0).count());
                    ++p.fault_calls;
                }
            } fault_timer{prof, fault_t0};
            cpu.pending_raise.active = true;
            cpu.pending_raise.from_helper = true;
            cpu.pending_raise.code = exec->last_fault_status();
            cpu.pending_raise.flags = 0;
            cpu.pending_raise.num_args = 2;
            cpu.pending_raise.args[0] = exec->last_fault_write();
            cpu.pending_raise.args[1] = exec->last_fault_address();
            if (!dispatch_pending_raise32(cpu, memory, thunk_registry,
                                          process_mgr)) {
                return NativeStop::Unhandled;
            }
            continue;
        }
    }
}

} // namespace

bool native32_wanted() noexcept {
    return environment_flag_enabled("VITREO_NO_NATIVE32", false) == false;
}

bool native32_is_prepared() noexcept { return g_prepared; }

VoidResult native32_prepare(GuestMemory& memory,
                            winapi::ThunkRegistry& thunk_registry) {
    if (g_prepared) return VoidResult::ok();

    // Guest stores from here on execute on the CPU, not through the
    // interpreter or JIT, so nothing marks the write-watch dirty map in
    // software. Switch MEM_WRITE_WATCH to hardware tracking: clean committed
    // pages are armed PROT_READ and the protection fault marks them. The host
    // page is 4 KiB here, the same as the guest page, so the arming is exact.
    //
    // The conservative superset this replaces was not a safe fallback -- with
    // WRITE_WATCH_FLAG_RESET it never shrinks, so Boehm's
    // `do { ... } while (count == capacity)` drain loop never terminates.
    if (environment_flag_enabled("VITREO_WRITE_WATCH_TRAPS", true)) {
        write_watch::enable_hardware_traps(
            reinterpret_cast<uintptr_t>(memory.linear_host(0)));
        memory::install_write_watch_signal_handler();
    } else {
        write_watch::set_stores_untracked(true);
    }

    auto bridge = X64ExceptionBridge::start();
    if (bridge.is_err()) {
        std::fprintf(stderr, "[native32] exception bridge failed to start\n");
        return VoidResult::err(bridge.error());
    }
    (void)X64ExceptionBridge::install_on_current_thread();

    if (!Compat32Executor::reserve_trampoline_pool()) {
        std::fprintf(stderr, "[native32] trampoline pool reserve failed\n");
        return VoidResult::err(Error::OutOfMemory);
    }
    const u32 pool_base = Compat32Executor::trampoline_pool_base();
    const usize pool_bytes = Compat32Executor::trampoline_pool_bytes();
    memory.add_reserved_hole(pool_base,
                             pool_base + static_cast<GuestAddr>(pool_bytes));

    const u32 proc_sentinel = static_cast<u32>(kProcessReturnSentinel);
    const u32 cb_sentinel = static_cast<u32>(kCallbackReturnSentinel);
    if (!map_sentinel_page(proc_sentinel) || !map_sentinel_page(cb_sentinel)) {
        std::fprintf(stderr, "[native32] sentinel page map failed\n");
        return VoidResult::err(Error::OutOfMemory);
    }

    // Rewrite CPUID in guest code so a natively-executing guest sees Vitreo's
    // CPU rather than this Mac's. Bounded to executable extents: the same byte
    // pair in data must not be touched. See core/cpu/x64/cpuid_intercept.hpp.
    {
        usize sites = 0;
        for (const auto& [lo, hi] : memory.executable_extents()) {
            if (lo >= 0x100000000ull || hi <= lo) continue;
            sites += cpu::x64::intercept_cpuid_in_range(
                memory, static_cast<u32>(lo), static_cast<u32>(hi - lo));
        }
        if (sites != 0) {
            std::fprintf(stderr, "[native32] CPUID intercepted at %zu site(s)\n",
                         sites);
        }
    }

    const GuestAddr thunk_base = thunk_registry.thunk_base();
    if (thunk_base != 0) {
        (void)memory.protect_region(thunk_base, kThunkRegionMapSize,
                                    MemoryProtection::ReadWrite);
        u8* host = memory.linear_host(thunk_base);
        if (host == nullptr) {
            std::fprintf(stderr, "[native32] thunk region has no host mapping\n");
            return VoidResult::err(Error::OutOfMemory);
        }
        for (u32 off = 0; off < kThunkRegionMapSize; off += 16) {
            Compat32Executor::emit_thunk_stub(
                host + off, static_cast<u32>(thunk_base + off));
        }
        (void)memory.protect_region(thunk_base, kThunkRegionMapSize,
                                    MemoryProtection::ReadExecute);
    }

    // The accelerators are installed once in run.cpp for both execution paths,
    // but native32's stub emission above rewrites EVERY slot -- including the
    // patched ones -- so they have to be re-applied here. install_* is
    // idempotent by design; the second call is the one that sticks on this path.
    (void)winapi::install_guest_crt_accelerators(memory, thunk_registry);

    g_prepared = true;
    std::fprintf(stderr,
                 "[native32] ready (per-thread trampolines, pool=0x%08x+%zu)\n",
                 pool_base, pool_bytes);
    return VoidResult::ok();
}

RunResult run_native32_loop(cpu::CpuState& cpu,
                            GuestMemory& memory,
                            winapi::ThunkRegistry& thunk_registry,
                            winapi::ProcessManager& process_mgr,
                            const RunConfig& config,
                            EventPumpCallback event_pump) {
    RunResult result{};
    const u32 sentinel = static_cast<u32>(kProcessReturnSentinel);
    u64 thunks = 0;
    const NativeStop stop = run_until(
        cpu, memory, thunk_registry, process_mgr, sentinel,
        0, 0, 0, event_pump, &thunks);
    result.instructions_executed = thunks;

    if (process_mgr.has_exited()) {
        result.exit_code = static_cast<int>(process_mgr.exit_code());
        result.success = true;
        return result;
    }
    if (stop == NativeStop::Sentinel) {
        result.exit_code = static_cast<int>(cpu.gpr.get32(0));
        result.success = true;
        return result;
    }
    result.exit_code = 1;
    result.success = false;
    result.error_message = (stop == NativeStop::SetupFailed)
                               ? "native32 setup failed"
                               : "unhandled guest exception";
    (void)config;
    return result;
}

bool native32_run_until_sentinel(cpu::CpuState& cpu,
                                 GuestMemory& memory,
                                 winapi::ThunkRegistry& thunk_registry,
                                 winapi::ProcessManager& process_mgr,
                                 u32 sentinel,
                                 u32 seh_abandon_ctx_addr,
                                 u32 seh_abandon_esp_min,
                                 u64 func_addr,
                                 u64* result_eax,
                                 bool* completed_normally) {
    if (!native32_is_prepared()) return false;
    const NativeStop stop = run_until(
        cpu, memory, thunk_registry, process_mgr, sentinel,
        seh_abandon_ctx_addr, seh_abandon_esp_min, func_addr,
        nullptr, nullptr);
    if (stop == NativeStop::SetupFailed) {
        static thread_local bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[native32] no trampoline for this thread; "
                         "interpreting guest code instead\n");
        }
        return false;
    }
    if (result_eax) {
        *result_eax = (stop == NativeStop::Abandoned)
                          ? cpu::ExceptionContinueExecution
                          : cpu.gpr.get32(0);
    }
    if (completed_normally) {
        *completed_normally = (stop == NativeStop::Sentinel ||
                               stop == NativeStop::Abandoned);
    }
    return true;
}

} // namespace vitreo::cli

#endif // VITREO_ARCH_X86_64
