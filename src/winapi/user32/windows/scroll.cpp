// SPDX-License-Identifier: LGPL-2.1-only

/// @file scroll.cpp
/// @brief Scroll bars and scrolling a window's contents.
///
/// SetScrollInfo / GetScrollInfo and the older Pos/Range pairs, plus
/// ScrollWindow and ScrollWindowEx.
///
/// A Win32 scroll bar is part of the window's non-client area and its position
/// is state the guest sets and reads; AppKit's is a view that owns its own
/// state. What is here is the Win32 model, kept whether or not anything native
/// is drawing it.
///
/// Gathered from user32_ext.cpp. Closed in both directions before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/result.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::winapi {

void User32ExtManager::impl_EnableScrollBar(Marshaler& m) {
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!EnableScrollBar: per-arrow enable state and native presentation\n");
    });
    m.set_return(0u);
}

void User32ExtManager::impl_ScrollWindow(Marshaler& m) {
    u32 hwnd = static_cast<u32>(m.arg(0));
    i32 dx = static_cast<i32>(m.arg(1));
    i32 dy = static_cast<i32>(m.arg(2));
    (void)m.arg_ptr(3); // lpRect
    (void)m.arg_ptr(4); // lpClipRect
    VITREO_TRACE("[u32ext] ScrollWindow hwnd=0x%x dx=%d dy=%d -> TRUE\n", hwnd, dx, dy);
    m.set_return(1u);
}


namespace {
void clamp_scroll(windowing::ScrollBarState& s) {
    if (s.maximum < s.minimum) s.maximum = s.minimum;
    const i64 count = static_cast<i64>(s.maximum) - s.minimum + 1;
    s.page = static_cast<u32>(std::min<i64>(s.page, count));
    const i64 limit = static_cast<i64>(s.maximum) - std::max<i64>(s.page - (s.page != 0), 0);
    s.position = static_cast<i32>(std::clamp<i64>(s.position, s.minimum, limit));
}
void sync_scroll(User32ExtManager& manager, windowing::User32Host& host,
                 windowing::HwndManager& windows, u32 hwnd, u32 bar,
                 windowing::ScrollBarState state, bool redraw) {
    auto* window = windows.find(hwnd);
    if (!window) return;
    const u32 bit = bar ? windowing::WS::VSCROLL : windowing::WS::HSCROLL;
    const u32 previous = window->style;
    window->style = state.visible ? previous | bit : previous & ~bit;
    host.update_window_scrollbar(hwnd, bar, state, redraw);
    if (window->style != previous)
        (void)manager.apply_set_window_pos(hwnd, 0, 0, 0, 0, 0, 0x37); // frame changed, no move/size/z/activate
}
windowing::ScrollBarState* scroll_state(windowing::HwndManager& manager, u32 hwnd, u32 bar) {
    auto* window = manager.find(hwnd);
    if (!window || bar > 1) return nullptr;
    auto& s = window->scroll_bars[bar];
    if (!s.initialized) {
        s.initialized = true;
        s.visible = (window->style & (bar ? 0x00200000 : 0x00100000)) != 0;
    }
    return &s;
}
}

void User32ExtManager::impl_SetScrollRange(Marshaler& m) {
    if (m.arg(1) == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!SetScrollRange: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    const u32 hwnd = m.arg(0), bar = m.arg(1);
    auto* s = scroll_state(m_hwnd_mgr, hwnd, bar);
    if (!s) { m.set_return(0u); return; }
    s->minimum = static_cast<i32>(m.arg(2)); s->maximum = static_cast<i32>(m.arg(3));
    clamp_scroll(*s); s->visible = s->minimum != s->maximum;
    sync_scroll(*this, m_host, m_hwnd_mgr, hwnd, bar, *s, m.arg(4) != 0);
    m.set_return(1u);
}

void User32ExtManager::impl_GetScrollRange(Marshaler& m) {
    if (m.arg(1) == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetScrollRange: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    auto* s = scroll_state(m_hwnd_mgr, m.arg(0), m.arg(1));
    if (!s) { m.set_return(0u); return; }
    const bool ok = m.write_u32(m.arg_ptr(2), static_cast<u32>(s->minimum)).is_ok() &&
                    m.write_u32(m.arg_ptr(3), static_cast<u32>(s->maximum)).is_ok();
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_ShowScrollBar(Marshaler& m) {
    if (m.arg(1) == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!ShowScrollBar: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    const u32 hwnd = m.arg(0), bar = m.arg(1);
    if (bar != 0 && bar != 1 && bar != 3) { m.set_return(0u); return; }
    bool ok = true;
    for (u32 i = 0; i < 2; ++i) if (bar == i || bar == 3) {
        auto* s = scroll_state(m_hwnd_mgr, hwnd, i);
        if (!s) { ok = false; continue; }
        s->visible = m.arg(2) != 0;
        sync_scroll(*this, m_host, m_hwnd_mgr, hwnd, i, *s, true);
    }
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_GetScrollInfo(Marshaler& m) {
    if (m.arg(1) == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetScrollInfo: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    const u32 hwnd = m.arg(0), bar = m.arg(1);
    const GuestAddr p = m.arg_ptr(2);
    auto cb = m.read_u32(p); auto mask = m.read_u32(p + 4);
    if (!p || !cb.is_ok() || cb.value() != 28 || !mask.is_ok()) { m.set_return(0u); return; }
    auto* s = scroll_state(m_hwnd_mgr, hwnd, bar);
    windowing::HostScrollInfo native{};
    const bool native_control = (!s || !s->visible) && bar == 1 && m_host.get_control_scroll_info(hwnd, native);
    if (!s && !native_control) { m.set_return(0u); return; }
    i32 track = s ? s->position : native.n_track_pos;
    m_host.get_scrollbar_tracking(hwnd, bar, track);
    const u32 flags = mask.value();
    if (flags & 1) {
        (void)m.write_u32(p + 8, native_control ? native.n_min : s->minimum);
        (void)m.write_u32(p + 12, native_control ? native.n_max : s->maximum);
    }
    if (flags & 2) (void)m.write_u32(p + 16, native_control ? native.n_page : s->page);
    if (flags & 4) (void)m.write_u32(p + 20, native_control ? native.n_pos : s->position);
    if (flags & 16) (void)m.write_u32(p + 24, track);
    m.set_return(1u);
}

void User32ExtManager::impl_SetScrollInfo(Marshaler& m) {
    const u32 hwnd = m.arg(0), bar = m.arg(1);
    // Learn requires SBM_SETSCROLLINFO dispatch for SB_CTL. Until that
    // control path exists, fail instead of treating it as a window axis.
    if (bar == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!SetScrollInfo: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    const GuestAddr p = m.arg_ptr(2);
    auto cb = m.read_u32(p); auto mask = m.read_u32(p + 4);
    auto* s = scroll_state(m_hwnd_mgr, hwnd, bar);
    if (!s || !p || !cb.is_ok() || cb.value() != 28 || !mask.is_ok()) { m.set_return(0u); return; }
    const u32 flags = mask.value();
    if ((flags & ~0x1fu) || !m.read_u32(p + 24).is_ok()) { m.set_return(0u); return; }
    if (flags & 1) { s->minimum = static_cast<i32>(m.read_u32(p + 8).value()); s->maximum = static_cast<i32>(m.read_u32(p + 12).value()); }
    if (flags & 2) s->page = m.read_u32(p + 16).value();
    if (flags & 4) s->position = static_cast<i32>(m.read_u32(p + 20).value());
    clamp_scroll(*s);
    if (flags & 3) {
        const bool needed = static_cast<i64>(s->maximum) - s->minimum + 1 > std::max<u32>(s->page, 1);
        s->visible = needed || (flags & 8); s->enabled = needed;
    }
    sync_scroll(*this, m_host, m_hwnd_mgr, hwnd, bar, *s, m.arg(3) != 0);
    m.set_return(static_cast<u32>(s->position));
}

void User32ExtManager::impl_GetScrollPos(Marshaler& m) {
    if (m.arg(1) == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetScrollPos: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    auto* s = scroll_state(m_hwnd_mgr, m.arg(0), m.arg(1));
    m.set_return(static_cast<u32>(s ? s->position : 0));
}

void User32ExtManager::impl_SetScrollPos(Marshaler& m) {
    if (m.arg(1) == 2) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!SetScrollPos: SB_CTL control dispatch\n");
        });
        m.set_return(0u);
        return;
    }
    const u32 hwnd = m.arg(0), bar = m.arg(1);
    auto* s = scroll_state(m_hwnd_mgr, hwnd, bar);
    if (!s) { m.set_return(0u); return; }
    const i32 previous = s->position;
    s->position = static_cast<i32>(m.arg(2)); clamp_scroll(*s);
    sync_scroll(*this, m_host, m_hwnd_mgr, hwnd, bar, *s, m.arg(3) != 0);
    m.set_return(static_cast<u32>(previous));
}

} // namespace vitreo::winapi
