// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_unwind_path.cpp
/// @brief Extended kernel32 export table, part 1 of 9: error mode, unwind helpers, file copy, paths.
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
#include "winapi/kernel32/thread/unwind64.hpp"
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

#include <cstdlib>
#include <unistd.h>

namespace vitreo::winapi {

void append_unwind_path_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // SetErrorMode (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetErrorMode", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetErrorMode(m); },
        4
    });

    // CompareFileTime (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "CompareFileTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_CompareFileTime(m); },
        8
    });

    // RtlCaptureContext (1 arg, x64)
    exports.entries.push_back(ExportEntry{
        "RtlCaptureContext", 0,
        [](Marshaler& m) {
            auto* cpu = m.cpu();
            u64 ret_addr = 0;
            if (cpu != nullptr) {
                auto ret_addr_result = m.read_u64(cpu->gpr.rsp);
                if (ret_addr_result.is_ok()) {
                    ret_addr = ret_addr_result.value();
                }
            }
            VITREO_TRACE(
                "[kernel32_ext] RtlCaptureContext(ctx=0x%llx, thunk_rip=0x%llx, ret=0x%llx)\n",
                static_cast<unsigned long long>(m.arg_ptr(0)),
                static_cast<unsigned long long>(cpu != nullptr ? cpu->rip : 0),
                static_cast<unsigned long long>(ret_addr));
            write_context64(m, m.arg_ptr(0));
            m.set_return(0);
        },
        0
    });

    // RtlLookupFunctionEntry (3 args, x64)
    exports.entries.push_back(ExportEntry{
        "RtlLookupFunctionEntry", 0,
        [&mgr](Marshaler& m) {
            GuestAddr control_pc = static_cast<GuestAddr>(m.arg(0));
            GuestAddr image_base_out = m.arg_ptr(1);
            GuestAddr image_base = 0;
            GuestAddr runtime_function = 0;

            if (mgr.module_manager() != nullptr) {
                runtime_function = mgr.module_manager()->lookup_runtime_function_entry(
                    control_pc, &image_base);
            }

            if (image_base_out != 0) {
                (void)mgr.memory().write_u64(image_base_out, image_base);
            }
            VITREO_TRACE(
                "[kernel32_ext] RtlLookupFunctionEntry(pc=0x%llx) -> func=0x%llx image_base=0x%llx\n",
                static_cast<unsigned long long>(control_pc),
                static_cast<unsigned long long>(runtime_function),
                static_cast<unsigned long long>(image_base));
            m.set_return(runtime_function);
        },
        0
    });

    // RtlVirtualUnwind (8 args, x64)
    exports.entries.push_back(ExportEntry{
        "RtlVirtualUnwind", 0,
        [](Marshaler& m) {
            GuestAddr handler_data = m.arg_ptr(5);
            GuestAddr establisher_frame = m.arg_ptr(6);
            GuestAddr exception_handler = 0;
            VITREO_TRACE(
                "[kernel32_ext] RtlVirtualUnwind(handler_type=0x%llx, image_base=0x%llx, control_pc=0x%llx, func=0x%llx, ctx=0x%llx)\n",
                static_cast<unsigned long long>(m.arg(0)),
                static_cast<unsigned long long>(m.arg(1)),
                static_cast<unsigned long long>(m.arg(2)),
                static_cast<unsigned long long>(m.arg(3)),
                static_cast<unsigned long long>(m.arg(4)));

            const bool ok = unwind_context64(
                m,
                m.arg_ptr(1),
                m.arg_ptr(2),
                m.arg_ptr(3),
                m.arg_ptr(4),
                handler_data,
                establisher_frame,
                &exception_handler,
                // HandlerType: only a handler of the requested kind is
                // returned (UNW_FLAG_NHANDLER asks for none).
                static_cast<u32>(m.arg(0)));

            if (!ok) {
                if (handler_data != 0) {
                    (void)m.write_u64(handler_data, 0);
                }
                if (establisher_frame != 0 && m.has_cpu()) {
                    (void)m.write_u64(establisher_frame, m.cpu()->gpr.rsp);
                }
                exception_handler = 0;
            }
            m.set_return(exception_handler);
        },
        0
    });

    // RtlPcToFileHeader (2 args, x64) — Microsoft Learn winnt.h: return the
    // base of the mapped image that contains PcValue, or NULL if none does.
    // Always returning the process EXE made x64 MSVC throws from a DLL
    // publish the wrong ExceptionInformation[3] image base, so every
    // personality ContinueSearch'd.
    exports.entries.push_back(ExportEntry{
        "RtlPcToFileHeader", 0,
        [&mgr](Marshaler& m) {
            GuestAddr base = 0;
            if (mgr.module_manager() != nullptr) {
                const auto found = mgr.module_manager()->module_for_address(
                    static_cast<GuestAddr>(m.arg(0)));
                if (found.info != nullptr) {
                    base = found.info->base_address;
                }
            }
            GuestAddr base_out = m.arg_ptr(1);
            if (base_out != 0) {
                (void)mgr.memory().write_u64(base_out, base);
            }
            m.set_return(base);
        },
        0
    });

    // RtlUnwindEx (6 args, x64): the one x64 unwinder, shared with ntdll's
    // export and kernel32!RtlUnwind (kernel32/thread/unwind64.cpp).
    exports.entries.push_back(ExportEntry{
        "RtlUnwindEx", 0,
        [&mgr](Marshaler& m) {
            rtl_unwind_export64(m, mgr.module_manager(), mgr.process_manager(),
                                /*is_ex=*/true);
        },
        0
    });

    // CopyFileW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "CopyFileA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyFileA(m); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "CopyFileW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyFileW(m); },
        12
    });
    // BOOL CopyFileEx(LPCTSTR, LPCTSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD)
    exports.entries.push_back(ExportEntry{
        "CopyFileExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyFileExA(m); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "CopyFileExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyFileExW(m); },
        24
    });
    // CopyFileTransactedW (7 args, stdcall_bytes=28)
    // BOOL CopyFileTransactedW(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID,
    //   LPBOOL pbCancel, DWORD dwCopyFlags, HANDLE hTransaction)
    // Prefix-compatible with CopyFileExW; transaction handle ignored.
    // (Moved here with CopyFileExW; behaviour unchanged.)
    exports.entries.push_back(ExportEntry{
        "CopyFileTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyFileExW(m); },
        28
    });
    // HRESULT CopyFile2(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*)
    exports.entries.push_back(ExportEntry{
        "CopyFile2", 0,
        [&mgr](Marshaler& m) { mgr.impl_CopyFile2(m); },
        12
    });

    // CreateProcessW (10 args, stdcall_bytes=40)
    exports.entries.push_back(ExportEntry{
        "CreateProcessW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreateProcessW(m); },
        40
    });

    // ExpandEnvironmentStringsA (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "ExpandEnvironmentStringsA", 0,
        [&mgr](Marshaler& m) { mgr.impl_ExpandEnvironmentStringsA(m); },
        12
    });

    // ExpandEnvironmentStringsW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "ExpandEnvironmentStringsW", 0,
        [&mgr](Marshaler& m) { mgr.impl_ExpandEnvironmentStringsW(m); },
        12
    });

    // GetDiskFreeSpaceW (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "GetDiskFreeSpaceW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDiskFreeSpaceW(m); },
        20
    });

    // GetExitCodeProcess (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetExitCodeProcess", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_GetExitCodeProcess(m); },
        8
    });

    // GetFullPathNameW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetFullPathNameW", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_NAME); m.set_return(0); return; }
            mgr.impl_GetFullPathNameW(m);
            },
        16
    });

    // GetProfileStringA/W (5 args, stdcall_bytes=20)
    exports.entries.push_back(ExportEntry{
        "GetProfileStringA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetProfileStringA(m); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "GetProfileStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetProfileStringW(m); },
        20
    });

    // GetPrivateProfileStringW (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "GetPrivateProfileStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPrivateProfileStringW(m); },
        24
    });

    // GetShortPathNameW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetShortPathNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetShortPathNameW(m); },
        12
    });

    // GetShortPathNameA (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetShortPathNameA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetShortPathNameA(m); },
        12
    });

    // GetTempFileNameW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetTempFileNameW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetTempFileNameW(m); },
        16
    });

    // GetTempPathW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetTempPathW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetTempPathW(m); },
        8
    });

    // GetWindowsDirectoryW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetWindowsDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowsDirectoryW(m); },
        8
    });

    // GetSystemWindowsDirectoryW / ...A (2 args, stdcall_bytes=8)
    //
    // Learn (GetSystemWindowsDirectory): this returns the *shared* Windows
    // directory on a multi-user Terminal Services system; "on a system that is
    // not running Terminal Services, this is the same as GetWindowsDirectory",
    // which is the only shape a capsule has. Same signature, same answer.
    //
    // Leaving it unimplemented is not benign. The missing-export stub answers
    // 0xFFFFFFFF, which a caller reads as a returned character count and then
    // uses to index or copy from an untouched buffer.
    exports.entries.push_back(ExportEntry{
        "GetSystemWindowsDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowsDirectoryW(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetSystemWindowsDirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowsDirectoryA(m); },
        8
    });

    // GetSystemWow64DirectoryW/A (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetSystemWow64DirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemWow64DirectoryW(m); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetSystemWow64DirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemWow64DirectoryA(m); },
        8
    });

    // MoveFileW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "MoveFileW", 0,
        [&mgr](Marshaler& m) { mgr.impl_MoveFileW(m); },
        8
    });

    // MoveFileA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "MoveFileA", 0,
        [&mgr](Marshaler& m) { mgr.impl_MoveFileA(m); },
        8
    });

    // MulDiv (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "MulDiv", 0,
        [&mgr](Marshaler& m) { mgr.impl_MulDiv(m); },
        12
    });

    // RemoveDirectoryW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "RemoveDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_RemoveDirectoryW(m); },
        4
    });

    // RemoveDirectoryTransactedW (2 args, stdcall_bytes=8)
    // BOOL RemoveDirectoryTransactedW(LPCWSTR lpPathName, HANDLE hTransaction)
    // Prefix-compatible with RemoveDirectoryW; transaction ignored.
    exports.entries.push_back(ExportEntry{
        "RemoveDirectoryTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_RemoveDirectoryW(m); },
        8
    });

    // FindFirstFileTransactedW (7 args, stdcall_bytes=28)
    // HANDLE FindFirstFileTransactedW(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID,
    //   FINDEX_SEARCH_OPS, LPVOID, DWORD dwAdditionalFlags, HANDLE hTransaction)
    // Prefix-compatible with FindFirstFileExW.
    exports.entries.push_back(ExportEntry{
        "FindFirstFileTransactedW", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindFirstFileExW(m); },
        28
    });

    // SearchPathW (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "SearchPathW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SearchPathW(m); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "SearchPathA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SearchPathA(m); },
        24
    });
    exports.entries.push_back(ExportEntry{
        "SetSearchPathMode", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetSearchPathMode(m); },
        4
    });

    // GetCurrentDirectoryW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetCurrentDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentDirectoryW(m); },
        8
    });

    // SetCurrentDirectoryW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetCurrentDirectoryW", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_SetCurrentDirectoryW(m); },
        4
    });

    // SetCurrentDirectoryA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "SetCurrentDirectoryA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetCurrentDirectoryA(m); },
        4
    });

    // WritePrivateProfileStringW (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "WritePrivateProfileStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_WritePrivateProfileStringW(m); },
        16
    });

    // lstrcmpW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcmpW", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcmpW(m); },
        8
    });

    // lstrcmpiA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcmpiA", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcmpiA(m); },
        8
    });

    // lstrcmpiW (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcmpiW", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcmpiW(m); },
        8
    });

    // lstrcpyA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcpyA", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcpyA(m); },
        8
    });

    // lstrcpynW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "lstrcpynW", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrcpynW(m); },
        12
    });

    // lstrlenA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "lstrlenA", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrlenA(m); },
        4
    });
    // Win9x GetProcAddress("lstrlen") — undecorated alias of lstrlenA.
    exports.entries.push_back(ExportEntry{
        "lstrlen", 0,
        [&mgr](Marshaler& m) { mgr.impl_lstrlenA(m); },
        4
    });
}

} // namespace vitreo::winapi
