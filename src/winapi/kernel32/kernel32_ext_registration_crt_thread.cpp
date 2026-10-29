// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_crt_thread.cpp
/// @brief Extended kernel32 export table, part 2 of 9: MSVC CRT startup stubs, SList/SRW, threads.
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

#include <pthread.h>
#include "winapi/kernel32/synchronization/slist.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <unistd.h>

namespace vitreo::winapi {

void append_crt_thread_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // =====================================================================
    // MSVC CRT startup stubs
    // =====================================================================

    // EncodePointer (1 arg)
    exports.entries.push_back(ExportEntry{
        "EncodePointer", 0,
        [&mgr](Marshaler& m) { mgr.impl_EncodePointer(m); },
        4
    });

    // DecodePointer (1 arg)
    exports.entries.push_back(ExportEntry{
        "DecodePointer", 0,
        [&mgr](Marshaler& m) { mgr.impl_DecodePointer(m); },
        4
    });

    // IsValidCodePage (1 arg)
    exports.entries.push_back(ExportEntry{
        "IsValidCodePage", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_IsValidCodePage(m); },
        4
    });

    // GetCPInfo (2 args)
    exports.entries.push_back(ExportEntry{
        "GetCPInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCPInfo(m); },
        8
    });

    // GetStringTypeW (4 args)
    exports.entries.push_back(ExportEntry{
        "GetStringTypeW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetStringTypeW(m); },
        16
    });

    // LCMapStringW (6 args)
    exports.entries.push_back(ExportEntry{
        "LCMapStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_LCMapStringW(m); },
        24
    });

    // RtlUnwind (4 args)
    exports.entries.push_back(ExportEntry{
        "RtlUnwind", 0,
        [&mgr](Marshaler& m) { mgr.impl_RtlUnwind(m); },
        16
    });

    // RaiseException (4 args)
    exports.entries.push_back(ExportEntry{
        "RaiseException", 0,
        [&mgr](Marshaler& m) { mgr.impl_RaiseException(m); },
        16
    });

    // InitializeSListHead (1 arg)
    exports.entries.push_back(ExportEntry{
        "InitializeSListHead", 0,
        [&mgr](Marshaler& m) { mgr.impl_InitializeSListHead(m); },
        4
    });

    // InterlockedPushEntrySList (2 args)
    exports.entries.push_back(ExportEntry{
        "InterlockedPushEntrySList", 0,
        [&mgr](Marshaler& m) { mgr.impl_InterlockedPushEntrySList(m); },
        8
    });

    // USHORT QueryDepthSList(PSLIST_HEADER ListHead)
    // https://learn.microsoft.com/en-us/windows/win32/api/interlockedapi/nf-interlockedapi-querydepthslist
    // Depth is a 16-bit field in SLIST_HEADER (offset 4 on x86, low 16 bits on x64).
    exports.entries.push_back(ExportEntry{
        "QueryDepthSList", 0,
        [](Marshaler& m) {
            const GuestAddr head = m.arg_ptr(0);
            if (head == 0) {
                m.set_return(0);
                return;
            }
            m.set_return(slist::depth(m.memory(), head, m.cpu_is_64bit()));
        },
        4
    });

    // InitializeSRWLock (1 arg)
    exports.entries.push_back(ExportEntry{
        "InitializeSRWLock", 0,
        [&mgr](Marshaler& m) { mgr.impl_InitializeSRWLock(m); },
        4
    });

    // TryAcquireSRWLockExclusive (1 arg)
    exports.entries.push_back(ExportEntry{
        "TryAcquireSRWLockExclusive", 0,
        [&mgr](Marshaler& m) { mgr.impl_TryAcquireSRWLockExclusive(m); },
        4
    });

    // InitOnceBeginInitialize (4 args)
    exports.entries.push_back(ExportEntry{
        "InitOnceBeginInitialize", 0,
        [&mgr](Marshaler& m) { mgr.impl_InitOnceBeginInitialize(m); },
        16
    });

    // InitOnceComplete (3 args)
    exports.entries.push_back(ExportEntry{
        "InitOnceComplete", 0,
        [&mgr](Marshaler& m) { mgr.impl_InitOnceComplete(m); },
        12
    });

    // CreateEventA — now handled by SyncManager (real event creation)

    // WaitForSingleObjectEx (3 args)
    exports.entries.push_back(ExportEntry{
        "WaitForSingleObjectEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_WaitForSingleObjectEx(m); },
        12
    });

    // SwitchToThread (0 args)
    exports.entries.push_back(ExportEntry{
        "SwitchToThread", 0,
        [&mgr](Marshaler& m) { mgr.impl_SwitchToThread(m); },
        0
    });

    // SetThreadPriority (2 args)
    exports.entries.push_back(ExportEntry{
        "SetThreadPriority", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_SetThreadPriority(m); },
        8
    });

    // SetThreadAffinityMask (2 args)
    exports.entries.push_back(ExportEntry{
        "SetThreadAffinityMask", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_SetThreadAffinityMask(m); },
        8
    });

    // SetThreadIdealProcessor (2 args) — stdcall
    exports.entries.push_back(ExportEntry{
        "SetThreadIdealProcessor", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadIdealProcessor(m); },
        8
    });

    // OpenThread (3 args)
    exports.entries.push_back(ExportEntry{
        "OpenThread", 0,
        [&mgr](Marshaler& m) { mgr.impl_OpenThread(m); },
        12
    });

    // GetCurrentProcessorNumber (0 args)
    exports.entries.push_back(ExportEntry{
        "GetCurrentProcessorNumber", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentProcessorNumber(m); },
        0
    });

    // VOID GetCurrentThreadStackLimits(PULONG_PTR LowLimit,
    //                                  PULONG_PTR HighLimit)  (2 args)
    exports.entries.push_back(ExportEntry{
        "GetCurrentThreadStackLimits", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentThreadStackLimits(m); },
        8
    });

    // BOOL SetThreadStackGuarantee(PULONG StackSizeInBytes)  (1 arg)
    exports.entries.push_back(ExportEntry{
        "SetThreadStackGuarantee", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadStackGuarantee(m); },
        4
    });

    // HANDLE FindFirstFileNameW(LPCWSTR lpFileName, DWORD dwFlags,
    //                           LPDWORD StringLength, PWSTR LinkName)
    // BOOL   FindNextFileNameW(HANDLE hFindStream, LPDWORD StringLength,
    //                          PWSTR LinkName)
    //
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilenamew
    // Enumerates every hard link to a file. That requires an index the volume
    // has to maintain; on volumes without one Windows reports
    // ERROR_INVALID_FUNCTION. Vitreo's VFS keeps no reverse link index, and
    // reporting only the queried name would silently under-report links —
    // the documented "this volume cannot do it" answer is the honest one.
    constexpr u64 INVALID_HANDLE = ~0ull;
    exports.entries.push_back(ExportEntry{
        "FindFirstFileNameW", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            set_last_error(error_codes::ERROR_INVALID_FUNCTION);
            m.set_return(INVALID_HANDLE);
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "FindNextFileNameW", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            set_last_error(error_codes::ERROR_INVALID_FUNCTION);
            m.set_return(0);
        },
        12
    });

    // BOOL GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber)
    //
    // Learn: https://learn.microsoft.com/en-us/windows/win32/api/processtopologyapi/nf-processtopologyapi-getcurrentprocessornumberex
    // PROCESSOR_NUMBER { WORD Group; BYTE Number; BYTE Reserved; }. Vitreo
    // presents a single processor group; Number is the host CPU the thread
    // is currently scheduled on (same as GetCurrentProcessorNumber).
    exports.entries.push_back(ExportEntry{
        "GetCurrentProcessorNumberEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentProcessorNumberEx(m); },
        4
    });

    // GetThreadPriority (1 arg)
    exports.entries.push_back(ExportEntry{
        "GetThreadPriority", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetThreadPriority(m); },
        4
    });

    // GetProcessAffinityMask (3 args)
    exports.entries.push_back(ExportEntry{
        "GetProcessAffinityMask", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); m.set_return(0); return; }
        mgr.impl_GetProcessAffinityMask(m); },
        12
    });

    // ---- XSTATE / processor group ----
    //
    // coreclr probes these during startup to decide which CPU state it may
    // save and which processor groups exist. All three were missing, so they
    // resolved to the missing-export stub returning 0xFFFFFFFF -- and
    // LocateXStateFeature returns a POINTER, so the caller dereferenced
    // 0xffffffff. Measured on PowerShell 7.

    // DWORD64 GetEnabledXStateFeatures(void)
    exports.entries.push_back(ExportEntry{
        "GetEnabledXStateFeatures", 0,
        [](Marshaler& m) {
            // XSTATE_MASK_LEGACY_FLOATING_POINT (0x1) | XSTATE_MASK_LEGACY_SSE
            // (0x2). AVX (0x4) is deliberately NOT advertised: the extended
            // save area is not modelled, and a caller told AVX state exists
            // will ask LocateXStateFeature for it and expect a real buffer.
            m.set_return(u64{0x3});
        },
        0
    });

    // PVOID LocateXStateFeature(PCONTEXT Context, DWORD FeatureId,
    //                           PDWORD Length)
    exports.entries.push_back(ExportEntry{
        "LocateXStateFeature", 0,
        [](Marshaler& m) {
            constexpr u32 kLegacyFloatingPoint = 0;
            constexpr u32 kLegacySse = 1;
            const GuestAddr context = m.arg_ptr(0);
            const u32 feature = static_cast<u32>(m.arg(1));
            const GuestAddr length_out = m.arg_ptr(2);
            if (context == 0) {
                if (length_out != 0) (void)m.write_u32(length_out, 0);
                m.set_return(u64{0});
                return;
            }
            // The legacy areas live inside CONTEXT itself. x64 CONTEXT places
            // FltSave at +0x100: XMM registers at +0x1A0 within it, the x87
            // area at its start. Anything beyond the legacy features has no
            // backing here, and NULL is the documented answer for a feature
            // that is not present -- far better than a bogus pointer.
            constexpr u32 kFltSaveOffset = 0x100;
            constexpr u32 kXmmOffsetInFltSave = 0xA0;
            if (!m.cpu_is_64bit()) {
                if (length_out != 0) (void)m.write_u32(length_out, 0);
                m.set_return(u64{0});
                return;
            }
            if (feature == kLegacyFloatingPoint) {
                if (length_out != 0) (void)m.write_u32(length_out, 160);
                m.set_return(static_cast<u64>(context + kFltSaveOffset));
                return;
            }
            if (feature == kLegacySse) {
                if (length_out != 0) (void)m.write_u32(length_out, 256);
                m.set_return(static_cast<u64>(
                    context + kFltSaveOffset + kXmmOffsetInFltSave));
                return;
            }
            if (length_out != 0) (void)m.write_u32(length_out, 0);
            m.set_return(u64{0});
        },
        12
    });

    // BOOL GetProcessGroupAffinity(HANDLE, PUSHORT GroupCount, PUSHORT Array)
    exports.entries.push_back(ExportEntry{
        "GetProcessGroupAffinity", 0,
        [](Marshaler& m) {
            const GuestAddr count_ptr = m.arg_ptr(1);
            const GuestAddr array_ptr = m.arg_ptr(2);
            if (m.arg(0) == 0 || count_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const auto have = m.read_u16(count_ptr);
            const u16 capacity = have.is_ok() ? have.value() : 0;
            // One processor group, number 0. Windows reports
            // ERROR_INSUFFICIENT_BUFFER and the REQUIRED count when the array
            // is too small, so a caller that sizes then retries works.
            (void)m.write_u16(count_ptr, 1);
            if (capacity < 1 || array_ptr == 0) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            (void)m.write_u16(array_ptr, 0);
            m.set_return(1u);
        },
        12
    });

    // SetProcessAffinityMask (2 args)
    exports.entries.push_back(ExportEntry{
        "SetProcessAffinityMask", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_SetProcessAffinityMask(m); },
        8
    });

    // ProcessIdToSessionId (2 args)
    exports.entries.push_back(ExportEntry{
        "ProcessIdToSessionId", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
        mgr.impl_ProcessIdToSessionId(m); },
        8
    });

    // OutputDebugStringA (1 arg)
    exports.entries.push_back(ExportEntry{
        "OutputDebugStringA", 0,
        [&mgr](Marshaler& m) { mgr.impl_OutputDebugStringA(m); },
        4
    });

    // OutputDebugStringW (1 arg)
    exports.entries.push_back(ExportEntry{
        "OutputDebugStringW", 0,
        [&mgr](Marshaler& m) { mgr.impl_OutputDebugStringW(m); },
        4
    });

    // DebugBreak (0 args)
    exports.entries.push_back(ExportEntry{
        "DebugBreak", 0,
        [&mgr](Marshaler& m) { mgr.impl_DebugBreak(m); },
        0
    });
}

} // namespace vitreo::winapi
