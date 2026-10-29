#pragma once
/// @file exception_dispatch.hpp
/// @brief x64 table-based SEH dispatch.
///
/// 32-bit Windows threads carry a linked list of exception registration
/// records on the stack. 64-bit ones do not: the unwind information lives in
/// the module's `.pdata`/`.xdata` sections, and dispatching an exception means
/// walking those tables to find a handler, reconstructing each frame's
/// register state from its UNWIND_INFO on the way.
///
/// That table walk is a self-contained algorithm over guest memory and module
/// layout, which is why it separates cleanly from the loop that invokes it.
/// The 32-bit path lives elsewhere, in cpu/seh/seh32.cpp.

#include <vitreo/common.hpp>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi {
class ThunkRegistry;
class ProcessManager;
class ModuleManager;
} // namespace winapi
} // namespace vitreo

namespace vitreo::cli {

/// Dispatch a 64-bit guest exception through the module's unwind tables.
///
/// Walks frames from @p exception_address outward looking for a language
/// handler, building the guest-visible EXCEPTION_RECORD, CONTEXT, and
/// DISPATCHER_CONTEXT each handler expects, and transfers control to the
/// first handler that claims the exception.
///
/// @param raise_code    Exception code, as passed to RaiseException.
/// @param raise_flags   EXCEPTION_RECORD flags; continuable vs not.
/// @param raise_nargs   Count of @p raise_args.
/// @param raise_args    Exception parameters, or null when there are none.
/// @param exception_address Faulting or raising address, where the walk begins.
/// @param module_mgr    Resolves an address to its module and unwind tables.
///        Required — dispatch cannot proceed without it, and returns false.
///
/// @return true when a handler took the exception and @p cpu was updated to
///         resume in it; false when no handler claimed it, leaving the caller
///         to treat it as unhandled.
[[nodiscard]] bool dispatch_x64_exception(cpu::CpuState& cpu,
                                          GuestMemory& memory,
                                          winapi::ThunkRegistry& thunk_registry,
                                          winapi::ProcessManager& process_mgr,
                                          u32 raise_code, u32 raise_flags,
                                          u32 raise_nargs, const u64* raise_args,
                                          u64 exception_address,
                                          winapi::ModuleManager* module_mgr,
                                          u64 record_address = 0);

/// What a run loop does after apply_pending_unwind_64.
enum class PendingUnwind64Action {
    /// The CPU is at the unwind target; keep running this level.
    Resume,
    /// The unwind continues beyond this guest-callback level: leave it
    /// without a RET (pending_unwind stays set for the enclosing level).
    LeaveLevel,
    /// cpu.pending_raise is set (STATUS_BAD_STACK, a nested fault, ...):
    /// take the loop's own pending-raise path. A thunk raise still has its
    /// return address on the stack, as a RaiseException thunk's does.
    Raise,
    /// The process ended.
    Stop,
};

/// x64: a thunk came back with cpu.pending_unwind set (DirectThunkResult::
/// Unwinding, SlowThunkDispatch::ControlTransferred, or the chain's
/// t_chain_slow_thunk_unwinding), RIP still at its gateway and RSP at its
/// return address. Either the frames of this level were already walked
/// (pending_unwind.level_walked: RtlUnwindEx's own walk reached the callback
/// boundary, so a callback level leaves), or the unwind crossed out of a
/// guest callback the thunk's host code ran and goes on here from the
/// thunk's caller (winapi::continue_pending_unwind64). @p in_callback: this
/// is run_guest_callback's loop, which has an enclosing level.
[[nodiscard]] PendingUnwind64Action apply_pending_unwind_64(
    cpu::CpuState& cpu, GuestMemory& memory,
    winapi::ModuleManager* module_mgr, winapi::ProcessManager& process_mgr,
    bool in_callback);

} // namespace vitreo::cli
