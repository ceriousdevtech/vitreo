// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_process_misc.cpp
/// @brief Extended kernel32 export table, part 5 of 9: CPU-Z stubs: times, locks, comm, Toolhelp.
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

void append_process_misc_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // =========================================================================
    // CPU-Z stubs
    // =========================================================================

    // SetPriorityClass (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SetPriorityClass", 0,
        [&mgr](Marshaler& m) {
        // NULL → ERROR_INVALID_HANDLE (process_contract_probe.expected;
        // Learn treats an invalid hProcess as failure with that class of error).
        if (m.arg(0) == 0) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        mgr.impl_SetPriorityClass(m); },
        8
    });
    // GetPriorityClass (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetPriorityClass", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            mgr.impl_GetPriorityClass(m);
            },
        4
    });

    // GetProcessTimes (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "GetProcessTimes", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_GetProcessTimes(m); },
        20
    });

    // GetSystemTimes (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetSystemTimes", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemTimes(m); },
        12
    });

    // GetProfileIntA/W (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetProfileIntA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetProfileIntA(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetProfileIntW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetProfileIntW(m); },
        12
    });

    // ConvertDefaultLocale (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "ConvertDefaultLocale", 0,
        [&mgr](Marshaler& m) { mgr.impl_ConvertDefaultLocale(m); },
        4
    });

    // FindResourceExA (4 args, stdcall_bytes=16).
    // Previously unregistered, so it fell through to the generic
    // missing-export stub whose stdcall_bytes=0 leaks the 4 args off the stack.
    exports.entries.push_back(ExportEntry{
        "FindResourceExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindResourceExA(m); },
        16
    });

    // FindResourceExW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "FindResourceExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindResourceExW(m); },
        16
    });

    // EnumResourceLanguagesA/W (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "EnumResourceLanguagesA", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(1) == 0 && m.arg(2) == 0) { set_last_error(0x715u /*ERROR_RESOURCE_TYPE_NOT_FOUND*/); m.set_return(0); return; }
        mgr.impl_EnumResourceLanguagesA(m); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "EnumResourceLanguagesW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(1) == 0 && m.arg(2) == 0) { set_last_error(0x715u /*ERROR_RESOURCE_TYPE_NOT_FOUND*/); m.set_return(0); return; }
        mgr.impl_EnumResourceLanguagesW(m); },
        20
    });

    // LockFile (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "LockFile", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_LockFile(m); },
        20
    });

    // UnlockFile (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "UnlockFile", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_UnlockFile(m); },
        20
    });

    // GlobalAddAtomW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalAddAtomW", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {  m.set_return(0); return; }
            mgr.impl_GlobalAddAtomW(m);
            },
        4
    });

    // GlobalFindAtomW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalFindAtomW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GlobalFindAtomW(m); },
        4
    });

    // GlobalDeleteAtom (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalDeleteAtom", 0,
        [&mgr](Marshaler& m) { mgr.impl_GlobalDeleteAtom(m); },
        4
    });

    // GlobalGetAtomNameW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GlobalGetAtomNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GlobalGetAtomNameW(m); },
        12
    });

    // PurgeComm (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "PurgeComm", 0,
        [&mgr](Marshaler& m) { mgr.impl_PurgeComm(m); },
        8
    });

    // GetCommState (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetCommState", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCommState(m); },
        8
    });

    // GetCommTimeouts (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetCommTimeouts", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCommTimeouts(m); },
        8
    });

    // GetOverlappedResult (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetOverlappedResult", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetOverlappedResult(m); },
        16
    });
    // BOOL GetOverlappedResultEx(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL)
    exports.entries.push_back(ExportEntry{
        "GetOverlappedResultEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetOverlappedResultEx(m); },
        20
    });

    // CancelIo (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "CancelIo", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_CancelIo(m); },
        4
    });

    // SetDllDirectoryA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetDllDirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetDllDirectoryA(m); },
        4
    });

    // CreateDirectoryA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "CreateDirectoryA", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_PATH_NOT_FOUND); m.set_return(0); return; }
        mgr.impl_CreateDirectoryA(m); },
        8
    });

    // GetTempPathA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetTempPathA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetTempPathA(m); },
        8
    });

    // DeleteFileA is implemented in file_ops.cpp against the VFS; the copy
    // that used to live here returned TRUE without deleting anything.

    // GetWindowsDirectoryA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetWindowsDirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowsDirectoryA(m); },
        8
    });

    // GetSystemDirectoryA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetSystemDirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemDirectoryA(m); },
        8
    });

    // ReleaseSemaphore (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "ReleaseSemaphore", 0,
        [&mgr](Marshaler& m) {
        // Learn / handles_waits_probe: NULL hSemaphore → ERROR_INVALID_HANDLE.
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_ReleaseSemaphore(m); },
        12
    });

    // CreateSemaphoreW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "CreateSemaphoreW", 0,
        // arg0 is lpSemaphoreAttributes, which is OPTIONAL — NULL is the
        // overwhelmingly common call. Do not reject it.
        [&mgr](Marshaler& m) { mgr.impl_CreateSemaphoreW(m); },
        16
    });

    // QueueUserAPC (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "QueueUserAPC", 0,
        [&mgr](Marshaler& m) {
        mgr.impl_QueueUserAPC(m); },
        12
    });

    // CreateToolhelp32Snapshot (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "CreateToolhelp32Snapshot", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateToolhelp32Snapshot(m); },
        8
    });

    // Process32First (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "Process32First", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_BAD_LENGTH); m.set_return(0); return; }
        mgr.impl_Process32First(m); },
        8
    });

    // Process32Next (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "Process32Next", 0,
        [&mgr](Marshaler& m) { mgr.impl_Process32Next(m); },
        8
    });

    // Thread32First/Next. A fail-thunk returns 0xFFFFFFFF and breaks a
    // Toolhelp thread walk.
    exports.entries.push_back(ExportEntry{
        "Thread32First", 0,
        [&mgr](Marshaler& m) { mgr.impl_Thread32First(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "Thread32Next", 0,
        [&mgr](Marshaler& m) { mgr.impl_Thread32Next(m); },
        8
    });

    // InterlockedExchangeAdd (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "InterlockedExchangeAdd", 0,
        [&mgr](Marshaler& m) { mgr.impl_InterlockedExchangeAdd(m); },
        8
    });

    // GlobalMemoryStatus (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GlobalMemoryStatus", 0,
        [&mgr](Marshaler& m) { mgr.impl_GlobalMemoryStatus(m); },
        4
    });

    // GetProductInfo (5 args, stdcall_bytes=20)
    // BOOL GetProductInfo(DWORD dwOSMajorVersion, DWORD dwOSMinorVersion,
    //   DWORD dwSpMajorVersion, DWORD dwSpMinorVersion, PDWORD pdwReturnedProductType)
    exports.entries.push_back(ExportEntry{
        "GetProductInfo", 0,
        [](Marshaler& m) {
            u32 maj = static_cast<u32>(m.arg(0));
            u32 min = static_cast<u32>(m.arg(1));
            (void)m.arg(2); (void)m.arg(3);
            GuestAddr out_ptr = m.arg_ptr(4);
            // Real Windows fails when major version < 6 or out_ptr is NULL.
            if (maj < 6 || out_ptr == 0) {
                if (out_ptr != 0) (void)m.write_u32(out_ptr, 0u); // PRODUCT_UNDEFINED
                m.set_return(0);
                return;
            }
            (void)min;
            // PRODUCT_PROFESSIONAL = 0x30
            (void)m.write_u32(out_ptr, 0x30u);
            VITREO_TRACE("[kernel32_ext] GetProductInfo -> TRUE (PRODUCT_PROFESSIONAL)\n");
            m.set_return(1); // TRUE
        },
        20
    });

    // RtlCaptureStackBackTrace (4 args, stdcall_bytes=16)
    // USHORT RtlCaptureStackBackTrace(ULONG FramesToSkip, ULONG FramesToCapture,
    //   PVOID* BackTrace, PULONG BackTraceHash)
    exports.entries.push_back(ExportEntry{
        "RtlCaptureStackBackTrace", 0,
        [](Marshaler& m) { impl_RtlCaptureStackBackTrace(m); },
        16
    });

    // WaitNamedPipeA / WaitNamedPipeW (2 args, stdcall_bytes=8)
    // BOOL WaitNamedPipe*(LPCTSTR lpNamedPipeName, DWORD nTimeOut)
    exports.entries.push_back(ExportEntry{
        "WaitNamedPipeA", 0,
        [&mgr](Marshaler& m) { mgr.impl_WaitNamedPipeA(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "WaitNamedPipeW", 0,
        [&mgr](Marshaler& m) { mgr.impl_WaitNamedPipeW(m); },
        8
    });

    // CallNamedPipeA / CallNamedPipeW (7 args, stdcall_bytes=28)
    // BOOL CallNamedPipe*(LPCTSTR, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, DWORD)
    exports.entries.push_back(ExportEntry{
        "CallNamedPipeA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallNamedPipeA(m); },
        28
    });
    exports.entries.push_back(ExportEntry{
        "CallNamedPipeW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallNamedPipeW(m); },
        28
    });

    // TransactNamedPipe (7 args, stdcall_bytes=28)
    // BOOL TransactNamedPipe(HANDLE, LPVOID lpInBuffer, DWORD nInBufferSize,
    //   LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesRead,
    //   LPOVERLAPPED lpOverlapped)
    exports.entries.push_back(ExportEntry{
        "TransactNamedPipe", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_TransactNamedPipe(m);
        },
        28
    });

    // RegisterApplicationRestart (2 args, stdcall_bytes=8)
    // HRESULT RegisterApplicationRestart(PCWSTR pwzCommandline, DWORD dwFlags)
    //
    // Registering it matters mainly for the ABI. Falling through to the generic
    // missing-export stub returned 0xFFFFFFFF *and* cleaned up zero bytes,
    // because that stub cannot know a missing function's stdcall argument size.
    // For this two-argument stdcall that leaks 8 bytes of the caller's stack on
    // every call.
    //
    // Vitreo has no Windows Restart Manager, so the registration's actual
    // side effect (relaunch this process after a crash or update) does not
    // happen and the call must not claim success. Parameter validation still
    // mirrors Windows so callers that probe for E_INVALIDARG see it.
    exports.entries.push_back(ExportEntry{
        "RegisterApplicationRestart", 0,
        [](Marshaler& m) {
            constexpr u32 RESTART_MAX_CMD_LINE = 1024;
            // RESTART_NO_CRASH | NO_HANG | NO_PATCH | NO_REBOOT
            constexpr u32 VALID_FLAGS = 0xF;
            constexpr u32 E_INVALIDARG_HR = 0x80070057;
            constexpr u32 E_FAIL_HR = 0x80004005;

            const GuestAddr cmdline_ptr = m.arg_ptr(0);
            const u32 flags = static_cast<u32>(m.arg(1));

            if ((flags & ~VALID_FLAGS) != 0) {
                m.set_return(E_INVALIDARG_HR);
                return;
            }
            if (cmdline_ptr != 0) {
                auto text = m.read_wide_string(cmdline_ptr);
                if (text.is_ok() && text.value().size() > RESTART_MAX_CMD_LINE) {
                    m.set_return(E_INVALIDARG_HR);
                    return;
                }
            }
            VITREO_TRACE("[k32ext] RegisterApplicationRestart(flags=0x%x) -> "
                         "E_FAIL (no restart manager)\n", flags);
            m.set_return(E_FAIL_HR);
        },
        8
    });

    // SetNamedPipeHandleState (4 args, stdcall_bytes=16)
    // BOOL SetNamedPipeHandleState(HANDLE hNamedPipe, LPDWORD lpMode,
    //   LPDWORD lpMaxCollectionCount, LPDWORD lpCollectDataTimeout)
    // Stub: accept without doing anything.
    exports.entries.push_back(ExportEntry{
        "SetNamedPipeHandleState", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetNamedPipeHandleState(m); },
        16
    });

    // GetNamedPipeHandleStateW (7 args, stdcall_bytes=28)
    exports.entries.push_back(ExportEntry{
        "GetNamedPipeHandleStateW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetNamedPipeHandleStateW(m); },
        28
    });

    // GetNamedPipeInfo (5 args, stdcall_bytes=20)
    // BOOL GetNamedPipeInfo(HANDLE hNamedPipe, LPDWORD lpFlags,
    //   LPDWORD lpOutBufferSize, LPDWORD lpInBufferSize,
    //   LPDWORD lpMaxInstances)
    exports.entries.push_back(ExportEntry{
        "GetNamedPipeInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetNamedPipeInfo(m); },
        20
    });

    // InterlockedFlushSList (1 arg, stdcall_bytes=4)
    // PSLIST_ENTRY InterlockedFlushSList(PSLIST_HEADER ListHead)
    // Atomically remove all entries and return the old head.
    exports.entries.push_back(ExportEntry{
        "InterlockedFlushSList", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr head = m.arg_ptr(0);
            m.set_return(head == 0 ? 0
                                   : slist::flush(mgr.memory(), head, m.cpu_is_64bit()));
        },
        4
    });
    // PSLIST_ENTRY InterlockedPushListSListEx(PSLIST_HEADER, PSLIST_ENTRY List,
    //                                         PSLIST_ENTRY ListEnd, ULONG Count)
    exports.entries.push_back(ExportEntry{
        "InterlockedPushListSListEx", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr head = m.arg_ptr(0);
            const GuestAddr list = m.arg_ptr(1);
            const GuestAddr list_end = m.arg_ptr(2);
            const u32 count = static_cast<u32>(m.arg(3));
            if (head == 0 || list == 0 || list_end == 0) {
                m.set_return(0);
                return;
            }
            m.set_return(slist::push_list(mgr.memory(), head, list, list_end,
                                          count, m.cpu_is_64bit()));
        },
        16
    });
    // InterlockedPushListSList: same contract. On x86 it is __fastcall, which
    // this export path does not marshal, so that bitness fails loudly.
    exports.entries.push_back(ExportEntry{
        "InterlockedPushListSList", 0,
        [&mgr](Marshaler& m) {
            if (!m.cpu_is_64bit()) {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!InterlockedPushListSList: "
                                     "x86 __fastcall entry\n");
                m.set_return(0);
                return;
            }
            const GuestAddr head = m.arg_ptr(0);
            const GuestAddr list = m.arg_ptr(1);
            const GuestAddr list_end = m.arg_ptr(2);
            const u32 count = static_cast<u32>(m.arg(3));
            if (head == 0 || list == 0 || list_end == 0) {
                m.set_return(0);
                return;
            }
            m.set_return(slist::push_list(mgr.memory(), head, list, list_end,
                                          count, true));
        },
        0
    });

    // VirtualQueryEx (4 args, stdcall_bytes=16)
    // SIZE_T VirtualQueryEx(HANDLE hProcess, LPCVOID lpAddress,
    //   PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength)
    exports.entries.push_back(ExportEntry{
        "VirtualQueryEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_VirtualQueryEx(m); },
        16
    });
}

} // namespace vitreo::winapi
