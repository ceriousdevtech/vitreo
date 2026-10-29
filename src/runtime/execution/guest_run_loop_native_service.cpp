// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: SYSCALL / SYSENTER / INT 2E native-service dispatch.
// Split out of guest_run_loop.cpp as verbatim code motion.

#include "runtime/execution/guest_run_loop_internal.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "graphics/common/autorelease_scope.hpp"
#include "winapi/core/marshaler.hpp"
#include "runtime/diagnostics/callstream.hpp"
#include "core/math/host_fenv.hpp"
#include <array>
#include <limits>
#include <string>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <unordered_set>

namespace vitreo::cli {

namespace {

void report_service_past_table(u32 service_number) {
    static std::mutex mutex;
    static std::unordered_set<u32> reported;
    std::lock_guard lock(mutex);
    if (reported.size() < 64 && reported.insert(service_number).second) {
        std::fprintf(stderr,
                     "[vitreo] native service 0x%x is past ntdll's service "
                     "table -> STATUS_INVALID_SYSTEM_SERVICE\n",
                     service_number);
    }
}

NativeServiceDispatch dispatch_x64_native_service(
    cpu::CpuState& cpu,
    winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard) {
    const u32 service_number = cpu.gpr.get32(
        static_cast<u8>(cpu::RegIndex::RAX));
    const GuestAddr thunk =
        thunk_registry.native_service_thunk(service_number);
    if (thunk == 0) {
        // A number past the service table is not a fault: the kernel returns
        // STATUS_INVALID_SYSTEM_SERVICE in RAX (ntdll_service_layout_probe,
        // "service 0xfff"), and SYSCALL has already left the return RIP in
        // RCX and RFLAGS in R11. Vitreo's table is smaller than Windows', so
        // name the number once: it may be a service a guest hardcoded.
        report_service_past_table(service_number);
        cpu.gpr.rax = winapi::nt_status::STATUS_INVALID_SYSTEM_SERVICE;
        return NativeServiceDispatch::Dispatched;
    }
    const auto handler_result = thunk_registry.resolve_handler(thunk);
    if (handler_result.is_err()) {
        return NativeServiceDispatch::UnknownService;
    }

    // SYSCALL has already captured the user return RIP in RCX and RFLAGS in
    // R11. Windows' x64 native-service ABI moves argument zero to R10 before
    // entering the kernel, so expose R10 as RCX only while the ordinary
    // Vitreo API marshaler invokes the NT implementation.
    const u64 syscall_return_rip = cpu.gpr.rcx;
    const u64 saved_rdi = cpu.gpr.rdi;
    const u64 saved_rsi = cpu.gpr.rsi;
    const u64 saved_rbx = cpu.gpr.rbx;
    const u64 saved_rbp = cpu.gpr.rbp;
    const u64 saved_r12 = cpu.gpr.r12;
    const u64 saved_r13 = cpu.gpr.r13;
    const u64 saved_r14 = cpu.gpr.r14;
    const u64 saved_r15 = cpu.gpr.r15;
    std::array<cpu::XmmReg, 10> saved_xmm_nonvolatile{};
    std::copy_n(cpu.sse.xmm + 6, saved_xmm_nonvolatile.size(),
                saved_xmm_nonvolatile.begin());
    const u32 saved_mxcsr_control = cpu.sse.mxcsr & 0xffc0u;

    cpu.gpr.rcx = cpu.gpr.r10;
    const bool cs_on = diagnostics::callstream_enabled();
    std::string cs_name;
    u64 cs_args[12] = {};
    u32 cs_argc = 0;
    u64 cs_caller = 0;
    if (cs_on && !cpu.is_64bit()) {
        cs_name = thunk_registry.resolve_name(static_cast<GuestAddr>(cpu.rip));
        const u16 ab = thunk_registry.resolve_stdcall_bytes(
            static_cast<GuestAddr>(cpu.rip));
        cs_argc = static_cast<u32>(ab / 4u);
        if (cs_argc > 12) cs_argc = 12;
        const GuestAddr sp = static_cast<GuestAddr>(cpu.gpr.rsp);
        cs_caller = marshaler.memory().read_u32(sp).value_or(0);
        for (u32 i = 0; i < cs_argc; ++i)
            cs_args[i] = marshaler.memory().read_u32(sp + 4 + i * 4).value_or(0);
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
    if (cs_on && !cpu.is_64bit()) {
        diagnostics::callstream_record(
            cs_name, winapi::ThreadManager::get_current_thread_id_static(),
            cs_args, cs_argc, cpu.gpr.rax, marshaler.memory(), cpu, cs_caller);
    }

    const bool transferred_control = cpu.thunk_control_transfer;
    cpu.thunk_control_transfer = false;
    if (transferred_control) {
        return NativeServiceDispatch::ControlTransferred;
    }

    cpu.gpr.rcx = syscall_return_rip;
    cpu.gpr.rdi = saved_rdi;
    cpu.gpr.rsi = saved_rsi;
    cpu.gpr.rbx = saved_rbx;
    cpu.gpr.rbp = saved_rbp;
    cpu.gpr.r12 = saved_r12;
    cpu.gpr.r13 = saved_r13;
    cpu.gpr.r14 = saved_r14;
    cpu.gpr.r15 = saved_r15;
    std::copy(saved_xmm_nonvolatile.begin(), saved_xmm_nonvolatile.end(),
              cpu.sse.xmm + 6);
    // MXCSR control is callee-saved, except for the CRT functions whose
    // documented purpose is to change it (CpuState::thunk_sets_fp_control).
    if (!cpu.thunk_sets_fp_control) {
        cpu.sse.mxcsr = (cpu.sse.mxcsr & 0x3fu) | saved_mxcsr_control;
    }
    cpu.thunk_sets_fp_control = false;
    return NativeServiceDispatch::Dispatched;
}

NativeServiceDispatch dispatch_x86_native_service(
    cpu::CpuState& cpu,
    winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard) {
    // A number past the table stays UnknownService: WoW64 has no defined
    // result for one (ntdll_service_layout_probe measured 0xC000000D,
    // 0xC0000005 and 0 for numbers just past its table and far beyond it).
    const u32 service_number = cpu.gpr.get32(
        static_cast<u8>(cpu::RegIndex::RAX));
    const GuestAddr thunk =
        thunk_registry.native_service_thunk(service_number);
    if (thunk == 0) {
        return NativeServiceDispatch::UnknownService;
    }
    const auto handler_result = thunk_registry.resolve_handler(thunk);
    if (handler_result.is_err()) {
        return NativeServiceDispatch::UnknownService;
    }

    // SYSENTER (Intel SDM Vol. 2) does not save RIP in RCX or RFLAGS in R11.
    // Windows 32-bit KiFastSystemCall is `mov edx, esp; sysenter` with EAX as
    // the native service number and stdcall args on the user stack. Vitreo
    // does not load IA32_SYSENTER_CS/ESP/EIP; user ESP and the interpreter's
    // post-SYSENTER EIP (typically the ntdll `ret` / `ret n`) remain. Do not
    // apply the x64 R10→RCX argument shuffle.
    const u32 saved_ebx = cpu.gpr.get32(
        static_cast<u8>(cpu::RegIndex::RBX));
    const u32 saved_esi = cpu.gpr.get32(
        static_cast<u8>(cpu::RegIndex::RSI));
    const u32 saved_edi = cpu.gpr.get32(
        static_cast<u8>(cpu::RegIndex::RDI));
    const u32 saved_ebp = cpu.gpr.get32(
        static_cast<u8>(cpu::RegIndex::RBP));

    const bool cs_on = diagnostics::callstream_enabled();
    std::string cs_name;
    u64 cs_args[12] = {};
    u32 cs_argc = 0;
    u64 cs_caller = 0;
    if (cs_on && !cpu.is_64bit()) {
        cs_name = thunk_registry.resolve_name(static_cast<GuestAddr>(cpu.rip));
        const u16 ab = thunk_registry.resolve_stdcall_bytes(
            static_cast<GuestAddr>(cpu.rip));
        cs_argc = static_cast<u32>(ab / 4u);
        if (cs_argc > 12) cs_argc = 12;
        const GuestAddr sp = static_cast<GuestAddr>(cpu.gpr.rsp);
        cs_caller = marshaler.memory().read_u32(sp).value_or(0);
        for (u32 i = 0; i < cs_argc; ++i)
            cs_args[i] = marshaler.memory().read_u32(sp + 4 + i * 4).value_or(0);
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
    if (cs_on && !cpu.is_64bit()) {
        diagnostics::callstream_record(
            cs_name, winapi::ThreadManager::get_current_thread_id_static(),
            cs_args, cs_argc, cpu.gpr.rax, marshaler.memory(), cpu, cs_caller);
    }

    const bool transferred_control = cpu.thunk_control_transfer;
    cpu.thunk_control_transfer = false;
    if (transferred_control) {
        return NativeServiceDispatch::ControlTransferred;
    }

    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RBX), saved_ebx);
    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RSI), saved_esi);
    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RDI), saved_edi);
    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RBP), saved_ebp);
    return NativeServiceDispatch::Dispatched;
}

} // namespace

NativeServiceDispatch dispatch_native_service(
    cpu::CpuState& cpu,
    winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard) {
    if (cpu.is_64bit()) {
        return dispatch_x64_native_service(
            cpu, thunk_registry, marshaler, suspend_guard);
    }
    return dispatch_x86_native_service(
        cpu, thunk_registry, marshaler, suspend_guard);
}

} // namespace vitreo::cli
