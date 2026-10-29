// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_handle_resource.cpp
/// @brief Extended kernel32 export table, part 3 of 9: handles, IOCP, resources, console, time zone.
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

#include <unistd.h>

namespace vitreo::winapi {

void append_handle_resource_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // DuplicateHandle (7 args)
    exports.entries.push_back(ExportEntry{
        "DuplicateHandle", 0,
        [&mgr](Marshaler& m) {
        // hTargetProcessHandle may be NULL with DUPLICATE_CLOSE_SOURCE
        // (Learn); impl_DuplicateHandle decides that case.
        if (m.arg(0) == 0 || m.arg(1) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_DuplicateHandle(m); },
        28
    });

    // SetHandleInformation (3 args)
    exports.entries.push_back(ExportEntry{
        "SetHandleInformation", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_SetHandleInformation(m); },
        12
    });

    // GetHandleInformation (2 args). A recipient verifies an adopted handle
    // with this API before wrapping it.
    exports.entries.push_back(ExportEntry{
        "GetHandleInformation", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetHandleInformation(m); },
        8
    });

    // FlushFileBuffers (1 arg)
    exports.entries.push_back(ExportEntry{
        "FlushFileBuffers", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_FlushFileBuffers(m); },
        4
    });

    // SetEndOfFile (1 arg)
    exports.entries.push_back(ExportEntry{
        "SetEndOfFile", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetEndOfFile(m); },
        4
    });

    // GetFileTime (4 args)
    exports.entries.push_back(ExportEntry{
        "GetFileTime", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_GetFileTime(m); },
        16
    });

    // GetFileInformationByHandle (2 args)
    exports.entries.push_back(ExportEntry{
        "GetFileInformationByHandle", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_GetFileInformationByHandle(m); },
        8
    });

    // FindFirstFileExW (6 args)
    exports.entries.push_back(ExportEntry{
        "FindFirstFileExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindFirstFileExW(m); },
        24
    });

    // DeviceIoControl (8 args)
    exports.entries.push_back(ExportEntry{
        "DeviceIoControl", 0,
        [&mgr](Marshaler& m) { mgr.impl_DeviceIoControl(m); },
        32
    });

    // CreateIoCompletionPort (4 args)
    exports.entries.push_back(ExportEntry{
        "CreateIoCompletionPort", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
            mgr.impl_CreateIoCompletionPort(m);
            },
        16
    });

    // PostQueuedCompletionStatus (4 args)
    exports.entries.push_back(ExportEntry{
        "PostQueuedCompletionStatus", 0,
        [&mgr](Marshaler& m) { mgr.impl_PostQueuedCompletionStatus(m); },
        16
    });

    // GetQueuedCompletionStatus (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "GetQueuedCompletionStatus", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetQueuedCompletionStatus(m); },
        20
    });

    // GetQueuedCompletionStatusEx (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "GetQueuedCompletionStatusEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetQueuedCompletionStatusEx(m); },
        24
    });

    // SetFileCompletionNotificationModes (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "SetFileCompletionNotificationModes", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileCompletionNotificationModes(m); },
        8
    });

    // FindResourceA (3 args)
    exports.entries.push_back(ExportEntry{
        "FindResourceA", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindResourceA(m); },
        12
    });

    // LoadResource (2 args)
    exports.entries.push_back(ExportEntry{
        "LoadResource", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadResource(m); },
        8
    });

    // LockResource (1 arg)
    exports.entries.push_back(ExportEntry{
        "LockResource", 0,
        [&mgr](Marshaler& m) { mgr.impl_LockResource(m); },
        4
    });

    // SizeofResource (2 args)
    exports.entries.push_back(ExportEntry{
        "SizeofResource", 0,
        [&mgr](Marshaler& m) { mgr.impl_SizeofResource(m); },
        8
    });

    // LoadStringW — Learn winuser.h; also exported from
    // api-ms-win-core-libraryloader (host kernel32). Missing export became
    // a fail-thunk returning 0xFFFFFFFF, which callers treat as a length.
    exports.entries.push_back(ExportEntry{
        "LoadStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadStringW(m); },
        16
    });

    // FindResourceW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "FindResourceW", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindResourceW(m); },
        12
    });

    // GetPrivateProfileIntW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetPrivateProfileIntW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPrivateProfileIntW(m); },
        16
    });

    // LoadLibraryExA (3 args)
    exports.entries.push_back(ExportEntry{
        "LoadLibraryExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadLibraryExA(m); },
        12
    });

    // GetModuleHandleExA (3 args)
    exports.entries.push_back(ExportEntry{
        "GetModuleHandleExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetModuleHandleExA(m); },
        12
    });

    // GetModuleHandleExW (3 args)
    exports.entries.push_back(ExportEntry{
        "GetModuleHandleExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetModuleHandleExW(m); },
        12
    });

    // IsBadWritePtr (2 args)
    exports.entries.push_back(ExportEntry{
        "IsBadWritePtr", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsBadWritePtr(m); },
        8
    });

    // IsBadReadPtr(const void* lp, UINT_PTR ucb) — Vitreo guest pointers passed
    // by real apps are valid; report readable (FALSE). (2 args)
    exports.entries.push_back(ExportEntry{
        "IsBadReadPtr", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); (void)m.arg(1); m.set_return(0); },
        8
    });

    // IsBadCodePtr(FARPROC lpfn) — report executable (FALSE). (1 arg)
    exports.entries.push_back(ExportEntry{
        "IsBadCodePtr", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); m.set_return(0); },
        4
    });

    // IsBadStringPtrA/W(lpsz, ucchMax) — report valid string (FALSE). (2 args)
    exports.entries.push_back(ExportEntry{
        "IsBadStringPtrA", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); (void)m.arg(1); m.set_return(0); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "IsBadStringPtrW", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); (void)m.arg(1); m.set_return(0); },
        8
    });

    // VirtualLock(lpAddress, dwSize) — pages are always resident here. TRUE. (2 args)
    exports.entries.push_back(ExportEntry{
        "VirtualLock", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); (void)m.arg(1); m.set_return(1); },
        8
    });

    // VirtualUnlock(lpAddress, dwSize) — TRUE. (2 args)
    exports.entries.push_back(ExportEntry{
        "VirtualUnlock", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); (void)m.arg(1); m.set_return(1); },
        8
    });

    // SetVolumeLabelA(lpRootPathName, lpVolumeName) — the capsule VFS has no
    // persistent volume label; accept the request. (2 args)
    exports.entries.push_back(ExportEntry{
        "SetVolumeLabelA", 0,
        [](Marshaler& m) { (void)m.arg_ptr(0); (void)m.arg_ptr(1); m.set_return(1); },
        8
    });

    // GlobalMemoryStatusEx (1 arg)
    exports.entries.push_back(ExportEntry{
        "GlobalMemoryStatusEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GlobalMemoryStatusEx(m); },
        4
    });

    // GetConsoleCP (0 args)
    exports.entries.push_back(ExportEntry{
        "GetConsoleCP", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetConsoleCP(m); },
        0
    });

    // ReadConsoleW (5 args)
    exports.entries.push_back(ExportEntry{
        "ReadConsoleW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_ReadConsoleW(m); },
        20
    });

    // SetConsoleCtrlHandler (2 args)
    exports.entries.push_back(ExportEntry{
        "SetConsoleCtrlHandler", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetConsoleCtrlHandler(m); },
        8
    });

    // SetStdHandle (2 args)
    exports.entries.push_back(ExportEntry{
        "SetStdHandle", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_SetStdHandle(m); },
        8
    });

    // GetCurrentDirectoryA (2 args)
    exports.entries.push_back(ExportEntry{
        "GetCurrentDirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentDirectoryA(m); },
        8
    });

    // RemoveDirectoryA (1 arg)
    exports.entries.push_back(ExportEntry{
        "RemoveDirectoryA", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_PATH_NOT_FOUND); m.set_return(0); return; }
        mgr.impl_RemoveDirectoryA(m); },
        4
    });

    // GetDriveTypeW (1 arg)
    exports.entries.push_back(ExportEntry{
        "GetDriveTypeW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDriveTypeW(m); },
        4
    });

    // GetDiskFreeSpaceA (5 args)
    exports.entries.push_back(ExportEntry{
        "GetDiskFreeSpaceA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDiskFreeSpaceA(m); },
        20
    });

    // GetDiskFreeSpaceExW (4 args)
    exports.entries.push_back(ExportEntry{
        "GetDiskFreeSpaceExW", 0,
        [&mgr](Marshaler& m) {
            // Real Windows treats NULL lpDirectoryName as the root of the
            // current drive and returns TRUE. The out pointers may be NULL
            // (caller doesn't care about them). Fall through to the impl,
            // which already accepts NULL path.
            mgr.impl_GetDiskFreeSpaceExW(m);
        },
        16
    });

    // GetTimeZoneInformation (1 arg)
    exports.entries.push_back(ExportEntry{
        "GetTimeZoneInformation", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetTimeZoneInformation(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetDynamicTimeZoneInformation", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDynamicTimeZoneInformation(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetTimeZoneInformationForYear", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetTimeZoneInformationForYear(m); },
        12
    });

    // LoadModule (2 args)
    exports.entries.push_back(ExportEntry{
        "LoadModule", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadModule(m); },
        8
    });

    // GetDateFormatA (6 args)
    exports.entries.push_back(ExportEntry{
        "GetDateFormatA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDateFormatA(m); },
        24
    });

    // GetDateFormatW (6 args)
    exports.entries.push_back(ExportEntry{
        "GetDateFormatW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDateFormatW(m); },
        24
    });

    // SystemTimeToTzSpecificLocalTime (3 args)
    exports.entries.push_back(ExportEntry{
        "SystemTimeToTzSpecificLocalTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_SystemTimeToTzSpecificLocalTime(m); },
        12
    });
    // TzSpecificLocalTimeToSystemTime — last registration wins over the
    // locale_string_registration stub (dll_registry merges ext after locale).
    exports.entries.push_back(ExportEntry{
        "TzSpecificLocalTimeToSystemTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_TzSpecificLocalTimeToSystemTime(m); },
        12
    });

    // HeapLock (1 arg)
    exports.entries.push_back(ExportEntry{
        "HeapLock", 0,
        [&mgr](Marshaler& m) { mgr.impl_HeapLock(m); },
        4
    });

    // HeapUnlock (1 arg)
    exports.entries.push_back(ExportEntry{
        "HeapUnlock", 0,
        [&mgr](Marshaler& m) { mgr.impl_HeapUnlock(m); },
        4
    });

    // HeapQueryInformation (5 args)
    exports.entries.push_back(ExportEntry{
        "HeapQueryInformation", 0,
        [&mgr](Marshaler& m) { mgr.impl_HeapQueryInformation(m); },
        20
    });

    // HeapWalk (2 args)
    exports.entries.push_back(ExportEntry{
        "HeapWalk", 0,
        [&mgr](Marshaler& m) { mgr.impl_HeapWalk(m); },
        8
    });

    // VerifyVersionInfoW (ptr + DWORD + DWORDLONG = 16 bytes)
    exports.entries.push_back(ExportEntry{
        "VerifyVersionInfoW", 0,
        [&mgr](Marshaler& m) { mgr.impl_VerifyVersionInfoW(m); },
        16
    });

    // VerifyVersionInfoA (ptr + DWORD + DWORDLONG = 16 bytes)
    exports.entries.push_back(ExportEntry{
        "VerifyVersionInfoA", 0,
        [&mgr](Marshaler& m) { mgr.impl_VerifyVersionInfoA(m); },
        16
    });

    // VerSetConditionMask (ULONGLONG + DWORD + BYTE-as-DWORD = 16 bytes)
    exports.entries.push_back(ExportEntry{
        "VerSetConditionMask", 0,
        // No early-out on a zero input mask: the very first call in a chain
        // passes ConditionMask=0, and it must still set the requested
        // condition bits rather than return 0.
        [&mgr](Marshaler& m) { mgr.impl_VerSetConditionMask(m); },
        16
    });

    // GetLogicalProcessorInformationEx (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetLogicalProcessorInformationEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetLogicalProcessorInformationEx(m); },
        12
    });
}

} // namespace vitreo::winapi
