// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file native64_loop.hpp
/// @brief Production Win64 native execution on Intel macOS.
///
/// The counterpart to native32_loop for 64-bit guests, and structurally
/// different for one reason: GS.
///
/// A 32-bit guest reaches its TEB through FS, which macOS does not use, so the
/// host thread swaps FS, calls into the guest and returns -- one thread does
/// both jobs. A 64-bit guest reaches its TEB through GS, which holds the
/// host's pthread TLS, and that swap cannot be undone (see
/// x64_guest_thread.hpp). So guest code runs on a thread that owns guest GS
/// for life, and this loop runs on the *host* thread that services it. The
/// two rendezvous through `GuestHostHop`.
///
/// The consequence worth stating plainly: every WinAPI call costs a thread
/// hand-off. CPU-bound guest code pays nothing -- it runs at hardware speed
/// between boundaries -- which is the whole point, since separating
/// translation defects from API defects is what the Intel backend is for.
///
/// Opt-in while incomplete: `VITREO_NATIVE64=1`. Default off, so the
/// interpreted path that works today is not disturbed.

#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/launch_request.hpp"

#include <vitreo/result.hpp>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi {
class ThunkRegistry;
class ProcessManager;
class ModuleManager;
}
}

namespace vitreo::cli {

/// True when this process should run 64-bit guests natively.
/// Opt-in: VITREO_NATIVE64=1.
[[nodiscard]] bool native64_wanted() noexcept;

/// True after a successful native64_prepare().
[[nodiscard]] bool native64_is_prepared() noexcept;

/// Fill the thunk region with native x86_64 stubs.
///
/// Unlike native32, there is nothing per-thread to build: the stubs address
/// their slots as `gs:[offset]`, and GS already selects the running thread's
/// TEB, so one shared copy serves every guest thread.
/// @param modules Loaded-module table, used to confine the segment-override
///                rewrite to real instruction boundaries (.pdata). May be
///                null, which falls back to a linear sweep.
[[nodiscard]] VoidResult native64_prepare(
    GuestMemory& memory, winapi::ThunkRegistry& thunk_registry,
    const winapi::ModuleManager* modules = nullptr);

/// Run native 64-bit guest code until RIP reaches @p sentinel.
///
/// Used by run_guest_callback for guest thread entry and every host->guest
/// callback. Returns false when this thread has no native64 guest thread, in
/// which case the caller must NOT fall back to the interpreter: the thunk
/// region holds native stubs the interpreter cannot decode.
bool native64_run_until_sentinel(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u64 sentinel,
    u64* result_rax,
    bool* completed_normally);

/// Native stand-in for run_execution_loop on 64-bit Intel.
[[nodiscard]] RunResult run_native64_loop(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    const RunConfig& config,
    EventPumpCallback event_pump);

} // namespace vitreo::cli
