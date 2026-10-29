#pragma once
/// @file thread_context.hpp
/// @brief GetThreadContext / SetThreadContext (and their ntdll frontends
///        NtGetContextThread / NtSetContextThread) -- one implementation.
///
/// Contract: Learn GetThreadContext / SetThreadContext, measured on Windows 11
/// x64 by tools/oracle_probe/thread_context_probe.c (.expected alongside):
///  - ContextFlags selects the CONTEXT groups read or written; everything else
///    in the caller's record is left alone (cpu::store_context64 /
///    cpu::load_context64 hold the per-group layout).
///  - A CONTEXT that is not 16-byte aligned fails with
///    STATUS_DATATYPE_MISALIGNMENT, which kernel32 reports as ERROR_NOACCESS.
///  - Success leaves the thread's last error untouched.

#include <vitreo/common.hpp>

namespace vitreo::cpu {
struct CpuState;
}

namespace vitreo::winapi {

class Marshaler;
class ThreadManager;

/// Gives a new guest thread's register file the floating-point state Windows
/// starts every user thread with -- main thread and CreateThread, x86 and x64:
/// x87 control word 0x27F (53-bit precision, all exceptions masked; NOT the
/// FNINIT value 0x37F) and MXCSR 0x1F80. Measured:
/// tools/oracle_probe/initial_fpu_state_probe.c / .expected.
void seed_windows_initial_fpu(cpu::CpuState& state);

/// Returns an NTSTATUS.
[[nodiscard]] u32 nt_get_context_thread(Marshaler& m, ThreadManager* threads,
                                        u64 handle, GuestAddr context);
/// Returns an NTSTATUS.
[[nodiscard]] u32 nt_set_context_thread(Marshaler& m, ThreadManager* threads,
                                        u64 handle, GuestAddr context);

/// The Win32 error kernel32 reports for a status the two functions above
/// return (RtlNtStatusToDosError for that set).
[[nodiscard]] u32 thread_context_status_to_win32(u32 status);

} // namespace vitreo::winapi
