// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file guest_run_loop_internal.hpp
/// @brief Private seams between the guest_run_loop*.cpp files (the run
///        loop, the callback loop, thunk dispatch, in-chain thunks, native
///        services, per-thread state and diagnostics), split out of
///        guest_run_loop.cpp as verbatim code motion. Each function and
///        variable declared here is defined once, in one of those files.
///        Include from those files only.

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_code_arena.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/thunk_fast_path.hpp"
#include <cstdio>

namespace vitreo::cpu::jit {
struct BasicBlock;
class BlockCache;
class JitCompiler;
}  // namespace vitreo::cpu::jit
namespace vitreo::winapi {
class ThunkRegistry;
class Marshaler;
class ModuleManager;
}  // namespace vitreo::winapi

namespace vitreo {

/// Human-readable label for CpuException values used in run-loop diagnostics.
[[nodiscard]] inline const char* exception_name(cpu::CpuException ex) {
    // Note: CpuException::None and CpuException::DivideError both map to 0.
    // Switch on the underlying integer value to avoid duplicate-case errors.
    const auto ex_u = static_cast<u8>(ex);
    switch (ex_u) {
        case 0:  return "None/DivideError";
        case 1:  return "Debug";
        case 2:  return "Nmi";
        case 3:  return "Breakpoint";
        case 4:  return "Overflow";
        case 5:  return "BoundRange";
        case 6:  return "InvalidOpcode";
        case 7:  return "DeviceNotAvailable";
        case 8:  return "DoubleFault";
        case 10: return "InvalidTss";
        case 11: return "SegmentNotPresent";
        case 12: return "StackFault";
        case 13: return "GeneralProtection";
        case 14: return "PageFault";
        case 16: return "FloatingPoint";
        case 17: return "AlignmentCheck";
        case 18: return "MachineCheck";
        case 19: return "SimdFloatingPoint";
    }
    return "Unknown";
}

} // namespace vitreo

// guest_run_loop_diag.cpp
void pctrace_dump_caller(std::FILE* out, vitreo::cpu::CpuState& cpu,
                         vitreo::GuestMemory& memory);
void dump_fastfail_context(const vitreo::cpu::CpuState& cpu,
                           vitreo::GuestMemory& memory, vitreo::u64 code);

// guest_run_loop_chain_thunk.cpp
extern constinit thread_local bool t_chain_slow_thunk_pending_raise;
extern constinit thread_local bool t_chain_slow_thunk_unwinding;

namespace vitreo::cli {

// guest_run_loop_native_service.cpp
enum class NativeServiceDispatch {
    Dispatched,
    UnknownService,
    ControlTransferred,
};

NativeServiceDispatch dispatch_native_service(
    cpu::CpuState& cpu,
    winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard);

// guest_run_loop_diag.cpp
void append_pc_watch_memory(const cpu::CpuState& cpu, GuestMemory& memory);
void dump_fatal_guest_context(vitreo::cpu::CpuState& cpu,
                              GuestMemory& memory);

// guest_run_loop_thunks.cpp
bool follow_thunk_sled_jump(cpu::CpuState& cpu, GuestMemory& memory,
                            const winapi::ThunkRegistry& thunk_registry);
void maybe_skip_thunk_nop_sled(cpu::CpuState& cpu, GuestMemory& memory,
                               const winapi::ThunkRegistry& thunk_registry);

/// Result of the shared slow-path API thunk body (resolve_handler path).
enum class SlowThunkDispatch : u8 {
    /// Handler ran; RET (or guard-dispatch rip rewrite) applied.
    Ok,
    /// Stack underflow during RET emulation.
    Fatal,
    /// Handler set thunk_control_transfer / pending_unwind; rip already final
    /// (or still at thunk for unwind — caller must not RET).
    ControlTransferred,
    /// resolve_handler failed.
    Unresolved,
};

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
    bool direct_checked = false);

// guest_run_loop_chain_thunk.cpp
/// Result of attempting to handle an API thunk inside a JIT chain.
enum class ChainThunkOutcome : u8 {
    /// RIP is not in the thunk region — caller should lookup_fast as usual.
    NotThunk,
    /// Thunk ran (fast or slow); `block` is lookup_fast(return rip) (may be null).
    Handled,
    /// Patched stub / unresolved — break to the outer dispatcher.
    BreakOuter,
    /// Stack underflow during RET emulation (or arena lease re-acquire fail).
    Fatal,
};

[[nodiscard]] ChainThunkOutcome try_chain_thunk_inline(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    const winapi::ThunkRegistry& thunk_registry,
    winapi::Marshaler& marshaler,
    winapi::ThreadSuspendRegistry::PublishGuard& suspend_guard,
    const FastThunkAddrs& fast_thunks,
    cpu::jit::BlockCache& block_cache,
    cpu::jit::BasicBlock*& block,
    winapi::ModuleManager* mods,
    cpu::jit::JitCodeArena::ArenaReadLease* chain_lease,
    bool slow_hostcall_diag,
    u64 slow_hostcall_threshold_ms);

// guest_run_loop_thread_state.cpp
extern constinit thread_local cpu::jit::BlockCache*  t_block_cache;
extern constinit thread_local cpu::jit::JitCompiler* t_jit_compiler;
extern constinit thread_local bool                   t_jit_enabled;

usize per_thread_block_cache_capacity();

bool ensure_block_decoded(GuestMemory& memory,
                          cpu::jit::BlockCache& cache,
                          cpu::jit::BasicBlock& block);

enum class DispatchCompileStatus { NotAttempted, Published, Deferred, Failed };

struct DispatchCompileResult {
    DispatchCompileStatus status{DispatchCompileStatus::NotAttempted};
    u32 code_size{0};
};

DispatchCompileResult compile_and_publish_dispatch_block(
    cpu::jit::BasicBlock& block, cpu::jit::JitCompiler& compiler);
DispatchCompileResult compile_dispatch_block(
    cpu::jit::BasicBlock& block, cpu::jit::JitCompiler* compiler);
cpu::jit::BasicBlock* prepare_dispatch_block(
    cpu::CpuState& cpu, GuestMemory& memory, cpu::jit::BlockCache& block_cache,
    cpu::jit::JitCompiler* jit_compiler,
    winapi::ThunkRegistry& thunk_registry);

extern constinit thread_local winapi::ModuleManager* t_module_manager;
extern winapi::ModuleManager* g_module_manager;

/// The calling thread's module manager, else the process-wide fallback.
/// Inline: the thunk dispatch paths call it once per guest API call.
inline winapi::ModuleManager* module_manager() {
    return t_module_manager != nullptr ? t_module_manager : g_module_manager;
}

extern constinit thread_local cpu::CpuState* t_active_guest_cpu;

class ActiveGuestCpuScope {
public:
    explicit ActiveGuestCpuScope(cpu::CpuState& cpu) noexcept
        : m_previous(t_active_guest_cpu) {
        t_active_guest_cpu = &cpu;
    }

    ~ActiveGuestCpuScope() {
        t_active_guest_cpu = m_previous;
    }

    ActiveGuestCpuScope(const ActiveGuestCpuScope&) = delete;
    ActiveGuestCpuScope& operator=(const ActiveGuestCpuScope&) = delete;

private:
    cpu::CpuState* m_previous;
};

} // namespace vitreo::cli
