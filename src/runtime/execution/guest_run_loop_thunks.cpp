// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: slow-path API thunk dispatch (thunk sled skip, full
// host-handler dispatch) and the native-backend seams. Split out of
// guest_run_loop.cpp as verbatim code motion.

#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/guest_run_loop_internal.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/common/autorelease_scope.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include "runtime/diagnostics/hostcall_ring.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include "runtime/execution/exception_dispatch.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include "runtime/execution/thunk_direct.hpp"
#include "core/math/host_fenv.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>

namespace vitreo::cli {

// A thunk entry stub that guest_accel.cpp (or a guest hook engine) patched
// with `jmp rel32` / `jmp rel8` is ordinary guest code, but no block is ever
// published at a thunk address, so reaching it costs a dispatcher round trip
// and an interpreted JMP on every call. Executing the JMP here is the same
// architectural effect (Intel SDM Vol. 2A, JMP: the target is the next
// instruction's address plus the sign-extended displacement, truncated to 32
// bits outside long mode; no flags change). The live bytes are read every
// time, so a later re-patch is observed. With TF set the interpreter must run
// it so the single-step trap is raised after it.
bool follow_thunk_sled_jump(cpu::CpuState& cpu, GuestMemory& memory,
                            const winapi::ThunkRegistry& thunk_registry) {
    if (cpu.flags.tf() || thunk_registry.stub_dispatch_target(cpu.rip) == 0) {
        return false;
    }
    u8 bytes[5] = {};
    const u8* const live = memory.linear_backs(cpu.rip, sizeof(bytes))
        ? memory.linear_host(cpu.rip)
        : memory.host_ptr(cpu.rip, sizeof(bytes));
    if (live == nullptr) {
        return false;
    }
    std::memcpy(bytes, live, sizeof(bytes));
    u64 next = 0;
    i64 displacement = 0;
    if (bytes[0] == 0xE9u) {
        i32 rel32 = 0;
        std::memcpy(&rel32, bytes + 1, sizeof(rel32));
        next = cpu.rip + 5;
        displacement = rel32;
    } else if (bytes[0] == 0xEBu) {
        next = cpu.rip + 2;
        displacement = static_cast<i8>(bytes[1]);
    } else {
        return false;
    }
    u64 target = next + static_cast<u64>(displacement);
    if (!cpu.is_64bit()) {
        target &= 0xFFFFFFFFull;
    }
    cpu.rip = target;
    return true;
}

/// Advance RIP past an unmodified thunk entry NOP sled to the dispatch
/// gateway. Same contract as the outer-loop skip: patched sleds are left alone.
void maybe_skip_thunk_nop_sled(cpu::CpuState& cpu, GuestMemory& memory,
                               const winapi::ThunkRegistry& thunk_registry) {
    if (follow_thunk_sled_jump(cpu, memory, thunk_registry)) {
        return;
    }
    GuestAddr dispatch = thunk_registry.stub_dispatch_target(cpu.rip);
    if (dispatch == 0) {
        return;
    }
    // Inspect the whole fixed-size entry with one checked lookup. Resolving
    // every byte separately in scattered memory made hook detection more
    // expensive than many of the APIs behind the thunk. We still inspect the
    // live guest bytes on every call, so a guest-installed hook remains
    // observable.
    if (dispatch - cpu.rip == winapi::ThunkRegistry::STUB_SIZE) {
        static_assert(winapi::ThunkRegistry::STUB_SIZE == sizeof(u64));
        // linear_backs, not is_linear: a thunk inside a module loaded ABOVE
        // the reservation (Windows puts high system DLLs just under the
        // 128 TiB ceiling) is a real guest address that linear_host() would
        // turn into a wild host pointer. Measured as a SIGSEGV here on the
        // first browser helper the moment the VA cap was removed.
        const u8* const stub =
            memory.linear_backs(cpu.rip, winapi::ThunkRegistry::STUB_SIZE)
            ? memory.linear_host(cpu.rip)
            : memory.host_ptr(cpu.rip, winapi::ThunkRegistry::STUB_SIZE);
        if (stub != nullptr) {
            u64 live_stub = 0;
            std::memcpy(&live_stub, stub, sizeof(live_stub));
            constexpr u64 kUnmodifiedNopSled = 0x9090909090909090ULL;
            if (live_stub == kUnmodifiedNopSled) {
                cpu.rip = dispatch;
                return;
            }
            // Native32 overwrites the entry with its per-thread trampoline,
            // while guest accelerators may replace it with an ordinary rel32
            // call/jump. One live 64-bit load classifies the common entry case
            // without eight byte-at-a-time branches; patched bytes still
            // remain authoritative.
            const u8 first = static_cast<u8>(live_stub);
            if (first == 0x64u || first == 0xE8u) {
                cpu.rip = dispatch;
            }
            return;
        }
        // Preserve the bytewise path below for a stub split across adjacent
        // mappings. Published thunk slabs normally resolve contiguously.
    }

    const auto read_stub_byte = [&](GuestAddr addr, u8& byte) -> bool {
        if (memory.linear_backs(addr, 1)) {
            byte = *memory.linear_host(addr);
            return true;
        }
        const auto result = memory.read_u8(addr);
        if (result.is_err()) return false;
        byte = result.value();
        return true;
    };
    u8 first = 0;
    if (!read_stub_byte(cpu.rip, first)) {
        return;
    }
    if (first == 0x90u) {
        for (GuestAddr a = cpu.rip + 1; a < dispatch; ++a) {
            u8 byte = 0;
            if (!read_stub_byte(a, byte) || byte != 0x90u) {
                return;
            }
        }
        cpu.rip = dispatch;
        return;
    }
    // Native32 overwrites the 8-byte entry with
    // `call dword ptr fs:[teb+collect]` (64 FF 15 disp32) so every thread can
    // leave 32-bit mode through its own trampoline. Interpreter workers must
    // not emulate that trampoline; skip to the untouched dispatch gateway.
    // The previous encoding was `call rel32` (E8); keep skipping that too.
    if ((first == 0x64u || first == 0xE8u) &&
        (dispatch - cpu.rip) == winapi::ThunkRegistry::STUB_SIZE) {
        cpu.rip = dispatch;
    }
}

/// Full host thunk dispatch shared by the outer loop and JIT-chain inline.
/// When `safepoint` is non-null, the chain's arena lease is parked around
/// `(*handler)(marshaler)` — the host call is a safepoint — and taken back
/// afterwards; the caller reads `safepoint->resume()` to learn whether the
/// chain may continue into compiled code.
[[nodiscard]] SlowThunkDispatch dispatch_slow_api_thunk(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    const winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard,
    const FastThunkAddrs& fast_thunks,
    winapi::ModuleManager* mods,
    cpu::jit::JitCodeArena::HostCallSafepoint* safepoint,
    bool slow_hostcall_diag,
    u64 slow_hostcall_threshold_ms,
    bool direct_checked) {
    // An export with a direct entry (the D3D11/DXGI COM method tables) is one
    // plain call: no resolve_handler, no std::function layers, none of the
    // diagnostic statics below, no TEB last-error sync (Windows leaves it
    // alone across these methods; thunk_direct.hpp). Every exit it takes is
    // one this function would have taken. @p direct_checked: the caller (the
    // chain) has already asked, and the answer was the full path.
    if (const winapi::DirectThunk* direct =
            direct_checked ? nullptr
                           : direct_thunk_for(thunk_registry, cpu.rip,
                                              slow_hostcall_diag)) {
        switch (run_direct_thunk(*direct, cpu, memory, marshaler,
                                 suspend_guard, safepoint)) {
        case DirectThunkResult::Returned:
        case DirectThunkResult::Raising:  // pending_raise: callers check it
            return SlowThunkDispatch::Ok;
        case DirectThunkResult::ControlTransferred:
        case DirectThunkResult::Unwinding:  // pending_unwind: callers check
            return SlowThunkDispatch::ControlTransferred;
        case DirectThunkResult::Fatal:
            return SlowThunkDispatch::Fatal;
        }
    }
    const bool lean_hot_handler =
        is_lean_hot_thunk(cpu.rip, fast_thunks) &&
        !hot_hostcall_diagnostics_enabled() &&
        // The lean path skips the bookkeeping the recorder reads from.
        (!diagnostics::callstream_enabled() ||
         diagnostics::callstream_keep_fastpath());
    if (!lean_hot_handler) {
        maybe_clobber_x86_thunk_callee_stack(cpu, memory);
    }

    static const bool api_trace = (std::getenv("VITREO_API_TRACE") != nullptr);
    if (api_trace) {
        std::string api_name = thunk_registry.resolve_name(cpu.rip);
        u32 tid = winapi::ThreadManager::get_current_thread_id_static();
        VITREO_TRACE("[api T%u] %s\n", tid, api_name.c_str());
    }

    const GuestAddr dispatched_thunk_addr = cpu.rip;
    // Address compare, not resolve_name(): resolve_name returns a std::string
    // BY VALUE, so the old spelling heap-allocated on every slow dispatch just
    // to compare it against a literal.
    const bool is_guard_dispatch =
        cpu.is_64bit() && dispatched_thunk_addr != 0 &&
        dispatched_thunk_addr == thunk_registry.guard_dispatch_icall_address();
    const u64 guard_dispatch_target = is_guard_dispatch ? cpu.gpr.rax : 0;

    auto handler_result = thunk_registry.resolve_handler(cpu.rip);
    if (handler_result.is_err()) {
        return SlowThunkDispatch::Unresolved;
    }
    const auto* handler = handler_result.value();

    const bool capture_hostcall =
        !lean_hot_handler && hostcall_capture_enabled();
    HostcallEntry hc{};
    if (capture_hostcall) {
        hostcall_fill_pre(hc, cpu, memory, thunk_registry, mods);
    }

    {
        static const bool s_trace_bad_caller =
            (std::getenv("VITREO_TRACE_BAD_CALLER") != nullptr);
        const bool is_stack_ret =
            hc.ret_addr != 0 &&
            (hc.ret_addr >= 0x7ff00000ULL && hc.ret_addr < 0x800000000ULL);
        if (s_trace_bad_caller && cpu.is_64bit() &&
            (is_stack_ret || (hc.ret_addr != 0 && hc.caller_mod[0] == 0))) {
            std::fprintf(stderr,
                "[bad-caller] thunk=%s rip=0x%llx rsp=0x%llx "
                "ret=0x%llx (unmapped) rcx=0x%llx rdx=0x%llx "
                "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx\n",
                hc.name,
                (unsigned long long)cpu.rip,
                (unsigned long long)cpu.gpr.rsp,
                (unsigned long long)hc.ret_addr,
                (unsigned long long)cpu.gpr.rcx,
                (unsigned long long)cpu.gpr.rdx,
                (unsigned long long)cpu.gpr.r8,
                (unsigned long long)cpu.gpr.r9,
                (unsigned long long)cpu.gpr.r10,
                (unsigned long long)cpu.gpr.r11);
            std::fflush(stderr);
        }
    }

    const u64 saved_edi = cpu.gpr.rdi;
    const u64 saved_esi = cpu.gpr.rsi;
    const u64 saved_ebx = cpu.gpr.rbx;
    const u64 saved_ebp = cpu.gpr.rbp;
    const u64 saved_r12 = cpu.gpr.r12;
    const u64 saved_r13 = cpu.gpr.r13;
    const u64 saved_r14 = cpu.gpr.r14;
    const u64 saved_r15 = cpu.gpr.r15;
    std::array<cpu::XmmReg, 10> saved_xmm_nonvolatile{};
    const u32 saved_mxcsr_control = cpu.sse.mxcsr & 0xffc0u;
    if (cpu.is_64bit()) {
        std::copy_n(cpu.sse.xmm + 6, saved_xmm_nonvolatile.size(),
                    saved_xmm_nonvolatile.begin());
    }

    // TEB.LastErrorValue (x86 fs:[0x34], x64 gs:[0x68]) is the thread's last
    // error on Windows; guest code reads and writes it directly. A managed
    // runtime reads gs:[0x68] after a call that sets last-error instead of
    // calling GetLastError. Sync it into the host copy before the handler
    // and back out afterwards, for both bitnesses.
    {
        const GuestAddr teb_base =
            cpu.is_64bit() ? cpu.seg.gs_desc.base : cpu.seg.fs_desc.base;
        const GuestAddr le_off = cpu.is_64bit() ? 0x68 : 0x34;
        auto teb_err = memory.read_u32(teb_base + le_off);
        if (teb_err.is_ok()) {
            winapi::set_last_error(teb_err.value());
        }
    }

    const std::string slow_hostcall_name = slow_hostcall_diag
        ? thunk_registry.resolve_name(dispatched_thunk_addr)
        : std::string{};
    // Timing is diagnostic-only. Calling mach_continuous_time through
    // steady_clock for every ordinary thunk was measurable in byte-at-a-time
    // CRT and COM traffic even though the result was never observed.
    std::chrono::steady_clock::time_point slow_hostcall_started{};
    if (slow_hostcall_diag) {
        slow_hostcall_started = std::chrono::steady_clock::now();
    }

    // Host handlers may block, pump messages or re-enter the guest. Outer
    // thunk dispatch never holds an arena lease; a chain does — park it for
    // the host call (JitCodeArena::HostCallSafepoint).
    if (safepoint != nullptr) {
        safepoint->park();
    }
    // Whole-program call stream, same format the native32 path emits, so a
    // JIT run and an Intel run can be compared. Read arguments before dispatch.
    const bool capture_callstream = diagnostics::callstream_enabled();
    std::string cs_name;
    u64 cs_args[12] = {};
    u32 cs_argc = 0;
    u64 cs_caller = 0;
    if (capture_callstream) {
        cs_name = thunk_registry.resolve_name(dispatched_thunk_addr);
        const GuestAddr sp = static_cast<GuestAddr>(cpu.gpr.rsp);
        if (cpu.is_64bit()) {
            // Win64: the first four integer arguments are in RCX/RDX/R8/R9 and
            // there is no stdcall byte count to say how many there are, so
            // record those four. They carry the identifying arguments of
            // nearly every API -- a handle, a pointer, a flag -- which is what
            // a first-divergence diff needs.
            cs_caller = memory.read_u64(sp).value_or(0);
            cs_args[0] = cpu.gpr.get64(1);   // RCX
            cs_args[1] = cpu.gpr.get64(2);   // RDX
            cs_args[2] = cpu.gpr.get64(8);   // R8
            cs_args[3] = cpu.gpr.get64(9);   // R9
            cs_argc = 4;
        } else {
            const u16 arg_bytes =
                thunk_registry.resolve_stdcall_bytes(dispatched_thunk_addr);
            cs_argc = static_cast<u32>(arg_bytes / 4u);
            if (cs_argc > 12) cs_argc = 12;
            cs_caller = memory.read_u32(sp).value_or(0);  // guest return addr
            for (u32 i = 0; i < cs_argc; ++i) {
                cs_args[i] = memory.read_u32(sp + 4 + i * 4).value_or(0);
            }
        }
    }

    // Every host call drains its own autorelease pool. A guest thread is a
    // host thread Cocoa did not create, so nothing else ever does (Apple,
    // "Using Autorelease Pool Blocks"); whatever Metal, AppKit or Foundation
    // autoreleases during the call is released here, before the guest's next
    // instruction. The pool lives in this C++ frame and every way a handler
    // leaves is a return to it (guest control transfers -- longjmp, RtlUnwind,
    // RaiseException, ExitThread, SwitchToFiber -- are CPU-state edits the
    // dispatcher acts on afterwards), and a guest callback or APC the handler
    // runs is a nested run loop whose crossings push and pop their own pools
    // inside this one, so pushes and pops stay LIFO.
    suspend_guard.enter_host_call();
    {
        graphics::AutoreleaseScope host_call_pool;
        // ...and the host FP environment: native code runs under the
        // platform default, the guest's mode is restored after it.
        math::HostFpEnvironmentScope host_call_fp(cpu.fpu.control_word, cpu.sse.mxcsr);
        (*handler)(marshaler);
    }
    suspend_guard.leave_host_call();

    // VITREO_ABI_CHECK=1 — report any handler that returns with a Win64
    // nonvolatile register changed. The restore below hides an ordinary
    // violation, but NOT the control-transfer path, which deliberately keeps
    // whatever the handler left; a clobber there reaches the guest, and a
    // guest that resumes with a foreign RBP does `call *%rbp` into nowhere.
    // Off by default: one comparison per hostcall is not free.
    {
        static const bool s_abi_check =
            (std::getenv("VITREO_ABI_CHECK") != nullptr);
        if (s_abi_check && cpu.is_64bit()) {
            const struct { const char* name; u64 before, after; } regs[] = {
                {"rbx", saved_ebx, cpu.gpr.rbx},
                {"rbp", saved_ebp, cpu.gpr.rbp},
                {"rsi", saved_esi, cpu.gpr.rsi},
                {"rdi", saved_edi, cpu.gpr.rdi},
                {"r12", saved_r12, cpu.gpr.r12},
                {"r13", saved_r13, cpu.gpr.r13},
                {"r14", saved_r14, cpu.gpr.r14},
                {"r15", saved_r15, cpu.gpr.r15},
            };
            for (const auto& r : regs) {
                if (r.before == r.after) continue;
                std::fprintf(stderr,
                    "[abi] %s clobbered %s 0x%llx -> 0x%llx "
                    "(transfer=%d rip=0x%llx)\n",
                    thunk_registry.resolve_name(dispatched_thunk_addr).c_str(),
                    r.name,
                    (unsigned long long)r.before,
                    (unsigned long long)r.after,
                    cpu.thunk_control_transfer ? 1 : 0,
                    (unsigned long long)dispatched_thunk_addr);
            }
        }
    }

    if (capture_callstream) {
        diagnostics::callstream_record(
            cs_name, winapi::ThreadManager::get_current_thread_id_static(),
            cs_args, cs_argc, cpu.gpr.rax, marshaler.memory(), cpu, cs_caller);
    }

    // Back from the host call: take the lease back before any compiled
    // pointer is consulted again. The outcome is left in the safepoint.
    if (safepoint != nullptr) {
        (void)safepoint->unpark();
    }

    if (slow_hostcall_diag) {
        const auto elapsed_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                      slow_hostcall_started).count();
        if (elapsed_ms >= static_cast<i64>(slow_hostcall_threshold_ms)) {
            std::fprintf(stderr,
                         "[slow-hostcall] %s elapsed_ms=%lld\n",
                         slow_hostcall_name.c_str(),
                         static_cast<long long>(elapsed_ms));
        }
    }

    if (cpu.is_64bit()) {
        (void)memory.write_u32(cpu.seg.gs_desc.base + 0x68,
                               winapi::get_last_error());
    } else {
        GuestAddr fs_base = cpu.seg.fs_desc.base;
        u32 le = winapi::get_last_error();
        (void)memory.write_u32(fs_base + 0x34, le);
        static const bool teb_trace =
            std::getenv("VITREO_TEB_TRACE") != nullptr;
        if (teb_trace && le == 2) {
            auto verify = memory.read_u32(fs_base + 0x34);
            VITREO_TRACE("[teb-sync] fs_base=0x%llx wrote last_error=%u, readback=%u\n",
                         (unsigned long long)fs_base, le,
                         verify.is_ok() ? verify.value() : 0xDEADu);
        }
    }

    // RtlUnwind owns nonvolatiles — do not restore before the caller breaks.
    if (cpu.pending_unwind.active) {
        cpu.thunk_control_transfer = false;
        if (capture_hostcall) {
            hostcall_fill_post(hc, cpu, memory);
            hostcall_record(hc);
        }
        return SlowThunkDispatch::ControlTransferred;
    }

    const bool handler_transferred_control = cpu.thunk_control_transfer;
    cpu.thunk_control_transfer = false;

    if (!handler_transferred_control) {
        cpu.gpr.rdi = saved_edi;
        cpu.gpr.rsi = saved_esi;
        cpu.gpr.rbx = saved_ebx;
        cpu.gpr.rbp = saved_ebp;
        cpu.gpr.r12 = saved_r12;
        cpu.gpr.r13 = saved_r13;
        cpu.gpr.r14 = saved_r14;
        cpu.gpr.r15 = saved_r15;
        if (cpu.is_64bit()) {
            std::copy(saved_xmm_nonvolatile.begin(),
                      saved_xmm_nonvolatile.end(), cpu.sse.xmm + 6);
            // Callee-saved, except for the CRT functions whose documented
            // purpose is to change it (CpuState::thunk_sets_fp_control).
            if (!cpu.thunk_sets_fp_control) {
                cpu.sse.mxcsr = (cpu.sse.mxcsr & 0x3fu) | saved_mxcsr_control;
            }
        }
    }
    cpu.thunk_sets_fp_control = false;

    if (capture_hostcall) {
        hostcall_fill_post(hc, cpu, memory);
        hostcall_record(hc);
    }

    if (handler_transferred_control) {
        return SlowThunkDispatch::ControlTransferred;
    }

    if (is_guard_dispatch) {
        VITREO_TRACE("[guard-dispatch] target=0x%llx via thunk=0x%llx\n",
                     static_cast<unsigned long long>(guard_dispatch_target),
                     static_cast<unsigned long long>(dispatched_thunk_addr));
        cpu.rip = guard_dispatch_target;
        return SlowThunkDispatch::Ok;
    }

    // RaiseException leaves pending_raise with rip still at the thunk gateway.
    // The outer SEH path simulates RET before dispatching; do not RET here.
    if (cpu.pending_raise.active) {
        return SlowThunkDispatch::Ok;
    }

    if (!finish_thunk_ret(cpu, memory, thunk_registry, dispatched_thunk_addr)) {
        return SlowThunkDispatch::Fatal;
    }
    return SlowThunkDispatch::Ok;
}

bool dispatch_guest_fault_native(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 status,
    u64 fault_address,
    bool is_write) {
    // Dispatch cannot proceed without unwind tables, and a guest fault with
    // no way to find a handler must not be reported as handled.
    auto* mods = module_manager();
    if (mods == nullptr) return false;

    // EXCEPTION_RECORD for an access violation carries two parameters: the
    // access type and the address. Other statuses carry none.
    const u64 args[2] = {is_write ? 1ull : 0ull, fault_address};
    const u32 nargs = (status == 0xC0000005u) ? 2u : 0u;

    return dispatch_x64_exception(
        cpu, memory, thunk_registry, process_mgr,
        status, /*raise_flags=*/0, nargs, args, cpu.rip, mods);
}

// ---------------------------------------------------------------------------
// Native-backend seam
//
// The native backends reach the thunk boundary by hardware rather than by
// decoding, but everything downstream of it -- marshalling, the calling
// convention, last_error, the hostcall ring, the call stream -- must stay one
// implementation. This forwards to exactly the body the translated loop runs.
// ---------------------------------------------------------------------------
NativeThunkDispatch dispatch_api_thunk_native(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard) {
    FastThunkAddrs fast_thunks;
    resolve_fast_thunks(fast_thunks, thunk_registry);

    const auto slow = dispatch_slow_api_thunk(
        cpu, memory, thunk_registry, marshaler, suspend_guard, fast_thunks,
        module_manager(), /*chain_lease=*/nullptr,
        /*slow_hostcall_diag=*/false, /*slow_hostcall_threshold_ms=*/0);

    switch (slow) {
        case SlowThunkDispatch::Ok:                 return NativeThunkDispatch::Ok;
        case SlowThunkDispatch::ControlTransferred: return NativeThunkDispatch::ControlTransferred;
        case SlowThunkDispatch::Unresolved:         return NativeThunkDispatch::Unresolved;
        case SlowThunkDispatch::Fatal:              break;
    }
    return NativeThunkDispatch::Fatal;
}

} // namespace vitreo::cli
