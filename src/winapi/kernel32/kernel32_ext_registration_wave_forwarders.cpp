// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_wave_forwarders.cpp
/// @brief Extended kernel32 export table, part 7 of 9: wave 1/3/6 forwarders.
///
/// A contiguous slice of the table that create_kernel32_ext_thunks builds
/// (see kernel32_ext_registration.cpp). The slice boundaries follow the
/// table's registration order, which is load-bearing: a name registered
/// twice resolves last-wins, and thunk addresses are assigned in
/// registration order. Do not reorder entries or call the parts out of
/// order.

#include "winapi/kernel32/kernel32_ext_registration_parts.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/thread/restore_context64.hpp"

#include "winapi/kernel32/synchronization/slist.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <cstdio>
#include <unistd.h>

namespace vitreo::winapi {

void append_wave_forwarder_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // ------------------------------------------------------------------------
    // Wave 1: sync/thread/proc-attr APIs
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "InterlockedPopEntrySList", 0,
        [&mgr](Marshaler& m){ mgr.impl_InterlockedPopEntrySList(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "InitOnceExecuteOnce", 0,
        [&mgr](Marshaler& m){ mgr.impl_InitOnceExecuteOnce(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "RegisterWaitForSingleObject", 0,
        [&mgr](Marshaler& m){ mgr.impl_RegisterWaitForSingleObject(m); }, 24 });
    exports.entries.push_back(ExportEntry{ "UnregisterWait", 0,
        [&mgr](Marshaler& m){ mgr.impl_UnregisterWait(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "UnregisterWaitEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_UnregisterWaitEx(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "CreateTimerQueue", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateTimerQueue(m); }, 0 });
    exports.entries.push_back(ExportEntry{ "CreateTimerQueueTimer", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateTimerQueueTimer(m); }, 28 });
    exports.entries.push_back(ExportEntry{ "ChangeTimerQueueTimer", 0,
        [&mgr](Marshaler& m){ mgr.impl_ChangeTimerQueueTimer(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "DeleteTimerQueueTimer", 0,
        [&mgr](Marshaler& m){ mgr.impl_DeleteTimerQueueTimer(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "DeleteTimerQueueEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_DeleteTimerQueueEx(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "DeleteTimerQueue", 0,
        [&mgr](Marshaler& m){ mgr.impl_DeleteTimerQueue(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetThreadId", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetThreadId(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetProcessIdOfThread", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetProcessIdOfThread(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetThreadTimes", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetThreadTimes(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "GetThreadContext", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetThreadContext(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "SetThreadContext", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetThreadContext(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "GetThreadGroupAffinity", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetThreadGroupAffinity(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "SetThreadGroupAffinity", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetThreadGroupAffinity(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetThreadInformation", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetThreadInformation(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "QueryThreadCycleTime", 0,
        [&mgr](Marshaler& m){ mgr.impl_QueryThreadCycleTime(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "QueryProcessCycleTime", 0,
        [&mgr](Marshaler& m){ mgr.impl_QueryProcessCycleTime(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "InitializeProcThreadAttributeList", 0,
        [&mgr](Marshaler& m){ mgr.impl_InitializeProcThreadAttributeList(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "UpdateProcThreadAttribute", 0,
        [&mgr](Marshaler& m){ mgr.impl_UpdateProcThreadAttribute(m); }, 28 });
    exports.entries.push_back(ExportEntry{ "DeleteProcThreadAttributeList", 0,
        [&mgr](Marshaler& m){ mgr.impl_DeleteProcThreadAttributeList(m); }, 4 });

    // ------------------------------------------------------------------------
    // Wave 3 registrations
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "CreateJobObjectA", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateJobObjectA(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "CreateJobObjectW", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateJobObjectW(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "AssignProcessToJobObject", 0,
        [&mgr](Marshaler& m){ mgr.impl_AssignProcessToJobObject(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "QueryInformationJobObject", 0,
        [&mgr](Marshaler& m){ mgr.impl_QueryInformationJobObject(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "SetInformationJobObject", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetInformationJobObject(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "TerminateJobObject", 0,
        [&mgr](Marshaler& m){ mgr.impl_TerminateJobObject(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "ReadProcessMemory", 0,
        [&mgr](Marshaler& m){ mgr.impl_ReadProcessMemory(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "WriteProcessMemory", 0,
        [&mgr](Marshaler& m){ mgr.impl_WriteProcessMemory(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "VirtualAllocEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_VirtualAllocEx(m); }, 20 });
    // LPVOID VirtualAllocExNuma(HANDLE hProcess, LPVOID lpAddress, SIZE_T
    //     dwSize, DWORD flAllocationType, DWORD flProtect, DWORD nndPreferred)
    //
    // Identical to VirtualAllocEx apart from a preferred NUMA node, which is
    // ADVISORY -- Microsoft Learn describes it as a preference, and a
    // single-node machine satisfies every request from node 0. So the correct
    // implementation is VirtualAllocEx with the node ignored, and the extra
    // parameter simply consumed.
    //
    // It was a missing-export stub returning 0xFFFFFFFF. A stub that returns
    // NON-NULL from an allocator is uniquely damaging: the caller believes it
    // holds a reservation that does not exist. coreclr's GC reserves through
    // this, then commits into the range -- and the commit failed with
    // "no reservation", which surfaced as a C++ exception, an unwind, and
    // Fatal error 0x80131506 (COR_E_EXECUTIONENGINE) 372,556 instructions
    // into PowerShell 7.
    exports.entries.push_back(ExportEntry{ "VirtualAllocExNuma", 0,
        [&mgr](Marshaler& m){ (void)m.arg(5); mgr.impl_VirtualAllocEx(m); }, 24 });
    exports.entries.push_back(ExportEntry{ "VirtualFreeEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_VirtualFreeEx(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "VirtualProtectEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_VirtualProtectEx(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "CreateRemoteThread", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateRemoteThread(m); }, 28 });
    exports.entries.push_back(ExportEntry{ "CreateRemoteThreadEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateRemoteThreadEx(m); }, 32 });
    exports.entries.push_back(ExportEntry{ "GetProcessHandleCount", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetProcessHandleCount(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "GetProcessWorkingSetSizeEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetProcessWorkingSetSizeEx(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "GetProcessWorkingSetSize", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetProcessWorkingSetSize(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetProcessWorkingSetSize", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetProcessWorkingSetSize(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetProcessWorkingSetSizeEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetProcessWorkingSetSizeEx(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "GetProcessMitigationPolicy", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetProcessMitigationPolicy(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "SetProcessMitigationPolicy", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetProcessMitigationPolicy(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "SetProcessDEPPolicy", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetProcessDEPPolicy(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "SetProcessInformation", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetProcessInformation(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "GetProcessInformation", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetProcessInformation(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "CheckRemoteDebuggerPresent", 0,
        [&mgr](Marshaler& m){ mgr.impl_CheckRemoteDebuggerPresent(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "AllocConsole", 0,
        [&mgr](Marshaler& m){ mgr.impl_AllocConsole(m); }, 0 });
    exports.entries.push_back(ExportEntry{ "FreeConsole", 0,
        [&mgr](Marshaler& m){ mgr.impl_FreeConsole(m); }, 0 });
    exports.entries.push_back(ExportEntry{ "GetConsoleDisplayMode", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetConsoleDisplayMode(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetConsoleWindow", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetConsoleWindow(m); }, 0 });
    exports.entries.push_back(ExportEntry{ "CreateSemaphoreExW", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateSemaphoreExW(m); }, 24 });
    exports.entries.push_back(ExportEntry{ "CreateSemaphoreExA", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateSemaphoreExA(m); }, 24 });
    exports.entries.push_back(ExportEntry{ "FindFirstFileExA", 0,
        [&mgr](Marshaler& m){ mgr.impl_FindFirstFileExA(m); }, 24 });
    exports.entries.push_back(ExportEntry{ "FindFirstVolumeW", 0,
        [&mgr](Marshaler& m){ mgr.impl_FindFirstVolumeW(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "FindNextVolumeW", 0,
        [&mgr](Marshaler& m){ mgr.impl_FindNextVolumeW(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "FindVolumeClose", 0,
        [&mgr](Marshaler& m){ mgr.impl_FindVolumeClose(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetFinalPathNameByHandleW", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetFinalPathNameByHandleW(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "GetFinalPathNameByHandleA", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetFinalPathNameByHandleA(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "GetFullPathNameA", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetFullPathNameA(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "GetTempFileNameA", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetTempFileNameA(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "QueryDosDeviceW", 0,
        [&mgr](Marshaler& m){ mgr.impl_QueryDosDeviceW(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "QueryDosDeviceA", 0,
        [&mgr](Marshaler& m){ mgr.impl_QueryDosDeviceA(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "LockFileEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_LockFileEx(m); }, 24 });
    exports.entries.push_back(ExportEntry{ "UnlockFileEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_UnlockFileEx(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "GetFirmwareType", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetFirmwareType(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetMaximumProcessorCount", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetMaximumProcessorCount(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetMaximumProcessorGroupCount", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetMaximumProcessorGroupCount(m); }, 0 });
    exports.entries.push_back(ExportEntry{ "GetActiveProcessorCount", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetActiveProcessorCount(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetActiveProcessorGroupCount", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetActiveProcessorGroupCount(m); }, 0 });
    exports.entries.push_back(ExportEntry{
        "GetPhysicallyInstalledSystemMemory", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_GetPhysicallyInstalledSystemMemory(m);
        }, 4 });
    exports.entries.push_back(ExportEntry{ "GetLogicalProcessorInformation", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetLogicalProcessorInformation(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "CreateMemoryResourceNotification", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateMemoryResourceNotification(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "QueryMemoryResourceNotification", 0,
        [&mgr](Marshaler& m){ mgr.impl_QueryMemoryResourceNotification(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "DiscardVirtualMemory", 0,
        [&mgr](Marshaler& m){ mgr.impl_DiscardVirtualMemory(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "PrefetchVirtualMemory", 0,
        [&mgr](Marshaler& m){ mgr.impl_PrefetchVirtualMemory(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "SetDefaultDllDirectories", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetDefaultDllDirectories(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "AddDllDirectory", 0,
        [&mgr](Marshaler& m){ mgr.impl_AddDllDirectory(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "RemoveDllDirectory", 0,
        [&mgr](Marshaler& m){ mgr.impl_RemoveDllDirectory(m); }, 4 });
    // HANDLE CreateNamedPipe{A,W}(LPCTSTR, DWORD, DWORD, DWORD, DWORD, DWORD,
    //                             DWORD, LPSECURITY_ATTRIBUTES) — 8 args,
    // stdcall_bytes = 32. Both forms are exported by kernel32; registering
    // only the wide one sent ANSI callers to the missing-export stub, which
    // returns 0xFFFFFFFF (== INVALID_HANDLE_VALUE) without setting a last
    // error, so the guest saw a failed create with GetLastError() == 0.
    exports.entries.push_back(ExportEntry{ "CreateNamedPipeA", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateNamedPipeA(m); }, 32 });
    exports.entries.push_back(ExportEntry{ "CreateNamedPipeW", 0,
        [&mgr](Marshaler& m){ mgr.impl_CreateNamedPipeW(m); }, 32 });
    exports.entries.push_back(ExportEntry{ "ConnectNamedPipe", 0,
        [&mgr](Marshaler& m){ mgr.impl_ConnectNamedPipe(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "DisconnectNamedPipe", 0,
        [&mgr](Marshaler& m){ mgr.impl_DisconnectNamedPipe(m); }, 4 });
    exports.entries.push_back(ExportEntry{ "GetNamedPipeClientProcessId", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetNamedPipeClientProcessId(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "GetNamedPipeServerProcessId", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetNamedPipeServerProcessId(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "ClearCommError", 0,
        [&mgr](Marshaler& m){ mgr.impl_ClearCommError(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "EscapeCommFunction", 0,
        [&mgr](Marshaler& m){ mgr.impl_EscapeCommFunction(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "GetCommModemStatus", 0,
        [&mgr](Marshaler& m){ mgr.impl_GetCommModemStatus(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "SetCommState", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetCommState(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "SetCommTimeouts", 0,
        [&mgr](Marshaler& m){ mgr.impl_SetCommTimeouts(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "K32EnumProcesses", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32EnumProcesses(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "K32GetModuleBaseNameA", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetModuleBaseNameA(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "K32GetModuleBaseNameW", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetModuleBaseNameW(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "K32EnumProcessModules", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32EnumProcessModules(m); }, 16 });
    // Learn (EnumProcessModulesEx): EnumProcessModules plus a filter selecting
    // 32-bit, 64-bit or all modules. Vitreo presents one module list per
    // process, and every module in it belongs to that process's own bitness,
    // so every filter value selects the same set — which is what the unfiltered
    // call already returns.
    exports.entries.push_back(ExportEntry{ "K32EnumProcessModulesEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32EnumProcessModules(m); }, 20 });
    exports.entries.push_back(ExportEntry{ "K32GetModuleFileNameExW", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetModuleFileNameExW(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "K32GetModuleInformation", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetModuleInformation(m); }, 16 });
    exports.entries.push_back(ExportEntry{ "K32GetPerformanceInfo", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetPerformanceInfo(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "K32GetProcessMemoryInfo", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetProcessMemoryInfo(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "K32GetProcessImageFileNameA", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetProcessImageFileName(m, false); }, 12 });
    exports.entries.push_back(ExportEntry{ "K32GetProcessImageFileNameW", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32GetProcessImageFileName(m, true); }, 12 });
    // BOOL K32EmptyWorkingSet(HANDLE hProcess)
    //
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-emptyworkingset
    // Equivalent to SetProcessWorkingSetSize(hProcess, -1, -1): a hint that
    // trims resident pages. Nothing about the guest's address space changes,
    // so there is no state to model — only the handle check is observable.
    exports.entries.push_back(ExportEntry{ "K32EmptyWorkingSet", 0,
        [](Marshaler& m){
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        }, 4 });
    exports.entries.push_back(ExportEntry{ "K32QueryWorkingSetEx", 0,
        [&mgr](Marshaler& m){ mgr.impl_K32QueryWorkingSetEx(m); }, 12 });
    exports.entries.push_back(ExportEntry{ "Module32FirstW", 0,
        [&mgr](Marshaler& m){ mgr.impl_Module32FirstW(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "Module32NextW", 0,
        [&mgr](Marshaler& m){ mgr.impl_Module32NextW(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "Process32FirstW", 0,
        [&mgr](Marshaler& m){ mgr.impl_Process32FirstW(m); }, 8 });
    exports.entries.push_back(ExportEntry{ "Process32NextW", 0,
        [&mgr](Marshaler& m){ mgr.impl_Process32NextW(m); }, 8 });

    // ------------------------------------------------------------------------
    // Wave 6: Power* (exposed from kernel32.dll in newer SDKs)
    // ------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{ "PowerCreateRequest", 0,
        [](Marshaler& m) {
            // A successful PowerCreateRequest must own an object releasable by
            // CloseHandle. Until host power assertions are supported, report
            // failure instead of manufacturing an unregistered handle.
            // https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-powercreaterequest
            std::fprintf(stderr, "[power] PowerCreateRequest: power requests are unsupported\n");
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(static_cast<u64>(-1));
        }, 4 });
    exports.entries.push_back(ExportEntry{ "PowerSetRequest", 0,
        [](Marshaler& m) {
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        }, 8 });
    exports.entries.push_back(ExportEntry{ "PowerClearRequest", 0,
        [](Marshaler& m) {
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        }, 8 });
}

} // namespace vitreo::winapi
