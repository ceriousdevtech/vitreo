// SPDX-License-Identifier: LGPL-2.1-only

/// @file shell_station_objects.cpp
/// @brief Opening desktop and window-station objects by name, GetShellWindow and
///        the WH_SHELL / SHELLHOOK notification fan-out. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/model/hwnd_manager.hpp"

#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {

// Caller holds m_user_object_mutex.
u64 User32ExtManager::open_desktop_by_name(const std::string& name) {
    auto ascii_iequal = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    };

    std::string canonical;
    for (const auto& [existing_handle, existing_name] : m_desktops) {
        (void)existing_handle;
        if (ascii_iequal(existing_name, name)) {
            canonical = existing_name;
            break;
        }
    }
    if (canonical.empty()) {
        return 0;
    }
    u32 dw_flags = 0;
    for (const auto& [existing_handle, existing_name] : m_desktops) {
        if (existing_name == canonical) {
            auto flags_it = m_user_object_dw_flags.find(existing_handle);
            if (flags_it != m_user_object_dw_flags.end()) {
                dw_flags = flags_it->second;
            }
            break;
        }
    }
    const u64 handle = m_next_desktop++;
    m_desktops.emplace(handle, canonical);
    m_user_object_dw_flags.emplace(handle, dw_flags);
    return handle;
}

// Caller holds m_user_object_mutex.
u64 User32ExtManager::open_window_station_by_name(const std::string& name) {
    auto ascii_iequal = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    };

    std::string canonical;
    u32 dw_flags = 0;
    for (const auto& [existing_handle, existing_name] : m_window_stations) {
        if (ascii_iequal(existing_name, name)) {
            canonical = existing_name;
            auto flags_it = m_user_object_dw_flags.find(existing_handle);
            if (flags_it != m_user_object_dw_flags.end()) {
                dw_flags = flags_it->second;
            } else if (canonical == "WinSta0") {
                dw_flags = 0x0001u; // WSF_VISIBLE
            }
            break;
        }
    }
    if (canonical.empty()) {
        return 0;
    }
    const u64 handle = m_next_window_station++;
    m_window_stations.emplace(handle, canonical);
    m_user_object_dw_flags.emplace(handle, dw_flags);
    return handle;
}

void User32ExtManager::impl_GetShellWindow(Marshaler& m) {
    VITREO_TRACE("[u32ext] GetShellWindow -> 0\n");
    m.set_return(0u);
}

void User32ExtManager::notify_shell_event(
    u32 event, windowing::HWND subject
) {
    // WH_SHELL procedures of this process get the same notification.
    dispatch_shell_hook(static_cast<i32>(event), static_cast<u64>(subject), 0);
    std::vector<windowing::HWND> recipients;
    {
        std::lock_guard lock(m_shell_hook_mutex);
        for (auto it = m_shell_hook_windows.begin();
             it != m_shell_hook_windows.end();) {
            if (!m_hwnd_mgr.is_valid(*it)) {
                it = m_shell_hook_windows.erase(it);
                continue;
            }
            recipients.push_back(*it);
            ++it;
        }
    }
    if (recipients.empty()) {
        return;
    }

    const u32 shell_message = register_window_message("SHELLHOOK");
    if (shell_message == 0) {
        return;
    }
    for (const windowing::HWND recipient : recipients) {
        windowing::MSG message{};
        message.hwnd = recipient;
        message.message = shell_message;
        message.wParam = event;
        message.lParam = subject;
        (void)m_queue.post(message);
    }
    VITREO_TRACE(
        "[user32_ext] shell hook event=%u subject=0x%x recipients=%zu "
        "message=0x%x\n",
        event, subject, recipients.size(), shell_message);
}

} // namespace vitreo::winapi
