#pragma once
/// @file ntdll_internal.hpp
/// @brief Shared surface between ntdll_ext.cpp and ntdll_registration.cpp.
///
/// Private to ntdll's implementation. These were file-local before the export
/// table was split out; the header exists so the two halves can still see each
/// other, not to publish them.
///
/// They sit in a nested namespace rather than directly in vitreo::winapi
/// because ntdll's names are the ones kernel32 wraps — an unqualified
/// impl_NtCreateFile in the namespace both share is a collision waiting for
/// whoever writes the next one.

#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"

namespace vitreo::winapi::ntdll {

/// The calling thread's TEB (x64 GS base, x86 FS base), 0 without a CPU.
[[nodiscard]] inline GuestAddr current_teb(Marshaler& m) noexcept {
    const cpu::CpuState* cpu = m.cpu();
    if (cpu == nullptr) return 0;
    return static_cast<GuestAddr>(cpu->is_64bit() ? cpu->seg.gs_desc.base
                                                  : cpu->seg.fs_desc.base);
}

/// TEB.LastStatusValue, located on Windows 11 by rtl_string_status_probe
/// (the one TEB dword RtlNtStatusToDosError stores): x64 +0x1250, x86 +0xBF4.
[[nodiscard]] inline u32 teb_last_status_offset(const Marshaler& m) noexcept {
    return m.cpu_is_64bit() ? 0x1250u : 0xBF4u;
}

void impl_NtQuerySemaphore(Marshaler& m, NtdllExtManager& mgr);
void impl_RtlNtStatusToDosError(Marshaler& m, NtdllExtManager& mgr);
void impl_RtlGetVersion(Marshaler& m, NtdllExtManager& mgr);
void impl_RtlQueryElevationFlags(Marshaler& m);
void impl_NtQueryVirtualMemory(Marshaler& m, NtdllExtManager& mgr);
void impl_NtQuerySystemInformation(Marshaler& m, NtdllExtManager& mgr);
void impl_NtQuerySection(Marshaler& m, NtdllExtManager& mgr);
void impl_NtCreateSection(Marshaler& m, NtdllExtManager& mgr);
void impl_NtMapViewOfSection(Marshaler& m, NtdllExtManager& mgr);
void impl_NtUnmapViewOfSection(Marshaler& m, NtdllExtManager& mgr);
void impl_NtClose(Marshaler& m, NtdllExtManager& mgr);
void impl_NtDelayExecution(Marshaler& m, NtdllExtManager& mgr);
void impl_NtWaitForSingleObject(Marshaler& m, NtdllExtManager& mgr);
void impl_NtCreateFile(Marshaler& m, NtdllExtManager& mgr);
void impl_NtOpenFile(Marshaler& m, NtdllExtManager& mgr);
void impl_NtQueryDirectoryFile(Marshaler& m, NtdllExtManager& mgr);
void impl_NtReadFile(Marshaler& m, NtdllExtManager& mgr);
void impl_NtWriteFile(Marshaler& m, NtdllExtManager& mgr);
void impl_NtDeviceIoControlFile(Marshaler& m, NtdllExtManager& mgr);
void impl_NtQueryVolumeInformationFile(Marshaler& m, NtdllExtManager& mgr);

} // namespace vitreo::winapi::ntdll
