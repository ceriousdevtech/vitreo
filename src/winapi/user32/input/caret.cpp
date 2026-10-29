// SPDX-License-Identifier: LGPL-2.1-only

/// @file caret.cpp
/// @brief The thread caret: CreateCaret, DestroyCaret, SetCaretPos, GetCaretPos and
///        ShowCaret/HideCaret. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace vitreo::winapi {

void User32ExtManager::impl_CreateCaret(Marshaler& m) {
    // BOOL CreateCaret(HWND hWnd, HBITMAP hBitmap, int nWidth, int nHeight).
    // s2msg_input_probe: an invalid window fails with
    // ERROR_INVALID_WINDOW_HANDLE; another thread's window fails with the
    // last error untouched. A new caret replaces the thread's old one and
    // starts hidden; 0 width/height mean the system border width/height.
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u64 bitmap = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    i32 width = static_cast<i32>(m.arg32(2));
    i32 height = static_cast<i32>(m.arg32(3));
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    const auto owner = m_hwnd_mgr.owner_thread_id(hwnd);
    if (owner && *owner != 0 && *owner != tid) {
        m.set_return(0u);
        return;
    }
    constexpr i32 SM_CXBORDER = 1;
    constexpr i32 SM_CYBORDER = 1;
    if (width == 0) width = SM_CXBORDER;
    if (height == 0) height = SM_CYBORDER;
    {
        std::lock_guard lock(m_caret_mutex);
        CaretState caret{};
        caret.hwnd = hwnd;
        caret.width = width;
        caret.height = height;
        caret.bitmap = bitmap;
        m_carets[tid] = caret;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED user32!CreateCaret: the caret's state is "
            "kept but it is not drawn\n");
    }
    m.set_return(1u);
}

void User32ExtManager::impl_DestroyCaret(Marshaler& m) {
    // No caret on the calling thread: FALSE / ERROR_ACCESS_DENIED
    // (destroycaret_no_caret / destroycaret_again).
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_caret_mutex);
    if (m_carets.erase(tid) == 0) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

void User32ExtManager::impl_SetCaretPos(Marshaler& m) {
    // No caret on the calling thread: FALSE / ERROR_ACCESS_DENIED
    // (setcaretpos_no_caret).
    const i32 x = static_cast<i32>(m.arg(0));
    const i32 y = static_cast<i32>(m.arg(1));
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_caret_mutex);
    auto it = m_carets.find(tid);
    if (it == m_carets.end()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    it->second.x = x;
    it->second.y = y;
    m.set_return(1u);
}

void User32ExtManager::impl_GetCaretPos(Marshaler& m) {
    // BOOL GetCaretPos(LPPOINT): the caret position in its window's client
    // coordinates (getcaretpos pt=5,6).
    const GuestAddr point = m.arg_ptr(0);
    if (point == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    i32 x = 0;
    i32 y = 0;
    {
        std::lock_guard lock(m_caret_mutex);
        auto it = m_carets.find(tid);
        if (it != m_carets.end()) {
            x = it->second.x;
            y = it->second.y;
        }
    }
    (void)m.write_u32(point, static_cast<u32>(x));
    (void)m.write_u32(point + 4, static_cast<u32>(y));
    m.set_return(1u);
}

void User32ExtManager::impl_ShowHideCaret(Marshaler& m, bool show) {
    // Show/HideCaret(hWnd): NULL means the calling thread's caret. No caret
    // fails with ERROR_ACCESS_DENIED (hidecaret_no_caret / showcaret_no_caret);
    // a window that does not own it fails with the last error untouched
    // (hidecaret_other_window). Hiding is cumulative and each ShowCaret undoes
    // one HideCaret (Learn); an extra ShowCaret still succeeds
    // (showcaret_again).
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_caret_mutex);
    auto it = m_carets.find(tid);
    if (it == m_carets.end()) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0u);
        return;
    }
    if (hwnd != windowing::NULL_HWND && hwnd != it->second.hwnd) {
        m.set_return(0u);
        return;
    }
    if (show) {
        if (it->second.hide_count > 0) --it->second.hide_count;
    } else {
        ++it->second.hide_count;
    }
    m.set_return(1u);
}

} // namespace vitreo::winapi
