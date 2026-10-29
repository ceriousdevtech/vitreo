// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_additional_win32.cpp
/// @brief Extended kernel32 export table, part 6 of 9: additional Win32 stubs.
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

#include <atomic>
#include <cerrno>
#include <string>
#include <unistd.h>

namespace vitreo::winapi {

void append_additional_win32_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // -------------------------------------------------------------------------
    // Additional Win32 stubs
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "InitializeConditionVariable", 0,
        [&mgr](Marshaler& m) { mgr.impl_InitializeConditionVariable(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "WakeConditionVariable", 0,
        [&mgr](Marshaler& m) { mgr.impl_WakeConditionVariable(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FreeLibraryAndExitThread", 0,
        [&mgr](Marshaler& m) { mgr.impl_FreeLibraryAndExitThread(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetProcessId", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_GetProcessId(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CreateProcessA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateProcessA(m); },
        40
    });
    exports.entries.push_back(ExportEntry{
        "CreateSemaphoreA", 0,
        // lpSemaphoreAttributes is optional; see CreateSemaphoreW above.
        [&mgr](Marshaler& m) { mgr.impl_CreateSemaphoreA(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "GetDriveTypeA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDriveTypeA(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetDiskFreeSpaceExA", 0,
        [&mgr](Marshaler& m) {
            // Real Windows treats NULL lpDirectoryName as the root of the
            // current drive and returns TRUE.
            mgr.impl_GetDiskFreeSpaceExA(m);
            },
        16
    });
    exports.entries.push_back(ExportEntry{
        "GetVolumeInformationA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetVolumeInformationA(m); },
        32
    });
    exports.entries.push_back(ExportEntry{
        "GetLogicalDriveStringsA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetLogicalDriveStringsA(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetVolumePathNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetVolumePathNameW(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetVolumeNameForVolumeMountPointW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetVolumeNameForVolumeMountPointW(m); },
        12
    });
    // MoveFileExA lives with MoveFileExW in file_ops (one implementation).
    exports.entries.push_back(ExportEntry{
        "GetPrivateProfileStringA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPrivateProfileStringA(m); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "GetPrivateProfileSectionA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPrivateProfileSectionA(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WritePrivateProfileSectionA", 0,
        [&mgr](Marshaler& m) { mgr.impl_WritePrivateProfileSectionA(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetPrivateProfileSectionNamesA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPrivateProfileSectionNamesA(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "K32GetModuleFileNameExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_K32GetModuleFileNameExA(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WTSGetActiveConsoleSessionId", 0,
        [&mgr](Marshaler& m) { mgr.impl_WTSGetActiveConsoleSessionId(m); },
        0
    });
    exports.entries.push_back(ExportEntry{
        "GetComputerNameExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetComputerNameExW(m); },
        12
    });
    // BOOL GetComputerNameExA(COMPUTER_NAME_FORMAT, LPSTR, LPDWORD)
    exports.entries.push_back(ExportEntry{
        "GetComputerNameExA", 0,
        [&mgr](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr buf = m.arg_ptr(1);
            GuestAddr size_ptr = m.arg_ptr(2);
            if (size_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            const u32 cch = m.memory().read_u32(size_ptr).value_or(0);
            constexpr const char kName[] = "VITREO";
            constexpr u32 kLen = 6;
            if (cch < kLen + 1 || buf == 0) {
                (void)m.memory().write_u32(size_ptr, kLen + 1);
                set_last_error(error_codes::ERROR_MORE_DATA);
                m.set_return(0u);
                return;
            }
            for (u32 i = 0; i < kLen; ++i) {
                (void)m.memory().write_u8(buf + i, static_cast<u8>(kName[i]));
            }
            (void)m.memory().write_u8(buf + kLen, 0);
            (void)m.memory().write_u32(size_ptr, kLen);
            m.set_return(1u);
            (void)mgr;
        },
        12
    });
    // BOOL IsProcessInJob(HANDLE, HANDLE, PBOOL)
    exports.entries.push_back(ExportEntry{
        "IsProcessInJob", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsProcessInJob(m); },
        12
    });
    // PTP_POOL CreateThreadpool(PVOID reserved) — an opaque pool handle.
    // Work objects accept initialized/default environs (Pool→Vitreo pool);
    // CleanupGroup still refused. Pool create/close and
    // min/max are no-ops beyond handle lifetime so IAT fail-thunks (0xFFFFFFFF)
    // are not left as fake PTP_POOL values.
    exports.entries.push_back(ExportEntry{
        "CreateThreadpool", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            static std::atomic<u32> next_pool{0x7C000004};
            const u32 pool = next_pool.fetch_add(4, std::memory_order_relaxed);
            m.set_return(pool);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "CloseThreadpool", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SetThreadpoolThreadMinimum", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            m.set_return(1);
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SetThreadpoolThreadMaximum", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
        },
        8
    });
    // Thread-pool timers, waits and callback-instance functions are
    // implemented with the work objects (thread/threadpool.cpp).
    exports.entries.push_back(ExportEntry{
        "FlushProcessWriteBuffers", 0,
        [](Marshaler& m) {
            // Learn: FlushProcessWriteBuffers — a process-wide barrier across
            // every thread of the process (see flush_process_write_buffers).
            (void)m;
            (void)flush_process_write_buffers();
        },
        0
    });
    exports.entries.push_back(ExportEntry{
        "SetThreadExecutionState", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadExecutionState(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SetProcessShutdownParameters", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetProcessShutdownParameters(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SetThreadErrorMode", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadErrorMode(m); },
        8
    });
    // GetThreadErrorMode (0 args) / GetErrorMode (0 args): the read halves of
    // the error-mode pair. .NET's DisableMediaInsertionPrompt (used by every
    // managed file-attribute query) pairs Set/GetThreadErrorMode.
    exports.entries.push_back(ExportEntry{
        "GetThreadErrorMode", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetThreadErrorMode(m); },
        0
    });
    exports.entries.push_back(ExportEntry{
        "GetErrorMode", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetErrorMode(m); },
        0
    });
    exports.entries.push_back(ExportEntry{
        "CancelIoEx", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_CancelIoEx(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetSystemPowerStatus", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemPowerStatus(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "AttachConsole", 0,
        [&mgr](Marshaler& m) { mgr.impl_AttachConsole(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GenerateConsoleCtrlEvent", 0,
        [&mgr](Marshaler& m) { mgr.impl_GenerateConsoleCtrlEvent(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "DebugActiveProcess", 0,
        [&mgr](Marshaler& m) { mgr.impl_DebugActiveProcess(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "DebugActiveProcessStop", 0,
        [&mgr](Marshaler& m) { mgr.impl_DebugActiveProcessStop(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "Module32First", 0,
        [&mgr](Marshaler& m) { mgr.impl_Module32First(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "Module32Next", 0,
        [&mgr](Marshaler& m) { mgr.impl_Module32Next(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetBinaryTypeW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetBinaryTypeW(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetBinaryTypeA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetBinaryTypeA(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "CreateHardLinkA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateHardLinkA(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CreateHardLinkW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateHardLinkW(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "ReadDirectoryChangesW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ReadDirectoryChangesW(m); },
        32
    });
    exports.entries.push_back(ExportEntry{
        "FindFirstChangeNotificationA", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindFirstChangeNotificationA(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "FindFirstChangeNotificationW", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindFirstChangeNotificationW(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "FindNextChangeNotification", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindNextChangeNotification(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FindCloseChangeNotification", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindCloseChangeNotification(m); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "EnumResourceNamesW", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumResourceNamesW(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "EnumResourceNamesA", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumResourceNamesA(m); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "AddVectoredExceptionHandler", 0,
        [&mgr](Marshaler& m) { mgr.impl_AddVectoredExceptionHandler(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "RemoveVectoredExceptionHandler", 0,
        [&mgr](Marshaler& m) { mgr.impl_RemoveVectoredExceptionHandler(m); },
        4
    });

    // GetLongPathNameW(lpszShortPath, lpszLongPath, cchBuffer) -> DWORD
    // Stub: just copy input to output (we don't shorten paths anyway).
    exports.entries.push_back(ExportEntry{
        "GetLongPathNameW", 0,
        [&mgr](Marshaler& m) {
            GuestAddr in_p  = m.arg_ptr(0);
            GuestAddr out_p = m.arg_ptr(1);
            u32 cch         = static_cast<u32>(m.arg(2));
            if (in_p == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            // Walk wide-char input to count length without conversion churn.
            u32 len = 0;
            while (true) {
                auto r = m.read_u16(in_p + len*2);
                if (!r.is_ok()) { set_last_error(error_codes::ERROR_PATH_NOT_FOUND); m.set_return(0); return; }
                if (r.value() == 0) break;
                ++len;
                if (len > 0x8000) { set_last_error(error_codes::ERROR_BUFFER_OVERFLOW); m.set_return(0); return; }
            }
            // GetLongPathName resolves an EXISTING path. A path that is not
            // there fails with ERROR_PATH_NOT_FOUND rather than echoing the
            // input back: callers use it to canonicalise a path they are
            // about to open, and a fabricated success sends them on to a
            // second failure carrying a misleading error.
            {
                std::string probe;
                probe.reserve(len);
                for (u32 i = 0; i < len; ++i) {
                    const u16 ch = m.read_u16(in_p + i * 2).value_or(0);
                    probe.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
                }
                if (mgr.vfs().stat(probe).is_err()) {
                    set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
                    m.set_return(0);
                    return;
                }
            }
            u32 need = len + 1; // includes NUL
            if (out_p == 0 || cch < need) {
                m.set_return(static_cast<u64>(need));
                return;
            }
            for (u32 i = 0; i < len; ++i) {
                auto r = m.read_u16(in_p + i*2);
                (void)m.write_u16(out_p + i*2, r.is_ok() ? r.value() : 0);
            }
            (void)m.write_u16(out_p + len*2, 0);
            m.set_return(static_cast<u64>(len));
        },
        12
    });

    // GetLongPathNameA(lpszShortPath, lpszLongPath, cchBuffer) -> DWORD
    // ANSI twin of GetLongPathNameW above: the capsule VFS does not generate
    // 8.3 aliases, so the long form of a path is the path itself.
    exports.entries.push_back(ExportEntry{
        "GetLongPathNameA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr in_p  = m.arg_ptr(0);
            GuestAddr out_p = m.arg_ptr(1);
            u32 cch         = static_cast<u32>(m.arg(2));
            if (in_p == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            u32 len = 0;
            while (true) {
                auto r = m.read_u8(in_p + len);
                if (!r.is_ok()) { set_last_error(error_codes::ERROR_PATH_NOT_FOUND); m.set_return(0); return; }
                if (r.value() == 0) break;
                ++len;
                if (len > 0x8000) { set_last_error(error_codes::ERROR_BUFFER_OVERFLOW); m.set_return(0); return; }
            }
            // GetLongPathName resolves an EXISTING path. A path that is not
            // there fails with ERROR_PATH_NOT_FOUND rather than echoing the
            // input back: callers use it to canonicalise a path they are
            // about to open, and a fabricated success sends them on to a
            // second failure carrying a misleading error.
            {
                std::string probe;
                probe.reserve(len);
                for (u32 i = 0; i < len; ++i) {
                    probe.push_back(
                        static_cast<char>(m.read_u8(in_p + i).value_or(0)));
                }
                if (mgr.vfs().stat(probe).is_err()) {
                    set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
                    m.set_return(0);
                    return;
                }
            }
            u32 need = len + 1; // includes NUL
            if (out_p == 0 || cch < need) {
                m.set_return(static_cast<u64>(need));
                return;
            }
            for (u32 i = 0; i < len; ++i) {
                auto r = m.read_u8(in_p + i);
                (void)m.write_u8(out_p + i, r.is_ok() ? r.value() : 0);
            }
            (void)m.write_u8(out_p + len, 0);
            m.set_return(static_cast<u64>(len));
        },
        12
    });

    // GetVolumePathNamesForVolumeNameW(VolumeName, VolumePaths, cch, ReturnLen) -> BOOL
    // Stub: report no paths; return FALSE with ERROR_MORE_DATA cleared.
    exports.entries.push_back(ExportEntry{
        "GetVolumePathNamesForVolumeNameW", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            GuestAddr paths = m.arg_ptr(1);
            (void)m.arg(2);
            GuestAddr ret_len = m.arg_ptr(3);
            if (ret_len) (void)m.write_u32(ret_len, 0);
            // Write an empty MULTI_SZ (two NUL terminators).
            if (paths) { (void)m.write_u16(paths, 0); (void)m.write_u16(paths + 2, 0); }
            m.set_return(1u); // pretend success with empty list
        },
        16
    });
}

} // namespace vitreo::winapi
