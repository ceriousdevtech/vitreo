#pragma once
/// @file thunk_fast_path.hpp
/// @brief Inline and reduced-bookkeeping handling for the hottest thunks.
///
/// `GetLastError`, `SetLastError`, `GetCurrentThreadId`, tick/QPC clocks,
/// Sleep (non-alertable), TLS get/set, and Interlocked* are called constantly
/// from CRT, COM, and wait loops. Dispatching each through the generic path
/// costs a registry hash lookup, a `std::function` call through the Marshaler,
/// guest callee-saved register save and restore, and a TEB[0x34] read and
/// write on every call.
///
/// These implement the same contracts directly against CPU and guest memory,
/// then fall through to the standard thunk RET emulation. The observable
/// behavior must stay identical to the registered handlers — this is an
/// optimization, and any divergence is a bug in the fast path rather than a
/// documented difference.

#include <vitreo/common.hpp>
#include <functional>
#include <memory>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi {
class ThunkRegistry;
class CriticalSectionManager;
class SyncManager;
class Marshaler;
}
} // namespace vitreo

namespace vitreo::cli {

/// Guest addresses of the thunks eligible for inline handling.
///
/// Resolved once per run and compared against `cpu.rip` on every dispatch, so
/// the layout is ordered roughly by how often each is hit.
struct FastThunkAddrs {
    GuestAddr get_last_error{0};
    GuestAddr set_last_error{0};
    GuestAddr get_current_thread_id{0};
    GuestAddr get_current_process_id{0};
    GuestAddr get_tick_count{0};
    GuestAddr get_tick_count64{0};
    GuestAddr time_get_time{0};
    GuestAddr query_performance_counter{0};
    GuestAddr query_performance_frequency{0};
    GuestAddr sleep{0};
    GuestAddr sleep_ex{0};
    GuestAddr tls_get_value{0};
    GuestAddr tls_set_value{0};
    GuestAddr interlocked_increment{0};
    GuestAddr interlocked_decrement{0};
    GuestAddr interlocked_compare_exchange{0};
    GuestAddr is_debugger_present{0};
    GuestAddr heap_alloc{0};
    GuestAddr heap_free{0};
    GuestAddr wait_for_single_object{0};
    GuestAddr release_semaphore{0};
    GuestAddr get_overlapped_result{0};
    GuestAddr sleep_condition_variable_cs{0};
    GuestAddr acquire_srw_exclusive{0};
    GuestAddr release_srw_exclusive{0};
    GuestAddr try_acquire_srw_exclusive{0};
    GuestAddr acquire_srw_shared{0};
    GuestAddr release_srw_shared{0};
    GuestAddr try_acquire_srw_shared{0};
    GuestAddr enter_critical_section{0};
    GuestAddr leave_critical_section{0};
    GuestAddr try_enter_critical_section{0};
    GuestAddr fgetc{0};
    GuestAddr ungetc{0};
    const std::function<void(winapi::Marshaler&)>* fgetc_handler{nullptr};
    const std::function<void(winapi::Marshaler&)>* ungetc_handler{nullptr};
    std::shared_ptr<const void> membership_probe;
    bool resolved{false};
};

/// Bind the process CriticalSectionManager used by Enter/Leave/TryEnter
/// fast thunks. Required before those APIs can be handled inline; null
/// falls through to the slow path.
void set_fast_thunk_cs_manager(winapi::CriticalSectionManager* mgr);

/// Bind the process SyncManager used by SleepConditionVariableCS's hot path.
/// Null falls through to the registered handler.
void set_fast_thunk_sync_manager(winapi::SyncManager* mgr);

/// The managers currently bound (a test that binds its own restores these).
[[nodiscard]] winapi::CriticalSectionManager* fast_thunk_cs_manager() noexcept;
[[nodiscard]] winapi::SyncManager* fast_thunk_sync_manager() noexcept;

/// Look up each fast-path thunk's address. Idempotent; does nothing once
/// `resolved` is set.
void resolve_fast_thunks(FastThunkAddrs& f, const winapi::ThunkRegistry& reg);

/// True for thunks worth a reduced-bookkeeping dispatch even though they are
/// not handled inline — hot enough that full hostcall recording dominates.
[[nodiscard]] bool is_lean_hot_thunk(GuestAddr rip, const FastThunkAddrs& f);

/// Whether @p rip is a thunk maybe_fast_thunk_call serves inline. It may still
/// decline a given call (an alertable SleepEx, a null SRW lock, an unbound
/// manager); when it does, nothing has run. False until @p f is resolved.
[[nodiscard]] bool fast_thunk_has_body(GuestAddr rip,
                                       const FastThunkAddrs& f) noexcept;

/// Whether VITREO_HOSTCALL_HOT is set, forcing hot thunks back onto the fully
/// instrumented path.
[[nodiscard]] bool hot_hostcall_diagnostics_enabled();

/// Scribble the x86 callee stack area after a host thunk returns.
///
/// A real stdcall callee writes locals below the return address. Host-only
/// thunks otherwise leave prior guest data intact indefinitely, which later
/// code can observe. Default wipe is 0x200 bytes below ESP; set
/// VITREO_THUNK_STACK_CLOBBER=0 to disable or to another size to override.
/// Does not touch [ESP] (return address) or stdcall args at [ESP+4…].
/// No-op for 64-bit guests.
void maybe_clobber_x86_thunk_callee_stack(cpu::CpuState& cpu, GuestMemory& memory);

/// Handle the call inline if `cpu.rip` is a fast-path thunk.
///
/// @return true when handled — the caller must not also dispatch it.
[[nodiscard]] bool maybe_fast_thunk_call(cpu::CpuState& cpu, GuestMemory& memory,
                                         const FastThunkAddrs& f);

/// Emulate stdcall/x64 RET after a successful thunk body: pop the return
/// address and apply x86 stdcall stack cleanup from `thunk_addr`.
///
/// @return false on stack underflow (rip/sp unchanged on failure).
[[nodiscard]] bool finish_thunk_ret(cpu::CpuState& cpu, GuestMemory& memory,
                                    const winapi::ThunkRegistry& thunk_registry,
                                    GuestAddr thunk_addr);

} // namespace vitreo::cli
