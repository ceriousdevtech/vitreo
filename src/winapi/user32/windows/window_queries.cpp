// SPDX-License-Identifier: LGPL-2.1-only

/// @file window_queries.cpp
/// @brief Window query APIs (IsWindow, GetParent, …).
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <optional>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ---------- Window queries ----------

void User32ExtManager::impl_FindWindowExA(Marshaler& m) {
    find_window_ex_impl(m, true);
}

void User32ExtManager::impl_FindWindowExW(Marshaler& m) {
    find_window_ex_impl(m, false);
}

// ---------------------------------------------------------------------------
// FindWindow(A/W) and FindWindowEx(A/W).
//
// Contract: Learn FindWindowW / FindWindowExW. FindWindow searches top-level
// windows only (measured, s1win_api_probe findwindoww_child_not_found), in
// Z order; class and title compare case-insensitively (findwindowa_case); a
// NULL class or title matches any window while an empty title matches only
// an untitled window. FindWindowEx searches the direct children of
// hwndParent after hwndChildAfter; a NULL parent searches the top-level
// windows and then the message-only windows (findwindowexw_msgonly_via_null),
// HWND_MESSAGE only the message-only windows; an invalid parent is
// ERROR_INVALID_WINDOW_HANDLE; a hwndChildAfter that is not a child finds
// nothing. A miss leaves the last error untouched.
// ---------------------------------------------------------------------------

namespace {

bool iequals_ci(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::towupper(static_cast<wint_t>(a[i])) !=
            std::towupper(static_cast<wint_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

void User32ExtManager::find_window_impl(Marshaler& m, bool ex, bool ansi) {
    const auto parent = ex ? static_cast<windowing::HWND>(m.arg(0)) : windowing::NULL_HWND;
    const auto child_after = ex ? static_cast<windowing::HWND>(m.arg(1)) : windowing::NULL_HWND;
    const GuestAddr cls_ptr = m.arg_ptr(ex ? 2 : 0);
    const GuestAddr title_ptr = m.arg_ptr(ex ? 3 : 1);

    std::optional<std::wstring> cls_name;
    std::optional<ATOM> cls_atom;
    std::optional<std::wstring> title;
    if (cls_ptr != 0 && cls_ptr < 0x10000) {
        cls_atom = static_cast<ATOM>(cls_ptr);
    } else if (cls_ptr != 0) {
        auto r = ansi ? m.read_ansi_string(cls_ptr) : m.read_wide_string(cls_ptr);
        cls_name = r.is_ok() ? utf8_to_wstring(r.value()) : std::wstring{};
    }
    if (title_ptr != 0) {
        auto r = ansi ? m.read_ansi_string(title_ptr) : m.read_wide_string(title_ptr);
        title = r.is_ok() ? utf8_to_wstring(r.value()) : std::wstring{};
    }

    auto matches = [&](windowing::HWND hwnd) -> bool {
        const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
        if (!win) return false;
        if (cls_atom) {
            const WindowClassData* cls = window_class(*win);
            const ATOM atom = cls ? cls->atom : static_cast<ATOM>(win->class_atom);
            if (atom != *cls_atom) return false;
        } else if (cls_name && !iequals_ci(window_class_name(hwnd, *win), *cls_name)) {
            return false;
        }
        if (title && !iequals_ci(win->title, *title)) return false;
        return true;
    };

    std::vector<windowing::HWND> candidates;
    if (!ex || parent == windowing::NULL_HWND || parent == windowing::HWND_DESKTOP) {
        candidates = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
        if (ex && parent == windowing::NULL_HWND) {
            const auto msg = m_hwnd_mgr.message_only_windows();
            candidates.insert(candidates.end(), msg.begin(), msg.end());
        }
    } else if (parent == windowing::HWND_MESSAGE) {
        candidates = m_hwnd_mgr.message_only_windows();
    } else if (m_hwnd_mgr.is_valid(parent)) {
        candidates = m_hwnd_mgr.enum_children(parent);
    } else {
        if (!ipc::hwnd_registry_is_window(static_cast<u32>(parent))) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        }
        m.set_return(0u);
        return;
    }

    usize start = 0;
    if (child_after != windowing::NULL_HWND) {
        auto it = std::find(candidates.begin(), candidates.end(), child_after);
        if (it == candidates.end()) {
            m.set_return(0u);
            return;
        }
        start = static_cast<usize>(it - candidates.begin()) + 1;
    }
    for (usize i = start; i < candidates.size(); ++i) {
        if (matches(candidates[i])) {
            VITREO_TRACE("[user32_ext] FindWindow%s%c -> 0x%x\n", ex ? "Ex" : "",
                         ansi ? 'A' : 'W', candidates[i]);
            m.set_return(static_cast<u64>(candidates[i]));
            return;
        }
    }

    // Not in this process. Win32 FindWindow searches the whole desktop, and
    // Vitreo runs each guest process as its own host process, so a peer's
    // top-level window is invisible to the local table. A program waiting on
    // another process's window POLLS, so a miss that can never resolve
    // becomes an unbounded wait rather than an error.
    const bool top_level_search =
        !ex || parent == windowing::NULL_HWND || parent == windowing::HWND_DESKTOP;
    if (top_level_search && child_after == windowing::NULL_HWND && !cls_atom &&
        ((cls_name && !cls_name->empty()) || (title && !title->empty()))) {
        const auto peer = ipc::hwnd_registry_find_window(
            cls_name ? wstring_to_utf8(*cls_name) : std::string{},
            title ? wstring_to_utf8(*title) : std::string{});
        if (peer.has_value() && (peer->parent_hwnd == 0 ||
                                 peer->parent_hwnd == windowing::HWND_DESKTOP)) {
            m.set_return(static_cast<u64>(peer->hwnd));
            return;
        }
    }
    m.set_return(0u);
}

void User32ExtManager::find_window_ex_impl(Marshaler& m, bool ansi) {
    find_window_impl(m, /*ex=*/true, ansi);
}

void User32ExtManager::impl_EnumWindows(Marshaler& m) {
    // BOOL WINAPI EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam);
    impl_EnumWindows_values(m, m.arg_ptr(0), m.arg(1));
}

void User32ExtManager::impl_EnumWindows_values(Marshaler& m, GuestAddr enum_proc,
                                               u64 lparam) {

    if (enum_proc == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    // Snapshot all top-level windows on the screen (Learn: system-wide, not
    // just this process). Local desktop children come first; peer tops from
    // the capsule hwnd_registry follow (same source FindWindow already uses).
    std::vector<windowing::HWND> top_level =
        m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    {
        std::unordered_set<u32> seen;
        seen.reserve(top_level.size() * 2 + 8);
        for (const auto hwnd : top_level) seen.insert(hwnd);
        const u32 self_pid = static_cast<u32>(::getpid());
        for (const auto& entry : ipc::hwnd_registry_enumerate()) {
            // A top-level window is published with the desktop as its parent.
            const bool is_top = entry.parent_hwnd == 0 ||
                                entry.parent_hwnd == windowing::HWND_DESKTOP;
            if (!is_top || entry.owner_host_pid == self_pid) {
                continue;
            }
            if (seen.insert(entry.hwnd).second) {
                top_level.push_back(static_cast<windowing::HWND>(entry.hwnd));
            }
        }
    }
    bool is_64bit = m.cpu_is_64bit();
    if (std::getenv("VITREO_WINDOW_DIAG") != nullptr) {
        std::fprintf(stderr, "[window-diag] EnumWindows count=%zu\n",
                     top_level.size());
        for (const auto hwnd : top_level) {
            const auto* window = m_hwnd_mgr.find(hwnd);
            if (window == nullptr) continue;
            std::fprintf(
                stderr,
                "[window-diag] hwnd=0x%x visible=%u minimized=%u "
                "rect=(%d,%d,%d,%d) style=0x%x ex=0x%x\n",
                hwnd, window->visible ? 1u : 0u,
                window->minimized ? 1u : 0u, window->rect.left,
                window->rect.top, window->rect.right, window->rect.bottom,
                window->style, window->ex_style);
        }
    }

    for (auto hwnd : top_level) {
        u64 args[2] = { static_cast<u64>(hwnd), lparam };
        u64 cb_ret = m_guest_call(enum_proc, args, 2, is_64bit);
        if (cb_ret == 0) {
            // Stop enumeration when callback returns FALSE. Learn: the
            // callback sets the last error it wants the caller to see; it
            // passes through untouched (measured on Windows 11:
            // last_error_on_success_user_probe.expected).
            m.set_return(0);
            return;
        }
    }

    m.set_return(1);
}

void User32ExtManager::impl_EnumThreadWindows(Marshaler& m) {
    // BOOL WINAPI EnumThreadWindows(
    //     DWORD dwThreadId, WNDENUMPROC lpfn, LPARAM lParam);
    const u32 thread_id = static_cast<u32>(m.arg(0));
    const GuestAddr enum_proc = m.arg_ptr(1);
    const u64 lparam = m.arg(2);

    if (enum_proc == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    // EnumThreadWindows enumerates nonchild windows only. Snapshot the
    // process-local top-level list before entering guest callbacks so a
    // callback may safely create or destroy windows.
    const auto top_level = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    const bool is_64bit = m.cpu_is_64bit();
    bool found = false;
    for (const auto hwnd : top_level) {
        const auto* window = m_hwnd_mgr.find(hwnd);
        if (window == nullptr || window->owner_thread_id != thread_id) {
            continue;
        }

        found = true;
        const u64 args[2] = {static_cast<u64>(hwnd), lparam};
        if (m_guest_call(enum_proc, args, 2, is_64bit) == 0) {
            // The callback owns LastError when it stops enumeration.
            m.set_return(0);
            return;
        }
    }

    // Measured (s1win_msgseq2_probe enumthreadwindows_*): a thread with no
    // nonchild windows -- live, exited or unknown -- returns TRUE with
    // ERROR_INVALID_PARAMETER; Learn's "FALSE if no windows are found" does
    // not hold on Windows 11.
    if (!found) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(1u);
}

void User32ExtManager::impl_GetWindowThreadProcessId(Marshaler& m) {
    // DWORD WINAPI GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId);
    windowing::HWND hwnd = static_cast<windowing::HWND>(m.arg(0));
    GuestAddr pid_out = m.arg_ptr(1);

    const windowing::WindowData* local = m_hwnd_mgr.find(hwnd);
    const auto foreign = local == nullptr
        ? ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))
        : std::nullopt;
    if (local == nullptr && !foreign.has_value()) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    if (m_process_mgr == nullptr) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    const u32 process_id = local != nullptr
        ? m_process_mgr->get_current_process_id()
        : foreign->owner_host_pid;
    u32 thread_id = local != nullptr
        ? local->owner_thread_id
        : foreign->owner_guest_thread_id;
    if (process_id == 0 || thread_id == 0) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }
    if (pid_out != 0) {
        if (m.write_u32(pid_out, process_id).is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }
    m.set_return(thread_id);
}

void User32ExtManager::impl_IsWindowEnabled(Marshaler& m) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        // A peer process's window: its WS_DISABLED bit is not published.
        if (hwnd != 0 && ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
            m.set_return(1u);
            return;
        }
        // Measured (iswindowenabled_invalid): ERROR_INVALID_WINDOW_HANDLE.
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    // Only the window's own WS_DISABLED bit counts (measured,
    // iswindowenabled_child_of_disabled).
    m.set_return(win->enabled ? 1u : 0u);
}

void User32ExtManager::impl_GetMessagePos(Marshaler& m) {
    // The retrieved message's screen point is stable even if the cursor
    // has since moved. Both packed coordinates are signed 16-bit values.
    m.set_return(last_message_pos());
}

} // namespace vitreo::winapi
