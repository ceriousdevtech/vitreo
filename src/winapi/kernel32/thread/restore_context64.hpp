#pragma once
/// @file restore_context64.hpp
/// @brief x64 RtlRestoreContext, including its ExceptionRecord forms.

#include <vitreo/common.hpp>

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;

/// winnt.h / ntstatus.h.
inline constexpr u32 STATUS_LONGJUMP_CODE = 0x80000026u;
inline constexpr u32 STATUS_UNWIND_CONSOLIDATE_CODE = 0x80000029u;

/// RtlRestoreContext(ContextRecord, ExceptionRecord) for the x64 CPU attached
/// to @p m, as Microsoft Learn documents it:
///
///  - no record, or any other code: resume in @p ctx_addr.
///  - STATUS_LONGJUMP: first copy the non-volatile state of the _JUMP_BUFFER
///    at ExceptionInformation[0] into the context.
///  - STATUS_UNWIND_CONSOLIDATE: call ExceptionInformation[0](ExceptionRecord)
///    on a frame that hides every frame between it and @p ctx_addr's frame
///    from exception handling, while leaving their stack data in place; the
///    callback's return value becomes the context's RIP.
///
/// RtlUnwindEx ends in exactly this call, with the target frame's context.
///
/// @p stack_ceiling is the lowest guest stack address still in use (the
/// caller's RSP). The consolidation frame is built below it so the frames it
/// hides -- and the throw object inside them -- stay intact.
///
/// The CPU is left positioned to resume; the caller must suppress its own
/// return (thunk_control_transfer). Returns false, with the CPU unchanged,
/// if a record is unreadable or the consolidation frame cannot be built.
[[nodiscard]] bool restore_context64_with_record(Marshaler& m,
                                                 ModuleManager& modules,
                                                 GuestAddr ctx_addr,
                                                 GuestAddr exception_record,
                                                 u64 stack_ceiling);

/// The RtlRestoreContext export (kernel32 and ntdll): ContextRecord in arg 0,
/// ExceptionRecord in arg 1. Does not return on success. An unreadable record
/// raises STATUS_ACCESS_VIOLATION, which is what touching it does on Windows.
void rtl_restore_context(Marshaler& m, ModuleManager* modules);

} // namespace vitreo::winapi
