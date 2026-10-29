// SPDX-License-Identifier: LGPL-2.1-only

/// @file dpi.cpp
/// @brief DPI awareness: the process and thread awareness contexts,
///        Set/GetProcessDpiAwareness, GetDpiForMonitor/Window and
///        SetProcessDPIAware. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/desktop/dpi_policy.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/hwnd_registry.hpp"

#include <mutex>

namespace vitreo::winapi {

// ---------------------------------------------------------------------------
// DPI awareness.
//
// Contract: Microsoft Learn SetProcessDpiAwarenessContext / SetProcessDPIAware
// / SetProcessDpiAwareness / GetProcessDpiAwareness / GetDpiForMonitor /
// GetDpiForWindow. The process default is unaware unless the manifest says
// otherwise, and it can be set once. Measured (s1win_display_probe "-- dpi"):
//  * once set, SetProcessDpiAwarenessContext fails with ERROR_ACCESS_DENIED
//    whatever the argument, SetProcessDpiAwareness returns E_ACCESSDENIED,
//    SetProcessDPIAware still returns TRUE;
//  * SetProcessDpiAwareness(out of range) is E_INVALIDARG;
//  * GetProcessDpiAwareness of a handle that is not a process is
//    E_INVALIDARG and writes -1;
//  * GetDpiForMonitor: an unknown monitor is E_HANDLE, an unknown
//    MONITOR_DPI_TYPE HRESULT_FROM_WIN32(ERROR_BAD_ARGUMENTS), a NULL output
//    HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS); each also sets the last
//    error;
//  * GetDpiForWindow of an invalid or NULL window is 0 /
//    ERROR_INVALID_PARAMETER.
// Vitreo presents every monitor at 96 DPI (1 Win32 pixel = 1 point).
// ---------------------------------------------------------------------------

u32 User32ExtManager::system_dpi() const noexcept {
    return dpi_policy::kDefaultScreenDpi;
}

bool User32ExtManager::set_process_dpi_awareness_context(i64 context) {
    if (m_process_dpi_context_set) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    if (context < -5 || context > -1) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    m_process_dpi_context = context;
    m_process_dpi_context_set = true;
    return true;
}

void User32ExtManager::set_manifest_dpi_awareness_context(i64 context) {
    if (context < -5 || context > -1) return;
    m_process_dpi_context = context;
    m_process_dpi_context_set = true;
}

void User32ExtManager::impl_SetProcessDpiAwareness(Marshaler& m) {
    // HRESULT SetProcessDpiAwareness(PROCESS_DPI_AWARENESS value)
    constexpr u32 E_INVALIDARG = 0x80070057u;
    constexpr u32 E_ACCESSDENIED = 0x80070005u;
    const u32 value = static_cast<u32>(m.arg(0));
    if (value > 2) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(E_INVALIDARG);
        return;
    }
    // PROCESS_DPI_UNAWARE 0 / SYSTEM_DPI_AWARE 1 / PER_MONITOR_DPI_AWARE 2.
    static constexpr i64 kContexts[3] = {-1, -2, -3};
    if (!set_process_dpi_awareness_context(kContexts[value])) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(E_ACCESSDENIED);
        return;
    }
    m.set_return(0u);
}

void User32ExtManager::impl_GetProcessDpiAwareness(Marshaler& m) {
    // HRESULT GetProcessDpiAwareness(HANDLE hprocess, PROCESS_DPI_AWARENESS*)
    constexpr u32 E_INVALIDARG = 0x80070057u;
    const u64 process = m.arg(0);
    const GuestAddr out = m.arg_ptr(1);
    const bool self = process == 0 || process == ~0ull ||
                      (!m.cpu_is_64bit() && process == 0xFFFFFFFFull);
    if (!self) {
        // Only the calling process's awareness is known here.
        if (out) (void)m.write_u32(out, 0xFFFFFFFFu);
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(E_INVALIDARG);
        return;
    }
    const i64 ctx = m_process_dpi_context;
    const u32 awareness = (ctx == -1 || ctx == -5) ? 0u : ctx == -2 ? 1u : 2u;
    (void)m.write_u32(out, awareness);
    m.set_return(0u);
}

void User32ExtManager::impl_GetDpiForMonitor(Marshaler& m) {
    // HRESULT GetDpiForMonitor(HMONITOR, MONITOR_DPI_TYPE, UINT* dpiX, UINT* dpiY)
    constexpr u32 E_HANDLE = 0x80070006u;
    constexpr u32 ERROR_BAD_ARGUMENTS = 0xA0;
    constexpr u32 ERROR_INVALID_ADDRESS = 0x1E7;
    const u64 hmon = m.arg(0);
    const u32 type = static_cast<u32>(m.arg(1));
    const GuestAddr px = m.arg_ptr(2);
    const GuestAddr py = m.arg_ptr(3);
    const int index = monitor_index(hmon);
    if (index < 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(E_HANDLE);
        return;
    }
    if (type > 2) {
        set_last_error(ERROR_BAD_ARGUMENTS);
        m.set_return(0x80070000u | ERROR_BAD_ARGUMENTS);
        return;
    }
    if (px == 0 || py == 0) {
        set_last_error(ERROR_INVALID_ADDRESS);
        m.set_return(0x80070000u | ERROR_INVALID_ADDRESS);
        return;
    }
    u32 x = 96, y = 96;
    if (type == 2) {
        // MDT_RAW_DPI: the panel's physical density. The host reports points
        // per inch for a 1x logical desktop; with no physical size known,
        // the logical density stands in.
        const auto monitors = guest_monitors();
        const auto& mi = monitors[static_cast<usize>(index)];
        if (mi.physical_width_mm > 0 && mi.physical_height_mm > 0) {
            const double scale = mi.scale_factor > 0 ? mi.scale_factor : 1.0;
            x = static_cast<u32>((mi.right - mi.left) * scale * 25.4 / mi.physical_width_mm + 0.5);
            y = static_cast<u32>((mi.bottom - mi.top) * scale * 25.4 / mi.physical_height_mm + 0.5);
        }
    }
    (void)m.write_u32(px, x);
    (void)m.write_u32(py, y);
    m.set_return(0u);
}

void User32ExtManager::impl_GetDpiForWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (hwnd == windowing::NULL_HWND ||
        (!m_hwnd_mgr.is_valid(hwnd) && !ipc::hwnd_registry_is_window(static_cast<u32>(hwnd)))) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    m.set_return(96u);
}

i64 User32ExtManager::thread_dpi_awareness_context() const {
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_dpi_context_mutex);
    const auto override = m_thread_dpi_contexts.find(thread_id);
    return override == m_thread_dpi_contexts.end()
        ? m_process_dpi_context
        : override->second;
}

i64 User32ExtManager::set_thread_dpi_awareness_context(i64 context) {
    if (context < -5 || context > -1) return 0;
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_dpi_context_mutex);
    const auto override = m_thread_dpi_contexts.find(thread_id);
    const i64 previous = override == m_thread_dpi_contexts.end()
        ? m_process_dpi_context
        : override->second;
    m_thread_dpi_contexts[thread_id] = context;
    return previous;
}

void User32ExtManager::impl_SetProcessDpiAwarenessContext(Marshaler& m) {
    const i64 context = static_cast<i32>(m.arg(0));
    m.set_return(set_process_dpi_awareness_context(context) ? 1u : 0u);
}

void User32ExtManager::impl_SetProcessDPIAware(Marshaler& m) {
    // Measured: TRUE, also when the awareness was already set (it is then
    // left as it was).
    if (!m_process_dpi_context_set) (void)set_process_dpi_awareness_context(-2);
    m.set_return(1u);
}

} // namespace vitreo::winapi
