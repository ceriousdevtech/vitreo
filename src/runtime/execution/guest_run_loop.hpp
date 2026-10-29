// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Execution loop: interpreter dispatch with thunk handling

#pragma once

#include "runtime/execution/launch_request.hpp"
#include "vitreo/guest_sentinels.hpp"
// PublishGuard is a nested class, so the native seam below cannot
// forward-declare it.
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include <functional>

namespace vitreo::cpu { struct CpuState; }
namespace vitreo::winapi {
    class ThunkRegistry;
    class ProcessManager;
    class ModuleManager;
    class Marshaler;
}
namespace vitreo { class GuestMemory; }

namespace vitreo::cli {

// Defined in vitreo/guest_sentinels.hpp so the WinAPI thread layer, which
// cannot include CLI headers, emits exactly the values this loop recognises.
inline constexpr u64 PROCESS_RETURN_SENTINEL = kProcessReturnSentinel;
inline constexpr u64 CALLBACK_RETURN_SENTINEL = kCallbackReturnSentinel;

/// Set thread-local ModuleManager for x64 SEH dispatch (.pdata lookup).
void set_module_manager(winapi::ModuleManager* mgr);

/// Guest frame interrupted by the current `run_guest_callback` trampoline.
/// x64 SEH must resume here when UNWIND_INFO walks to the callback sentinel.
struct Seh64HostCallbackFrame {
    u64 rip = 0;
    u64 rsp = 0;
    u64 gpr[16] = {};
};

/// Fill the innermost interrupted guest frame on this host thread.
[[nodiscard]] bool seh64_interrupted_guest_frame(Seh64HostCallbackFrame* out);

/// Callback to pump native events periodically during execution.
/// Called every N instructions to keep the UI responsive.
using EventPumpCallback = std::function<void()>;

/// Distinguishes a Windows thread's top-level entry point from callbacks
/// nested inside API dispatch (WndProc, guest callbacks, timer procedures).
enum class GuestCallbackKind {
    Nested,
    ThreadEntry,
    TimerProcedure,
    /// A thread entry that starts from the CpuState exactly as given --
    /// func_addr must equal cpu.rip and no arguments, return frame or stack
    /// adjustment are applied. For a CREATE_SUSPENDED thread whose context
    /// was changed with SetThreadContext: Windows resumes it at precisely that
    /// context. The run ends when the thread calls ExitThread (which transfers
    /// to the callback sentinel); there is no return frame to come back to.
    ThreadEntryAtContext,
};

/// Runs on a new thread after its DLL_THREAD_ATTACH notifications and before
/// its start routine: delivers APCs queued before the thread began (Learn
/// QueueUserAPC: "If an application queues an APC before the thread begins
/// running, the thread begins by calling the APC function";
/// thread_lifecycle_probe APC_BEFORE_START order=PT).
void set_thread_start_hook(std::function<void()> hook);

/// Return the CpuState currently executing guest code on this host thread.
///
/// Win32 callbacks can be requested from inside thunk handlers running on any
/// guest thread. Callback routers must use this per-host-thread state instead
/// of capturing the process main thread's CpuState.
[[nodiscard]] cpu::CpuState* active_guest_cpu() noexcept;

/// Use the active guest CpuState when one is published, otherwise use the
/// supplied bootstrap/main-thread state. The fallback is needed while loader
/// callbacks run before the main execution loop begins.
[[nodiscard]] cpu::CpuState& active_guest_cpu_or(
    cpu::CpuState& fallback) noexcept;

/// Native callback JIT is the production default for ordinary callbacks and
/// thread entries. Multimedia TIMEPROCs run on a long-lived service thread;
/// keep those on the interpreter reference path until callback codegen is
/// re-entrant under their high-frequency dispatch cadence.
[[nodiscard]] constexpr bool native_callback_jit_default(
    bool, GuestCallbackKind kind) noexcept {
    return kind != GuestCallbackKind::TimerProcedure;
}

/// Run the interpreter execution loop.
///
/// Repeatedly steps the interpreter, dispatching thunk calls when RIP
/// enters the thunk region.  Terminates on ExitProcess, instruction limit,
/// halt, exception, or unhandled interrupt.
///
/// @param cpu            CPU state (RIP should already point to entry)
/// @param memory         Guest memory
/// @param thunk_registry Registry for resolving thunk addresses to handlers
/// @param process_mgr    Checked for ExitProcess after each thunk dispatch
/// @param config         Run configuration (max_instructions, verbose)
/// @param event_pump     Optional callback to pump native events periodically
/// @return RunResult with exit code and instruction count
RunResult run_execution_loop(cpu::CpuState& cpu,
                             GuestMemory& memory,
                             winapi::ThunkRegistry& thunk_registry,
                             winapi::ProcessManager& process_mgr,
                             const RunConfig& config,
                             EventPumpCallback event_pump = nullptr);

/// Execute a guest callback function (e.g. WndProc, DialogProc).
///
/// Sets up a call frame, runs the interpreter until the callback returns,
/// and returns the result. Used by DispatchMessageW and SendMessageW.
///
/// @param cpu            CPU state (will be modified during execution)
/// @param memory         Guest memory
/// @param thunk_registry Registry for resolving thunk addresses
/// @param process_mgr    Checked for ExitProcess after each thunk dispatch
/// @param func_addr      Guest address of the callback function
/// @param args           Arguments to pass (for x86: pushed on stack in reverse order)
/// @param arg_count      Number of arguments
/// @param is_64bit       Whether to use x64 calling convention
/// @param completed_normally Set true only when the callback returns through the sentinel
/// @param kind           Top-level guest thread entry or nested API callback
/// @param seh_abandon_ctx_addr If non-zero, this callback is a 32-bit frame SEH
///        handler. Packers (Petite) may unwind past the dispatcher and jump to
///        the OEP instead of returning a disposition. When guest ESP reaches
///        at least @p seh_abandon_esp_min (the establisher frame) and RIP
///        leaves the handler's page, CPU state is retained directly and the
///        callback reports a transfer. This CONTEXT address enables detection;
///        it is never written after a transfer because its storage may overlap
///        the restored stack.
/// @param seh_abandon_esp_min Establisher-frame address (see above); ignored
///        unless @p seh_abandon_ctx_addr is set.
///        The handler is also treated as having left once ESP rises above its
///        own argument area -- how MSVC's catch continuation and __except
///        entry leave -- whatever page RIP is on.
/// @return The callback's return value
u64 run_guest_callback(cpu::CpuState& cpu,
                       GuestMemory& memory,
                       winapi::ThunkRegistry& thunk_registry,
                       winapi::ProcessManager& process_mgr,
                       u64 func_addr,
                       const u64* args,
                       usize arg_count,
                       bool is_64bit,
                       bool* completed_normally = nullptr,
                       GuestCallbackKind kind = GuestCallbackKind::Nested,
                       u32 seh_abandon_ctx_addr = 0,
                       u32 seh_abandon_esp_min = 0);

/// True, once, after the most recent SEH frame-handler callback on this
/// thread LEFT instead of returning (see seh_abandon_ctx_addr). Its caller
/// never runs the code after the call -- on Windows, the epilogue of
/// RtlpExecuteHandlerForException that pops the protector registration.
[[nodiscard]] bool consume_seh32_handler_left_dispatcher() noexcept;
/// Record a nonreturning frame-handler transfer in either execution backend.
void mark_seh32_handler_left_dispatcher() noexcept;

/// Initialize per-thread JIT state for worker threads.
/// Must be called at the start of each new host thread that will
/// run guest code (via run_guest_callback).  The caller owns the
/// returned block cache and must keep it alive for the thread's
/// lifetime.  Pass the GuestMemory reference that this thread uses.
void init_thread_jit_state(GuestMemory& memory);

// ---------------------------------------------------------------------------
// Native-backend seam
// ---------------------------------------------------------------------------

/// Outcome of dispatching one API thunk on behalf of a native backend.
enum class NativeThunkDispatch : u8 {
    /// Handler ran and the thunk's RET was applied; `cpu.rip` is the guest's
    /// resume address.
    Ok,
    /// The handler transferred control itself (`thunk_control_transfer` or a
    /// pending unwind). `cpu.rip` is already final; do not RET.
    ControlTransferred,
    /// No handler is registered for this slot.
    Unresolved,
    /// Stack underflow emulating the thunk's RET.
    Fatal,
};

/// Dispatch one API thunk for a native execution backend.
///
/// Precondition: `cpu.rip` is the thunk slot's dispatch gateway and the
/// guest's own return address is on top of its stack -- exactly the state the
/// interpreter reaches a thunk in. Native backends are responsible for
/// producing that state; everything after it is shared.
///
/// This exists so a native backend never grows a second copy of argument
/// marshalling, the calling convention, `last_error` propagation, the hostcall
/// ring or call-stream recording. A fix to a Windows API must benefit every
/// backend, which it cannot do if each one dispatches its own way.
/// Dispatch a guest CPU fault taken under a native backend.
///
/// `cpu` must hold the faulting register file, with `cpu.rip` at the faulting
/// instruction. Runs the same table-based x64 SEH/VEH dispatch the interpreter
/// uses, so a guest's __try/__except and vectored handlers behave identically
/// on both backends.
///
/// @return true when a handler resolved it and `cpu` has been updated to
///         resume; false when nothing handled it, which is fatal for the guest.
[[nodiscard]] bool dispatch_guest_fault_native(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 status,
    u64 fault_address,
    bool is_write);

[[nodiscard]] NativeThunkDispatch dispatch_api_thunk_native(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard);

} // namespace vitreo::cli
