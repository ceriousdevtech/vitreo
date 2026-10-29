// SPDX-License-Identifier: LGPL-2.1-only

/// @file hotkeys.cpp
/// @brief RegisterHotKey, UnregisterHotKey and the WM_HOTKEY dispatch that scans
///        the hot-key table. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace vitreo::winapi {

void User32ExtManager::impl_RegisterHotKey(Marshaler& m) {
    // BOOL RegisterHotKey(HWND, int id, UINT fsModifiers, UINT vk).
    // s2msg_input_probe: an invalid window fails with
    // ERROR_INVALID_WINDOW_HANDLE; modifier bits outside MOD_ALT|CONTROL|
    // SHIFT|WIN|NOREPEAT with ERROR_INVALID_FLAGS; registering the same
    // (hWnd, id) again replaces it (registerhotkey_same_id = 1); a key
    // combination another hot key owns fails with
    // ERROR_HOTKEY_ALREADY_REGISTERED (Learn).
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const i32 id = static_cast<i32>(m.arg(1));
    const u32 mods = static_cast<u32>(m.arg(2));
    const u32 vk = static_cast<u32>(m.arg(3));
    constexpr u32 MOD_NOREPEAT = 0x4000;
    constexpr u32 kValid = 0x000Fu | MOD_NOREPEAT;
    if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if ((mods & ~kValid) != 0) {
        set_last_error(error_codes::ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    const u32 key_mods = mods & 0x000Fu;
    {
        std::lock_guard lock(m_hotkey_mutex);
        for (const HotKey& hk : m_hotkeys) {
            if (hk.vk == vk && hk.modifiers == key_mods &&
                !(hk.hwnd == hwnd && hk.id == id &&
                  (hwnd != windowing::NULL_HWND || hk.thread_id == tid))) {
                set_last_error(error_codes::ERROR_HOTKEY_ALREADY_REGISTERED);
                m.set_return(0u);
                return;
            }
        }
        std::erase_if(m_hotkeys, [&](const HotKey& hk) {
            return hk.hwnd == hwnd && hk.id == id &&
                   (hwnd != windowing::NULL_HWND || hk.thread_id == tid);
        });
        m_hotkeys.push_back(HotKey{hwnd, id, key_mods, vk,
                                   (mods & MOD_NOREPEAT) != 0, tid});
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED user32!RegisterHotKey: hot keys fire only "
            "while a window of this process has keyboard focus, not system-wide\n");
    }
    m.set_return(1u);
}

void User32ExtManager::impl_UnregisterHotKey(Marshaler& m) {
    // An invalid window: ERROR_INVALID_WINDOW_HANDLE; no such hot key:
    // ERROR_HOTKEY_NOT_REGISTERED (unregisterhotkey_again).
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const i32 id = static_cast<i32>(m.arg(1));
    if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    usize removed = 0;
    {
        std::lock_guard lock(m_hotkey_mutex);
        removed = std::erase_if(m_hotkeys, [&](const HotKey& hk) {
            return hk.hwnd == hwnd && hk.id == id &&
                   (hwnd != windowing::NULL_HWND || hk.thread_id == tid);
        });
    }
    if (removed == 0) {
        set_last_error(error_codes::ERROR_HOTKEY_NOT_REGISTERED);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

bool User32ExtManager::dispatch_hotkey(const windowing::MSG& msg) {
    if (msg.message != windowing::WM::KEYDOWN && msg.message != windowing::WM::SYSKEYDOWN) {
        return false;
    }
    const u32 vk = static_cast<u32>(msg.wParam);
    const auto down = [this](u32 key) {
        return (m_injected_keys[key].load() & 0x80u) != 0 ||
               (m_host.get_native_async_key_state(key) & 0x8000) != 0;
    };
    u32 mods = 0;
    if (down(0x12)) mods |= 0x1;                  // MOD_ALT
    if (down(0x11)) mods |= 0x2;                  // MOD_CONTROL
    if (down(0x10)) mods |= 0x4;                  // MOD_SHIFT
    if (down(0x5B) || down(0x5C)) mods |= 0x8;    // MOD_WIN
    const bool repeat = (msg.lParam & (1ull << 30)) != 0;
    HotKey hit{};
    {
        std::lock_guard lock(m_hotkey_mutex);
        if (m_hotkeys.empty()) return false;
        const auto it = std::find_if(m_hotkeys.begin(), m_hotkeys.end(),
            [&](const HotKey& hk) { return hk.vk == vk && hk.modifiers == mods; });
        if (it == m_hotkeys.end()) return false;
        hit = *it;
    }
    if (repeat && hit.no_repeat) return true;
    // WM_HOTKEY: wParam the id, lParam MAKELPARAM(modifiers, vk).
    windowing::MSG hot{};
    hot.hwnd = hit.hwnd;
    hot.message = 0x0312;  // WM_HOTKEY
    hot.wParam = static_cast<u64>(static_cast<i64>(hit.id));
    hot.lParam = (static_cast<u64>(vk) << 16) | mods;
    hot.time = msg.time;
    if (hit.hwnd == windowing::NULL_HWND) hot.target_thread_id = hit.thread_id;
    (void)m_queue.post(hot);
    return true;
}

} // namespace vitreo::winapi
