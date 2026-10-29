// SPDX-License-Identifier: LGPL-2.1-only

/// @file console_alloc.cpp
/// @brief Wave 3 Group B: AllocConsole / FreeConsole / GetConsoleWindow.
///
/// Console-attachment stubs from the Wave 3 banner.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
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

// ---- Group B: Console ----
void Kernel32ExtManager::impl_AllocConsole(Marshaler& m) { m.set_return(1); }
void Kernel32ExtManager::impl_FreeConsole(Marshaler& m) {
    // BOOL FreeConsole(void)
    // Learn + process_contract_probe.expected "console state": always succeeds;
    // when a console was attached, detaches it (console APIs then fail with
    // ERROR_INVALID_HANDLE) and resets the control-handler table. First
    // detach leaves last error ERROR_INVALID_HANDLE (measured); already
    // detached leaves last error untouched.
    if (m_console_manager == nullptr) {
        m.set_return(1u);
        return;
    }
    const bool was_attached = m_console_manager->is_attached();
    m_console_manager->detach();
    m_console_manager->clear_ctrl_handlers();
    if (was_attached) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
    }
    m.set_return(1u);
}
// GetConsoleWindow returns the HWND of the console window owned by the calling
// process, or NULL when the process has no console. Vitreo never creates a
// console *window* (console output is routed to the host terminal), so NULL is
// the honest and Windows-correct answer. Callers use this to decide whether to
// hide/own a console; the generic missing-export stub's 0xFFFFFFFF is a bogus
// HWND that they then pass to ShowWindow/GetWindowThreadProcessId.
// CreateSemaphoreEx{W,A}(lpAttrs, lInitial, lMaximum, lpName, dwFlags,
// dwDesiredAccess). Identical object semantics to CreateSemaphore{W,A} for the
// first four arguments; dwFlags is reserved and MUST be zero (Windows fails the
// call with ERROR_INVALID_PARAMETER otherwise). dwDesiredAccess is not enforced
// because Vitreo handles carry no access mask — a documented limitation, not a
// per-call failure.
//
// dwFlags is the 5th argument, so on x64 it arrives in a stack home whose
// upper dword is undefined — it must be read 32 bits wide. See arg32().
void Kernel32ExtManager::impl_CreateSemaphoreExW(Marshaler& m) {
    if (m.arg32(4) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    impl_CreateSemaphoreW(m);
}

void Kernel32ExtManager::impl_CreateSemaphoreExA(Marshaler& m) {
    if (m.arg32(4) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    impl_CreateSemaphoreA(m);
}

void Kernel32ExtManager::impl_GetConsoleWindow(Marshaler& m) {
    VITREO_TRACE("[k32ext] GetConsoleWindow() -> NULL (no console window)\n");
    m.set_return(0u);
}

void Kernel32ExtManager::impl_GetConsoleDisplayMode(Marshaler& m) {
    GuestAddr p = m.arg_ptr(0);
    if (p) (void)m.write_u32(p, 0);
    m.set_return(1);
}

} // namespace vitreo::winapi
