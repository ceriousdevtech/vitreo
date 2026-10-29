// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_tooltip.cpp
/// @brief tooltips_class32 native control.

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/user32/messages/cross_process_send.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_registry.hpp"
#include <unistd.h>
#include <algorithm>
#include <pthread.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

/// tooltips_class32 — the comctl32 tooltip control.
///
/// The owner drives it through TTM_* messages: TTM_ADDTOOL registers the
/// tool, TTM_SETTOOLINFO / TTM_UPDATETIPTEXT set the text, TTM_TRACKPOSITION
/// places it, and TTM_TRACKACTIVATE shows and hides it.
///
/// TOOLINFO layout (commctrl.h, packed to natural alignment):
///          x86   x64
///   cbSize   0     0
///   uFlags   4     4
///   hwnd     8     8
///   uId     12    16   (UINT_PTR)
///   rect    16    24   (RECT, 16 bytes)
///   hinst   32    40
///   lpszText 36   48
bool MessageSendManager::dispatch_tooltip_control(Marshaler& m,
                               windowing::HWND hwnd, u32 msg, u64 wParam,
                               u64 lParam) {
    auto* win = window_data(hwnd);
    if (!win) return false;
    auto name = win->control_class_name();
    for (auto& c : name) c = std::towlower(c);
    if (name != L"tooltips_class32") return false;

    constexpr u32 WM_USER_BASE = 0x0400;
    constexpr u32 TTM_ACTIVATE       = WM_USER_BASE + 1;
    constexpr u32 TTM_SETDELAYTIME   = WM_USER_BASE + 3;
    constexpr u32 TTM_ADDTOOLA       = WM_USER_BASE + 4;
    constexpr u32 TTM_DELTOOLA       = WM_USER_BASE + 5;
    constexpr u32 TTM_RELAYEVENT     = WM_USER_BASE + 7;
    constexpr u32 TTM_GETTOOLINFOA   = WM_USER_BASE + 8;
    constexpr u32 TTM_SETTOOLINFOA   = WM_USER_BASE + 9;
    constexpr u32 TTM_UPDATETIPTEXTA = WM_USER_BASE + 12;
    constexpr u32 TTM_GETTOOLCOUNT   = WM_USER_BASE + 13;
    constexpr u32 TTM_TRACKACTIVATE  = WM_USER_BASE + 17;
    constexpr u32 TTM_TRACKPOSITION  = WM_USER_BASE + 18;
    constexpr u32 TTM_SETTIPBKCOLOR  = WM_USER_BASE + 19;
    constexpr u32 TTM_SETTIPTEXTCOLOR = WM_USER_BASE + 20;
    constexpr u32 TTM_GETDELAYTIME   = WM_USER_BASE + 21;
    constexpr u32 TTM_SETMAXTIPWIDTH = WM_USER_BASE + 24;
    constexpr u32 TTM_GETMAXTIPWIDTH = WM_USER_BASE + 25;
    constexpr u32 TTM_SETMARGIN      = WM_USER_BASE + 26;
    constexpr u32 TTM_POP            = WM_USER_BASE + 28;
    constexpr u32 TTM_UPDATE         = WM_USER_BASE + 29;
    constexpr u32 TTM_GETBUBBLESIZE  = WM_USER_BASE + 30;
    constexpr u32 TTM_ADJUSTRECT     = WM_USER_BASE + 31;
    constexpr u32 TTM_ADDTOOLW       = WM_USER_BASE + 50;
    constexpr u32 TTM_DELTOOLW       = WM_USER_BASE + 51;
    constexpr u32 TTM_GETTOOLINFOW   = WM_USER_BASE + 53;
    constexpr u32 TTM_SETTOOLINFOW   = WM_USER_BASE + 54;
    constexpr u32 TTM_UPDATETIPTEXTW = WM_USER_BASE + 57;

    std::unique_lock lock(m_control_state_mutex);
    auto& state = m_tooltip_state[hwnd];
    const auto show_tip = [&] {
        const auto snapshot = state;
        lock.unlock();
        m_bridge.show_tooltip(hwnd, snapshot.text, snapshot.track_x,
                             snapshot.track_y);
    };
    const auto hide_tip = [&] {
        state.visible = false;
        lock.unlock();
        m_bridge.hide_tooltip(hwnd);
    };
    const bool is_64bit = m.cpu_is_64bit();
    const u32 text_off = is_64bit ? 48u : 36u;

    // LPSTR_TEXTCALLBACK asks the owner for the text through TTN_GETDISPINFO.
    // Any other lpszText is a string pointer.
    const auto read_tool_text = [&](u64 info_ptr, bool wide) -> bool {
        if (info_ptr == 0) return false;
        const u64 text_ptr = is_64bit
            ? m.memory().read_u64(static_cast<GuestAddr>(info_ptr) + text_off)
                  .value_or(0)
            : m.read_u32(static_cast<GuestAddr>(info_ptr) + text_off)
                  .value_or(0);
        if (text_ptr == 0 || text_ptr == 0xFFFFFFFFFFFFFFFFull ||
            text_ptr == 0xFFFFFFFFull) {
            return false;
        }
        auto text = wide
            ? m.read_wide_string(static_cast<GuestAddr>(text_ptr))
            : m.read_ansi_string(static_cast<GuestAddr>(text_ptr));
        if (text.is_err()) return false;
        const std::string& utf8 = text.value();
        std::wstring wide_text;
        wide_text.reserve(utf8.size());
        for (usize i = 0; i < utf8.size();) {
            unsigned char c = static_cast<unsigned char>(utf8[i]);
            char32_t cp = c;
            usize len = 1;
            if (c >= 0xF0) { cp = c & 0x07u; len = 4; }
            else if (c >= 0xE0) { cp = c & 0x0Fu; len = 3; }
            else if (c >= 0xC0) { cp = c & 0x1Fu; len = 2; }
            for (usize k = 1; k < len && i + k < utf8.size(); ++k) {
                cp = (cp << 6) |
                     (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
            }
            wide_text.push_back(static_cast<wchar_t>(cp));
            i += len;
        }
        state.text = std::move(wide_text);
        return true;
    };

    switch (msg) {
    case TTM_ADDTOOLA:
    case TTM_ADDTOOLW:
        (void)read_tool_text(lParam, msg == TTM_ADDTOOLW);
        ++state.tool_count;
        VITREO_TRACE("[send] TTM_ADDTOOL hwnd=0x%x tools=%u\n", hwnd,
                     state.tool_count);
        m.set_return(1);
        return true;

    case TTM_DELTOOLA:
    case TTM_DELTOOLW:
        if (state.tool_count > 0) --state.tool_count;
        if (state.tool_count == 0) {
            hide_tip();
        }
        m.set_return(0);
        return true;

    case TTM_SETTOOLINFOA:
    case TTM_SETTOOLINFOW:
    case TTM_UPDATETIPTEXTA:
    case TTM_UPDATETIPTEXTW: {
        const bool wide = msg == TTM_SETTOOLINFOW || msg == TTM_UPDATETIPTEXTW;
        (void)read_tool_text(lParam, wide);
        // Learn: changing the text of a tip that is already up refreshes it.
        if (state.visible) {
            show_tip();
        }
        m.set_return(0);
        return true;
    }

    case TTM_GETTOOLINFOA:
    case TTM_GETTOOLINFOW:
        m.set_return(state.tool_count != 0 ? 1 : 0);
        return true;

    case TTM_GETTOOLCOUNT:
        m.set_return(state.tool_count);
        return true;

    case TTM_TRACKPOSITION: {
        // lParam packs a screen point: x in the low word, y in the high word.
        state.track_x = static_cast<i16>(lParam & 0xFFFFu);
        state.track_y = static_cast<i16>((lParam >> 16) & 0xFFFFu);
        if (state.visible) {
            show_tip();
        }
        m.set_return(0);
        return true;
    }

    case TTM_TRACKACTIVATE: {
        state.visible = wParam != 0;
        VITREO_TRACE("[send] TTM_TRACKACTIVATE hwnd=0x%x show=%d at (%d,%d)\n",
                     hwnd, state.visible ? 1 : 0, state.track_x, state.track_y);
        if (state.visible) show_tip();
        else hide_tip();
        m.set_return(0);
        return true;
    }

    case TTM_POP:
    case TTM_ACTIVATE:
        // TTM_ACTIVATE(FALSE) suppresses the control; TTM_POP takes down the
        // tip that is showing. Either way nothing may be left on screen.
        if (msg == TTM_POP || wParam == 0) {
            hide_tip();
        }
        m.set_return(0);
        return true;

    case TTM_UPDATE:
        if (state.visible) {
            show_tip();
        }
        m.set_return(0);
        return true;

    case TTM_SETMAXTIPWIDTH: {
        const i32 previous = state.max_width;
        state.max_width = static_cast<i32>(static_cast<i64>(lParam));
        m.set_return(static_cast<u64>(static_cast<i64>(previous)));
        return true;
    }

    case TTM_GETMAXTIPWIDTH:
        m.set_return(static_cast<u64>(static_cast<i64>(state.max_width)));
        return true;

    case TTM_GETBUBBLESIZE: {
        // Learn: LOWORD is the width and HIWORD the height of the bubble the
        // tool would draw. Callers place the tip from that size.
        i32 width = 0;
        i32 height = 0;
        const auto snapshot = state;
        lock.unlock();
        m_bridge.measure_tooltip(snapshot.text, snapshot.max_width, width,
                                height);
        m.set_return((static_cast<u64>(static_cast<u16>(height)) << 16) |
                     static_cast<u16>(width));
        return true;
    }

    case TTM_ADJUSTRECT: {
        // Maps between the text rect and the window rect. The bubble's margin
        // is the same one measure_tooltip adds.
        if (lParam == 0) { m.set_return(0); return true; }
        const auto rect = static_cast<GuestAddr>(lParam);
        const i32 dx = wParam != 0 ? 6 : -6;
        const i32 dy = wParam != 0 ? 3 : -3;
        const i32 left = static_cast<i32>(m.read_u32(rect + 0).value_or(0));
        const i32 top = static_cast<i32>(m.read_u32(rect + 4).value_or(0));
        const i32 right = static_cast<i32>(m.read_u32(rect + 8).value_or(0));
        const i32 bottom = static_cast<i32>(m.read_u32(rect + 12).value_or(0));
        (void)m.write_u32(rect + 0, static_cast<u32>(left - dx));
        (void)m.write_u32(rect + 4, static_cast<u32>(top - dy));
        (void)m.write_u32(rect + 8, static_cast<u32>(right + dx));
        (void)m.write_u32(rect + 12, static_cast<u32>(bottom + dy));
        m.set_return(1);
        return true;
    }

    case TTM_SETDELAYTIME:
        state.delay_ms = static_cast<u32>(lParam & 0xFFFFu);
        m.set_return(0);
        return true;

    case TTM_GETDELAYTIME:
        m.set_return(state.delay_ms);
        return true;

    // Colours and margins are host presentation decisions here: the bubble is
    // a native macOS tooltip. Accept and report them so a caller that reads
    // back what it set is not surprised, but do not repaint from them.
    case TTM_SETTIPBKCOLOR:
    case TTM_SETTIPTEXTCOLOR:
    case TTM_SETMARGIN:
        m.set_return(0);
        return true;

    case TTM_RELAYEVENT:
        // TTM_RELAYEVENT drives non-tracking tools from relayed mouse
        // messages. A TTF_TRACK tool is positioned by TTM_TRACKPOSITION, so
        // this returns 0.
        m.set_return(0);
        return true;

    default:
        break;
    }
    return false;
}

} // namespace vitreo::winapi
