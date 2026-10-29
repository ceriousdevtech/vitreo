// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file native32_loop.hpp
/// @brief Production 32-bit native execution on Intel macOS.
///
/// Wired into run_execution_loop / run_guest_callback on VITREO_ARCH_X86_64
/// when identity mapping is active and VITREO_NO_NATIVE32 is unset.

#include "runtime/execution/launch_request.hpp"
#include "runtime/execution/guest_run_loop.hpp"

#include <vitreo/result.hpp>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi {
class ThunkRegistry;
class ProcessManager;
}
}

namespace vitreo::cli {

/// Trampolines come from Compat32Executor::for_current_thread()'s pool.
/// Shared thunk and sentinel stubs are FS-relative and thread-independent.

/// True when this process should run 32-bit guests natively.
[[nodiscard]] bool native32_wanted() noexcept;

/// True after a successful native32_prepare().
[[nodiscard]] bool native32_is_prepared() noexcept;

/// Create per-thread trampolines on demand, install the Mach exception
/// bridge, write shared FS-relative thunk stubs, and map executable pages
/// at the process/callback sentinels.
[[nodiscard]] VoidResult native32_prepare(GuestMemory& memory,
                                          winapi::ThunkRegistry& thunk_registry);

/// Native stand-in for run_execution_loop on 32-bit Intel.
[[nodiscard]] RunResult run_native32_loop(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    const RunConfig& config,
    EventPumpCallback event_pump);

/// Run native 32-bit code until RIP hits @p sentinel (or the process exits).
/// Used by run_guest_callback after the callback frame is already on the stack.
///
/// @return true if this thread entered native 32-bit execution. False means
///         the caller must interpret: the trampoline pool was exhausted or
///         this thread has no TEB from which shared stubs can dispatch.
bool native32_run_until_sentinel(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 sentinel,
    u32 seh_abandon_ctx_addr,
    u32 seh_abandon_esp_min,
    u64 func_addr,
    u64* result_eax,
    bool* completed_normally);

} // namespace vitreo::cli
