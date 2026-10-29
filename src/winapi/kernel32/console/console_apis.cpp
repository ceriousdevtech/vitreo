// SPDX-License-Identifier: LGPL-2.1-only

/// @file console_apis.cpp
/// @brief Console banner APIs.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

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
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
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
// Console
// =============================================================================

void Kernel32ExtManager::impl_GetConsoleCP(Marshaler& m) {
    // UINT GetConsoleCP(void)
    // Learn / process_contract_probe.expected: no attached console → 0 +
    // ERROR_INVALID_HANDLE; otherwise the input code page (OEM US 437).
    if (m_console_manager == nullptr || !m_console_manager->is_attached()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    m.set_return(m_console_manager->input_code_page());
}

void Kernel32ExtManager::impl_SetConsoleCP(Marshaler& m) {
    // BOOL SetConsoleCP(UINT wCodePageID)
    // Learn: sets the console input code page; a code page that is not
    // installed fails with ERROR_INVALID_PARAMETER and keeps the current one.
    const u32 cp = static_cast<u32>(m.arg(0));
    if (m_console_manager == nullptr || !m_console_manager->is_attached()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    if (!codepage::is_valid_code_page(cp)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m_console_manager->set_input_code_page(cp);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_ReadConsoleW(Marshaler& m) {
    // BOOL ReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer,
    //   DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead,
    //   PCONSOLE_READCONSOLE_CONTROL pInputControl)
    // Learn ReadConsole: reads typed characters from the console input
    // buffer (blocking until input is available) and fails for a handle that
    // is not a console input handle. Windows 11 (process_contract_probe):
    // a pipe -> ERROR_INVALID_FUNCTION, an invalid handle ->
    // ERROR_INVALID_HANDLE, *lpNumberOfCharsRead untouched on failure.
    // CONSOLE_READCONSOLE_CONTROL (wincon.h): nLength, nInitialChars,
    // dwCtrlWakeupMask, dwControlKeyState (4 x ULONG). nInitialChars units
    // at the start of lpBuffer are preserved and counted in the result; the
    // read ends after a control character whose bit is in dwCtrlWakeupMask.
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 to_read = static_cast<u32>(m.arg(2));
    const GuestAddr read_ptr = m.arg_ptr(3);
    const GuestAddr control = m.arg_ptr(4);
    if (m_console_manager == nullptr ||
        !m_console_manager->is_console_input(handle)) {
        set_last_error(m_console_manager != nullptr
                           ? m_console_manager->read_handle_error(handle)
                           : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    u32 initial = 0;
    u32 wakeup_mask = 0;
    if (control != 0) {
        const u32 length = m_memory.read_u32(control).value_or(0);
        if (length < 16) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        initial = std::min(m_memory.read_u32(control + 4).value_or(0), to_read);
        wakeup_mask = m_memory.read_u32(control + 8).value_or(0);
    }
    const std::vector<u16> text =
        m_console_manager->read_text(to_read - initial, wakeup_mask);
    for (usize i = 0; i < text.size(); ++i) {
        if (m_memory.write_u16(buffer + (initial + i) * 2, text[i]).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0u);
            return;
        }
    }
    if (control != 0) (void)m_memory.write_u32(control + 12, 0);
    if (read_ptr != 0) {
        (void)m_memory.write_u32(read_ptr, initial + static_cast<u32>(text.size()));
    }
    m.set_return(1u);
}

void Kernel32ExtManager::impl_SetConsoleCtrlHandler(Marshaler& m) {
    // BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE HandlerRoutine, BOOL Add)
    // Learn + Windows 11 (process_contract_probe.expected):
    //  - Add=TRUE appends the routine (called last-registered first);
    //  - Add=FALSE removes it; removing a routine that is not registered
    //    fails with ERROR_INVALID_PARAMETER;
    //  - HandlerRoutine=NULL sets (TRUE) / clears (FALSE) the process's
    //    "ignore CTRL+C" attribute; both succeed.
    const GuestAddr routine = m.arg_ptr(0);
    const bool add = static_cast<u32>(m.arg(1)) != 0;
    if (m_console_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    if (routine == 0) {
        m_console_manager->set_ignore_ctrl_c(add);
        m.set_return(1u);
        return;
    }
    const bool ok = add ? m_console_manager->add_ctrl_handler(routine)
                        : m_console_manager->remove_ctrl_handler(routine);
    if (!ok) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(ok ? 1u : 0u);
}

void Kernel32ExtManager::impl_SetStdHandle(Marshaler& m) {
    // BOOL SetStdHandle(DWORD nStdHandle, HANDLE hHandle)
    // Learn SetStdHandle + Windows 11 (process_contract_probe.expected): any
    // handle value, NULL and INVALID_HANDLE_VALUE included, replaces the slot
    // and GetStdHandle returns it; an nStdHandle other than -10/-11/-12 fails
    // with ERROR_INVALID_HANDLE. Success leaves the last error untouched.
    if (m_console_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    const bool ok = m_console_manager->set_std_handle(
        static_cast<u32>(m.arg(0)), static_cast<Handle>(m.arg(1)));
    m.set_return(ok ? 1u : 0u);
}

} // namespace vitreo::winapi
