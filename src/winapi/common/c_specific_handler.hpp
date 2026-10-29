#pragma once
/// @file c_specific_handler.hpp
/// @brief The x64 language-specific handler __C_specific_handler, shared by
///        ntdll.dll and the CRT DLLs (msvcrt, ucrtbase) that export it.

#include <functional>

#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Call guest function @p fn with @p count integer arguments (Win64 ABI) on
/// @p cpu and return RAX. *completed is false when the call did not return
/// normally (it raised or transferred control away).
using SehGuestCall = std::function<u64(cpu::CpuState& cpu, GuestAddr fn, const u64* args,
                                       usize count, bool* completed)>;

/// Install the guest-call hook the handler runs filters and termination
/// handlers through. Set once when the runtime is wired, before guest code.
void set_seh_guest_call(SehGuestCall call);

/// The installed hook (empty before the runtime is wired). RtlUnwindEx calls
/// unwind handlers through it too (kernel32/thread/unwind64.hpp).
[[nodiscard]] const SehGuestCall& seh_guest_call();

/// Ask the x64 dispatcher to unwind to @p target_frame / @p target_ip, as
/// RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, ...) does.
void request_x64_unwind(cpu::CpuState& cpu, u64 target_frame, u64 target_ip,
                        u64 exception_record, u64 return_value);

/// The export: EXCEPTION_DISPOSITION __C_specific_handler(
///     PEXCEPTION_RECORD, PVOID EstablisherFrame, PCONTEXT, PDISPATCHER_CONTEXT).
[[nodiscard]] ExportEntry make_c_specific_handler_export();

}  // namespace vitreo::winapi
