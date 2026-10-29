// SPDX-License-Identifier: LGPL-2.1-only

/// @file comm_port.cpp
/// @brief kernel32 Communications (serial port) family.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// Communications (serial port) family
// =============================================================================
//
// Vitreo exposes no serial devices: no COMn device exists, CreateFile on one
// fails ERROR_FILE_NOT_FOUND and HARDWARE\DEVICEMAP\SERIALCOMM lists nothing.
// So every handle a guest can pass here is some other object, and each
// function reports exactly what Windows reports for that object. The rules,
// including which outputs are written before the failure, are the Windows
// capture tools/oracle_probe/comm_probe.expected (x86 == x64). Windows has a
// COM1, so its "no such port" rows are the COM9 / OpenCommPort(2) ones.

namespace {

/// sizeof(DCB), sizeof(COMMPROP), sizeof(COMMCONFIG): no pointers, so one
/// layout for both bitnesses (winbase.h).
constexpr u32 kDcbSize = 28;
constexpr u32 kCommPropSize = 64;
constexpr u32 kCommConfigSize = 52;
constexpr u32 kPstRs232 = 1;  // PST_RS232

/// The error a comm function sets for `handle`, which cannot be a comm device.
/// Disk files and directories fail the serial IOCTL with 87; pipes,
/// mailslots, the console, NUL and volumes with 1; anything that is not a
/// file object at all (NULL, INVALID_HANDLE_VALUE, events, unknown values)
/// with 6. comm_probe rows file/dir, anon_pipe/named_pipe/mailslot/conout/
/// nul/volume, event/null/invalid/bogus.
u32 non_comm_handle_error(FileManager* files, Handle handle) {
    if (files == nullptr) return error_codes::ERROR_INVALID_HANDLE;
    switch (files->handle_table().get_type(handle)) {
        case HandleType::File:
            return files->is_character_file(handle)
                       ? error_codes::ERROR_INVALID_FUNCTION
                       : error_codes::ERROR_INVALID_PARAMETER;
        case HandleType::Pipe:
        case HandleType::Mailslot:
        case HandleType::ConsoleInput:
        case HandleType::ConsoleOutput:
        case HandleType::Device:
            return error_codes::ERROR_INVALID_FUNCTION;
        default:
            return error_codes::ERROR_INVALID_HANDLE;
    }
}

/// Fail the call the way Windows does for a non-comm handle.
void fail_for_handle(Marshaler& m, FileManager* files, Handle handle, const char* api) {
    const u32 error = non_comm_handle_error(files, handle);
    set_last_error(error);
    VITREO_TRACE("[comm] %s(h=0x%x) -> FALSE err=%u (not a comm device)\n", api,
                 static_cast<unsigned>(handle), error);
    m.set_return(0u);
}

/// The DCB that GetCommState / GetCommConfig write before asking the device:
/// DCBlength = 28, fBinary = 1, everything else zero (GetCommState_bytes).
void write_default_dcb(GuestMemory& memory, GuestAddr dcb) {
    std::array<u8, kDcbSize> bytes{};
    bytes[0] = static_cast<u8>(kDcbSize);
    bytes[8] = 1;  // bit 0 of the flags word: fBinary
    (void)memory.write_bytes(dcb, bytes.data(), bytes.size());
}

}  // namespace

void Kernel32ExtManager::impl_ClearCommBreak(Marshaler& m) {
    fail_for_handle(m, m_file_manager, static_cast<Handle>(m.arg(0)), "ClearCommBreak");
}

void Kernel32ExtManager::impl_SetCommBreak(Marshaler& m) {
    fail_for_handle(m, m_file_manager, static_cast<Handle>(m.arg(0)), "SetCommBreak");
}

void Kernel32ExtManager::impl_ClearCommError(Marshaler& m) {
    // BOOL ClearCommError(HANDLE, LPDWORD lpErrors, LPCOMSTAT lpStat): neither
    // output is written on failure (ClearCommError row, buf and word intact).
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);
    (void)m.arg_ptr(2);
    fail_for_handle(m, m_file_manager, handle, "ClearCommError");
}

void Kernel32ExtManager::impl_EscapeCommFunction(Marshaler& m) {
    // BOOL EscapeCommFunction(HANDLE, DWORD dwFunc). dwFunc is validated first:
    // SETXOFF (1) .. CLRBREAK (9) reach the handle; 0 and 10+ fail 87 for any
    // handle (EscapeCommFunction code= rows).
    const Handle handle = static_cast<Handle>(m.arg(0));
    const u32 function = m.arg32(1);
    if (function < 1 || function > 9) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    fail_for_handle(m, m_file_manager, handle, "EscapeCommFunction");
}

void Kernel32ExtManager::impl_GetCommMask(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);  // lpEvtMask: untouched on failure
    fail_for_handle(m, m_file_manager, handle, "GetCommMask");
}

void Kernel32ExtManager::impl_SetCommMask(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg32(1);
    fail_for_handle(m, m_file_manager, handle, "SetCommMask");
}

void Kernel32ExtManager::impl_GetCommModemStatus(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);  // lpModemStat: untouched on failure
    fail_for_handle(m, m_file_manager, handle, "GetCommModemStatus");
}

void Kernel32ExtManager::impl_GetCommProperties(Marshaler& m) {
    // BOOL GetCommProperties(HANDLE, LPCOMMPROP): the whole COMMPROP is zeroed
    // before the device is asked, whatever the handle (GetCommProperties_bytes).
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr props = m.arg_ptr(1);
    const std::array<u8, kCommPropSize> zeros{};
    (void)m_memory.write_bytes(props, zeros.data(), zeros.size());
    fail_for_handle(m, m_file_manager, handle, "GetCommProperties");
}

void Kernel32ExtManager::impl_GetCommState(Marshaler& m) {
    // BOOL GetCommState(HANDLE, LPDCB): the default DCB is written first, and
    // the caller's DCBlength is not checked (GetCommState_len0).
    const Handle handle = static_cast<Handle>(m.arg(0));
    write_default_dcb(m_memory, m.arg_ptr(1));
    fail_for_handle(m, m_file_manager, handle, "GetCommState");
}

void Kernel32ExtManager::impl_SetCommState(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);
    fail_for_handle(m, m_file_manager, handle, "SetCommState");
}

void Kernel32ExtManager::impl_GetCommConfig(Marshaler& m) {
    // BOOL GetCommConfig(HANDLE, LPCOMMCONFIG, LPDWORD lpdwSize). The size is
    // checked before the handle: below sizeof(COMMCONFIG) it fails 87 and
    // reports 52 (GetCommConfig_small, event GetCommConfig_size0). Otherwise
    // *lpdwSize becomes 52 and the default COMMCONFIG is written - dwSize,
    // wVersion 1, the default DCB, dwProviderSubType PST_RS232, no provider
    // data; 50 bytes, the trailing padding untouched - before the handle fails
    // (GetCommConfig_bytes, GetCommConfig_big).
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr config = m.arg_ptr(1);
    const GuestAddr size_ptr = m.arg_ptr(2);
    const u32 size = m.read_u32(size_ptr).value_or(0);
    (void)m.write_u32(size_ptr, kCommConfigSize);
    if (size < kCommConfigSize) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    std::array<u8, kCommConfigSize - 2> bytes{};
    bytes[0] = static_cast<u8>(kCommConfigSize);  // dwSize
    bytes[4] = 1;                                 // wVersion
    (void)m_memory.write_bytes(config, bytes.data(), bytes.size());
    write_default_dcb(m_memory, config + 8);      // dcb
    (void)m.write_u32(config + 36, kPstRs232);    // dwProviderSubType
    fail_for_handle(m, m_file_manager, handle, "GetCommConfig");
}

void Kernel32ExtManager::impl_SetCommConfig(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);
    (void)m.arg32(2);
    fail_for_handle(m, m_file_manager, handle, "SetCommConfig");
}

void Kernel32ExtManager::impl_GetCommTimeouts(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);  // lpCommTimeouts: untouched on failure
    fail_for_handle(m, m_file_manager, handle, "GetCommTimeouts");
}

void Kernel32ExtManager::impl_SetCommTimeouts(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);
    fail_for_handle(m, m_file_manager, handle, "SetCommTimeouts");
}

void Kernel32ExtManager::impl_PurgeComm(Marshaler& m) {
    // The flags are not validated ahead of the handle (PurgeComm_badflags).
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg32(1);
    fail_for_handle(m, m_file_manager, handle, "PurgeComm");
}

void Kernel32ExtManager::impl_SetupComm(Marshaler& m) {
    // BOOL SetupComm(HANDLE, DWORD dwInQueue, DWORD dwOutQueue)
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg32(1);
    (void)m.arg32(2);
    fail_for_handle(m, m_file_manager, handle, "SetupComm");
}

void Kernel32ExtManager::impl_TransmitCommChar(Marshaler& m) {
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg(1);
    fail_for_handle(m, m_file_manager, handle, "TransmitCommChar");
}

void Kernel32ExtManager::impl_WaitCommEvent(Marshaler& m) {
    // BOOL WaitCommEvent(HANDLE, LPDWORD lpEvtMask, LPOVERLAPPED)
    const Handle handle = static_cast<Handle>(m.arg(0));
    (void)m.arg_ptr(1);  // untouched on failure
    (void)m.arg_ptr(2);
    fail_for_handle(m, m_file_manager, handle, "WaitCommEvent");
}

// Get/SetDefaultCommConfig name a port. With no port of that name they fail 87
// and write nothing, not even the size (GetDefaultCommConfigW_COM9 /
// _COM9_small / _BOGUS / _null, SetDefaultCommConfigW_COM9 / _BOGUS).
void Kernel32ExtManager::impl_GetDefaultCommConfigA(Marshaler& m) {
    (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg_ptr(2);
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0u);
}
void Kernel32ExtManager::impl_GetDefaultCommConfigW(Marshaler& m) {
    (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg_ptr(2);
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0u);
}
void Kernel32ExtManager::impl_SetDefaultCommConfigA(Marshaler& m) {
    (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg32(2);
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0u);
}
void Kernel32ExtManager::impl_SetDefaultCommConfigW(Marshaler& m) {
    (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg32(2);
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0u);
}

void Kernel32ExtManager::impl_GetCommPorts(Marshaler& m) {
    // ULONG GetCommPorts(PULONG lpPortNumbers, ULONG uPortNumbersCount,
    //                    PULONG puPortNumbersFound)
    // Learn: ERROR_FILE_NOT_FOUND when there are no comm ports. The result is
    // also the last error (Windows GetCommPorts / _null rows), and the
    // found count is the number written: none.
    (void)m.arg_ptr(0);
    (void)m.arg32(1);
    const GuestAddr found = m.arg_ptr(2);
    (void)m.write_u32(found, 0);
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    m.set_return(error_codes::ERROR_FILE_NOT_FOUND);
}

void Kernel32ExtManager::impl_OpenCommPort(Marshaler& m) {
    // HANDLE OpenCommPort(ULONG uPortNumber, DWORD, DWORD): a port that does
    // not exist (including 0) fails ERROR_FILE_NOT_FOUND (OpenCommPort_0/_2).
    (void)m.arg32(0); (void)m.arg32(1); (void)m.arg32(2);
    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    m.set_return(m.cpu_is_64bit() ? ~0ull : 0xFFFFFFFFull);
}

} // namespace vitreo::winapi
