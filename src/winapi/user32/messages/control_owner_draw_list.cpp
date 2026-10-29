// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_owner_draw_list.cpp
/// @brief Owner-drawn LISTBOX emulation.

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/user32/messages/user32_case.hpp"
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

void MessageSendManager::paint_owner_draw_list(Marshaler& m, windowing::HWND hwnd, u64 supplied_dc) {
    auto* win = window_data(hwnd);
    if (!win || !m_gdi_manager || !win->owner_draw_list.redraw || win->owner_draw_list.painting) return;
    const auto items = win->owner_draw_list.items;
    const auto state = win->owner_draw_list;
    const auto parent = win->parent;
    const u32 id = win->ctrl_id;
    const i32 width = win->client_rect.width(), height = win->client_rect.height();
    if (width <= 0 || height <= 0) return;
    const GuestAddr scratch = m.memory().find_free_gap(0x3f000000, 4096);
    if (!scratch || !m.memory().map_region(scratch, 4096, MemoryProtection::ReadWrite).is_ok()) return;
    const auto dc = supplied_dc ? supplied_dc : m_gdi_manager->get_dc(hwnd);
    if (!dc) { (void)m.memory().unmap_region(scratch, 4096); return; }
    win->owner_draw_list.painting = true;
    // Every pixel of an owner-draw list is the owner's, so clicks on it are
    // the list's own WM_LBUTTONDOWN (selection, and an owner's subclass such
    // as a check list toggling its boxes), not the native list view's.
    if (!win->owner_draw_list.input_claimed) {
        win->owner_draw_list.input_claimed = true;
        m_bridge.set_control_owns_input(hwnd);
    }
    const bool wide = m.cpu_is_64bit();
    // The control's WM_SETFONT font is selected into the paint DC before the
    // owner draws, so TextOut in WM_DRAWITEM uses it (Ark's console asks for
    // a fixed-pitch face and aligns its columns with spaces).
    if (win->hfont != 0) (void)m_gdi_manager->select_object(dc, static_cast<Handle>(win->hfont));
    // A LISTBOX erases its client area with the brush the parent returns from
    // WM_CTLCOLORLISTBOX before the owner draws any row (default: the window
    // colour). Ark's console answers with a black brush; without this erase
    // the rows below the last line and the gaps around each string kept the
    // host view's colour and the text sat on dark boxes over a navy panel.
    if (auto* mgr = m_gdi_manager) {
        const u64 brush = send_message(parent, 0x134, dc, hwnd); // WM_CTLCOLORLISTBOX
        if (!window_data(hwnd)) { (void)m.memory().unmap_region(scratch, 4096); return; }
        u32 color = 0;
        if (brush != 0 && mgr->lookup_brush_color(static_cast<Handle>(brush), &color)) {
            (void)mgr->fill_rect_with_brush(dc, 0, 0, width, height, static_cast<Handle>(brush));
        } else {
            (void)mgr->fill_rect_gdi(dc, 0, 0, width, height, 0xFFFFFF); // COLOR_WINDOW
        }
        VITREO_TRACE("[ownerdraw] CTLCOLORLISTBOX brush=0x%llx color=0x%06x\n",
                     (unsigned long long)brush, color);
    }
    const bool variable = (win->style & 0x0020) != 0;  // LBS_OWNERDRAWVARIABLE
    const auto row_height = [&](i32 index) {
        return variable && index < static_cast<i32>(state.heights.size())
            ? state.heights[static_cast<usize>(index)] : state.item_height;
    };
    // itemState (Learn DRAWITEMSTRUCT): ODS_SELECTED for the selection,
    // ODS_FOCUS for the caret item while the list has the keyboard focus (a
    // single-selection list's caret is its selection, item 0 before any),
    // ODS_DISABLED for every item of a disabled list.
    constexpr u32 kOdsSelected = 0x1, kOdsDisabled = 0x4, kOdsFocus = 0x10;
    constexpr u32 kWsDisabled = 0x08000000;
    const bool focused = m_bridge.keyboard_focus_hwnd() == hwnd;
    const i32 caret = state.selection >= 0 ? state.selection : 0;
    const bool disabled = (win->style & kWsDisabled) != 0;
    i32 y = 0;
    for (i32 index = state.top; index < static_cast<i32>(items.size()); y += row_height(index), ++index) {
        if (y >= height) break;
        (void)m.write_u32(scratch, 2); // ODT_LISTBOX
        (void)m.write_u32(scratch + 4, id);
        (void)m.write_u32(scratch + 8, index);
        (void)m.write_u32(scratch + 12, 1); // ODA_DRAWENTIRE
        (void)m.write_u32(scratch + 16, (index == state.selection ? kOdsSelected : 0u) |
                                            (focused && index == caret ? kOdsFocus : 0u) |
                                            (disabled ? kOdsDisabled : 0u));
        const u32 rect = wide ? 40 : 28;
        if (wide) {
            (void)m.write_u64(scratch + 24, hwnd); (void)m.write_u64(scratch + 32, dc);
            (void)m.write_u64(scratch + 56, items[index]);
        } else {
            (void)m.write_u32(scratch + 20, hwnd); (void)m.write_u32(scratch + 24, dc);
            (void)m.write_u32(scratch + 44, items[index]);
        }
        (void)m.write_u32(scratch + rect, 0); (void)m.write_u32(scratch + rect + 4, y);
        (void)m.write_u32(scratch + rect + 8, width);
        (void)m.write_u32(scratch + rect + 12, std::min(height, y + row_height(index)));
        VITREO_TRACE("[ownerdraw] DRAWITEM id=%u itemID=%d data=0x%llx y=%d h=%d\n",
                     id, index, (unsigned long long)items[index], y,
                     row_height(index));
        (void)send_message(parent, 0x2b, id, scratch); // WM_DRAWITEM
        if (!window_data(hwnd)) break;
    }
    if (auto* current = window_data(hwnd)) current->owner_draw_list.painting = false;
    if (!supplied_dc) (void)m_gdi_manager->release_dc(hwnd, dc);
    (void)m.memory().unmap_region(scratch, 4096);
}

bool MessageSendManager::dispatch_owner_draw_list(Marshaler& m, windowing::HWND hwnd,
        u32 msg, u64 wParam, u64 lParam, bool force_native, bool ansi) {
    auto* win = window_data(hwnd);
    // LBS_OWNERDRAWFIXED (0x10) without LBS_OWNERDRAWVARIABLE (0x20).
    // LBS_HASSTRINGS (0x40) used to be excluded here, which was wrong: it says
    // the CONTROL stores the strings, not that the control draws them. Ark's
    // server console is LBS_OWNERDRAWFIXED|LBS_HASSTRINGS, so every line it
    // added went to the Cocoa listbox, the parent was never sent WM_DRAWITEM,
    // and the console's text area stayed blank.
    // LBS_OWNERDRAWVARIABLE (0x20) asks the owner for each item's height
    // (Learn: WM_MEASUREITEM is sent for every item added).
    constexpr u32 kLbsOwnerDrawMask = 0x0030, kLbsOwnerDrawVariable = 0x0020;
    if (!win || (win->control_class_name() != L"ListBox" && win->control_class_name() != L"listbox") ||
        (win->style & kLbsOwnerDrawMask) == 0 ||
        (!force_native && wndproc_for(hwnd) != 0)) return false;
    const bool variable = (win->style & kLbsOwnerDrawVariable) != 0;
    constexpr u32 kLbsHasStrings = 0x0040;
    const bool has_strings = (win->style & kLbsHasStrings) != 0;
    auto& state = win->owner_draw_list;
    const auto owner = win->parent;
    const auto control_id = win->ctrl_id;
    auto notify_deleted = [&](u32 item_index, u64 data) {
        const GuestAddr p = m.memory().find_free_gap(0x3f000000, 4096);
        if (!p || !m.memory().map_region(p, 4096, MemoryProtection::ReadWrite).is_ok()) return;
        (void)m.write_u32(p, 2); (void)m.write_u32(p + 4, control_id);
        (void)m.write_u32(p + 8, item_index);
        if (m.cpu_is_64bit()) { (void)m.write_u64(p + 16, hwnd); (void)m.write_u64(p + 24, data); }
        else { (void)m.write_u32(p + 12, hwnd); (void)m.write_u32(p + 16, data); }
        (void)send_message(owner, 0x2d, control_id, p); // WM_DELETEITEM
        (void)m.memory().unmap_region(p, 4096);
    };
    const i32 index = static_cast<i32>(wParam);
    i64 result = 0;
    bool repaint = false;
    const auto row_height = [&](i32 i) {
        return variable && i >= 0 && i < static_cast<i32>(state.heights.size())
            ? std::max(1, state.heights[static_cast<usize>(i)]) : std::max(1, state.item_height);
    };
    // Rows that fit from the current top; the list can never scroll past its
    // last full page.
    const auto visible_rows = [&]() {
        if (!variable) return std::max(1, win->client_rect.height() / std::max(1, state.item_height));
        i32 used = 0, rows = 0;
        for (i32 i = state.top; i < static_cast<i32>(state.items.size()); ++i) {
            used += row_height(i);
            if (used > win->client_rect.height()) break;
            ++rows;
        }
        return std::max(1, rows);
    };
    const auto max_top = [&]() {
        const i32 count = static_cast<i32>(state.items.size());
        if (!variable) return std::max(0, count - visible_rows());
        i32 used = 0, top = count;
        while (top > 0 && used + row_height(top - 1) <= win->client_rect.height()) {
            used += row_height(--top);
        }
        return std::min(top, std::max(0, count - 1));
    };
    /// y of @p i relative to the client top (rows above the top are negative).
    const auto row_y = [&](i32 i) {
        i32 y = 0;
        if (i >= state.top) { for (i32 k = state.top; k < i; ++k) y += row_height(k); }
        else { for (i32 k = i; k < state.top; ++k) y -= row_height(k); }
        return y;
    };
    /// Item under client point y, or -1.
    const auto item_at = [&](i32 y) {
        if (y < 0) return -1;
        i32 top_y = 0;
        for (i32 i = state.top; i < static_cast<i32>(state.items.size()); ++i) {
            if (y < top_y + row_height(i)) return i;
            top_y += row_height(i);
        }
        return -1;
    };
    const auto notify_parent = [&](u32 code) {
        constexpr u32 kLbsNotify = 0x0001;
        if (!(win->style & kLbsNotify) || owner == windowing::NULL_HWND) return;
        (void)send_message(owner, 0x111, (static_cast<u64>(code) << 16) | (control_id & 0xFFFFu), hwnd);
    };
    // A real LISTBOX owns its WS_VSCROLL bar: it publishes range/page/pos after
    // every change and scrolls itself on WM_VSCROLL / WM_MOUSEWHEEL. Ark's
    // server console relies on that — it only ever sends LB_SETTOPINDEX, so
    // without this the host scroller had no range and dragging it did nothing.
    constexpr u32 kWsVScroll = 0x00200000;
    const auto publish_scrollbar = [&]() {
        if (!(win->style & kWsVScroll)) return;
        auto& bar = win->scroll_bars[1];
        const i32 count = static_cast<i32>(state.items.size());
        bar.minimum = 0;
        bar.maximum = std::max(0, count - 1);
        bar.page = static_cast<u32>(visible_rows());
        bar.position = state.top;
        bar.initialized = true;
        bar.visible = true;
        bar.enabled = count > visible_rows();
        (void)m_bridge.update_window_scrollbar(hwnd, 1, bar, true);
    };
    switch (msg) {
    case 0x115: { // WM_VSCROLL from the host scroller (or the guest)
        const u32 code = static_cast<u32>(wParam & 0xffff);
        i32 track = static_cast<i32>((wParam >> 16) & 0xffff);
        (void)m_bridge.get_scrollbar_tracking(hwnd, 1, track);
        i32 top = state.top;
        switch (code) {
        case 0: top -= 1; break;                 // SB_LINEUP
        case 1: top += 1; break;                 // SB_LINEDOWN
        case 2: top -= visible_rows(); break;    // SB_PAGEUP
        case 3: top += visible_rows(); break;    // SB_PAGEDOWN
        case 4: case 5: top = track; break;      // SB_THUMBPOSITION / SB_THUMBTRACK
        case 6: top = 0; break;                  // SB_TOP
        case 7: top = max_top(); break;          // SB_BOTTOM
        default: break;                          // SB_ENDSCROLL
        }
        top = std::clamp(top, 0, max_top());
        repaint = top != state.top;
        state.top = top;
        break;
    }
    case 0x20a: { // WM_MOUSEWHEEL: three rows per notch, like the system default
        const i32 delta = static_cast<i16>((wParam >> 16) & 0xffff);
        const i32 top = std::clamp(state.top - (delta / 120) * 3, 0, max_top());
        repaint = top != state.top;
        state.top = top;
        break;
    }
    case 0x180: case 0x181: { // LB_ADDSTRING / LB_INSERTSTRING: lParam is item data.
        if (win->style & 2) { result = -1; break; } // Sorted owner-data needs WM_COMPAREITEM.
        const i32 at = msg == 0x180 || index < 0 ? static_cast<i32>(state.items.size()) : index;
        if (at > static_cast<i32>(state.items.size())) { result = -1; break; }
        // Measured on Windows (tools/oracle_probe/listbox_measure_order_probe):
        // the item is in the list BEFORE WM_MEASUREITEM, so the parent's
        // handler sees LB_GETCOUNT including it and LB_GETTEXT(itemID) its
        // text (VCL's TNewCheckListBox reads Items[itemID] there). itemData is
        // lParam, with or without LBS_HASSTRINGS.
        // With LBS_HASSTRINGS lParam points at the text and the item data
        // starts at 0 (the guest sets it separately with LB_SETITEMDATA);
        // without it lParam IS the item data.
        // The text is in the CALLER's encoding: Ark's console populates with
        // SendMessageA, so reading it as UTF-16 yielded an empty string for
        // every row and the console drew blank lines.
        std::wstring text;
        if (has_strings && lParam != 0) {
            auto str_res = ansi
                ? m.read_ansi_string(static_cast<GuestAddr>(lParam))
                : m.read_wide_string(static_cast<GuestAddr>(lParam));
            if (str_res.is_ok()) {
                text = message_send_utf8_to_wstring(str_res.value());
            }
        }
        state.strings.resize(state.items.size());
        state.heights.resize(state.items.size(), state.item_height);
        state.items.insert(state.items.begin() + at,
                           has_strings ? 0ull
                                       : (m.cpu_is_64bit() ? lParam
                                                           : static_cast<u32>(lParam)));
        state.strings.insert(state.strings.begin() + at, has_strings ? std::move(text) : std::wstring{});
        state.heights.insert(state.heights.begin() + at, state.item_height);
        if (!state.measured || variable) {
            const GuestAddr p = m.memory().find_free_gap(0x3f000000, 4096);
            if (!p || !m.memory().map_region(p, 4096, MemoryProtection::ReadWrite).is_ok()) {
                state.items.erase(state.items.begin() + at);
                state.strings.erase(state.strings.begin() + at);
                state.heights.erase(state.heights.begin() + at);
                result = -2; break;
            }
            // MEASUREITEMSTRUCT: CtlType, CtlID, itemID (the new item's index
            // for a variable list), itemWidth, itemHeight, itemData (lParam).
            (void)m.write_u32(p, 2); (void)m.write_u32(p + 4, win->ctrl_id);
            (void)m.write_u32(p + 8, variable ? static_cast<u32>(at) : 0u);
            (void)m.write_u32(p + 12, win->client_rect.width());
            (void)m.write_u32(p + 16, state.item_height);
            if (m.cpu_is_64bit()) (void)m.write_u64(p + 24, lParam);
            else (void)m.write_u32(p + 20, lParam);
            (void)send_message(win->parent, 0x2c, win->ctrl_id, p); // WM_MEASUREITEM
            if (!window_data(hwnd)) {
                (void)m.memory().unmap_region(p, 4096);
                m.set_return(static_cast<u64>(-1));
                return true;
            }
            const i32 measured_height = std::max(1, static_cast<i32>(m.read_u32(p + 16).value_or(16)));
            if (!variable) {
                state.item_height = measured_height;
                std::fill(state.heights.begin(), state.heights.end(), measured_height);
            } else if (at < static_cast<i32>(state.heights.size())) {
                state.heights[at] = measured_height;
            }
            state.measured = true;
            (void)m.memory().unmap_region(p, 4096);
        }
        if (state.selection >= at) ++state.selection;
        VITREO_TRACE("[ownerdraw] LB_ADD hwnd=0x%x at=%d n=%zu text=\"%.80s\"\n",
                     hwnd, at, state.items.size(),
                     has_strings ? wstring_to_utf8(state.strings[at]).c_str() : "<data>");
        result = at; repaint = true; break;
    }
    case 0x182:
        if (index < 0 || index >= static_cast<i32>(state.items.size())) { result = -1; break; }
        {
        const auto removed = state.items[index];
        state.items.erase(state.items.begin() + index);
        if (index < static_cast<i32>(state.heights.size())) {
            state.heights.erase(state.heights.begin() + index);
        }
        if (index < static_cast<i32>(state.strings.size())) {
            state.strings.erase(state.strings.begin() + index);
        }
        if (state.selection == index) state.selection = -1;
        else if (state.selection > index) --state.selection;
        result = state.items.size();
        notify_deleted(index, removed);
        repaint = true; break;
        }
    case 0x184: {
        const auto removed = std::move(state.items);
        state.items.clear(); state.heights.clear(); state.selection = -1; state.top = 0;
        for (u32 i = 0; i < removed.size() && window_data(hwnd); ++i) notify_deleted(i, removed[i]);
        repaint = true; break;
    }
    case 0x189: { // LB_GETTEXT: lParam is a caller buffer, result is the length
        if (!has_strings || index < 0 ||
            index >= static_cast<i32>(state.strings.size()) || lParam == 0) {
            result = -1; break;
        }
        const std::string utf8 =
            wstring_to_utf8(state.strings[index]);
        if (ansi) {
            if (m.write_ansi_string(static_cast<GuestAddr>(lParam), utf8).is_err()) {
                result = -1; break;
            }
        } else if (m.write_wide_string(static_cast<GuestAddr>(lParam), utf8).is_err()) {
            result = -1; break;
        }
        result = static_cast<i64>(state.strings[index].size());
        break;
    }
    case 0x18a: // LB_GETTEXTLEN
        if (has_strings) {
            result = index >= 0 && index < static_cast<i32>(state.strings.size())
                ? static_cast<i64>(state.strings[index].size()) : -1;
        } else {
            result = index >= 0 && index < static_cast<i32>(state.items.size()) ? 4 : -1;
        }
        break;
    case 0x18b: result = state.items.size(); break;
    case 0x87: // WM_GETDLGCODE (Learn, list box default processing)
        result = 0x0001 | 0x0080; // DLGC_WANTARROWS | DLGC_WANTCHARS
        break;
    case 0x18f: case 0x1a2: case 0x18c: { // LB_FINDSTRING / LB_FINDSTRINGEXACT / LB_SELECTSTRING
        // Learn: the search starts after item wParam (-1: the first item),
        // wraps to the top and stops at wParam; strings compare case-
        // insensitively, FINDSTRING / SELECTSTRING as a prefix. Without
        // LBS_HASSTRINGS (unsorted) lParam is compared with each item's data.
        // LB_ERR when nothing matches; SELECTSTRING then keeps the selection.
        // Measured (listbox_find_probe): a start outside 0..count-1 searches
        // from the first item; an empty string matches nothing.
        const i32 count = static_cast<i32>(state.items.size());
        result = -1;
        if (count == 0) break;
        const bool exact = msg == 0x1a2;
        std::wstring wanted;
        if (has_strings) {
            if (lParam == 0) break;
            auto str = ansi ? m.read_ansi_string(static_cast<GuestAddr>(lParam))
                            : m.read_wide_string(static_cast<GuestAddr>(lParam));
            if (str.is_err()) break;
            wanted = message_send_utf8_to_wstring(str.value());
            if (wanted.empty()) break;
        }
        const auto fold = [](wchar_t c) {
            return c <= 0xFFFF ? static_cast<wchar_t>(user32_case::upper(static_cast<u16>(c))) : c;
        };
        const auto matches = [&](i32 i) {
            if (!has_strings) {
                const u64 data = m.cpu_is_64bit() ? lParam : static_cast<u32>(lParam);
                return state.items[static_cast<usize>(i)] == data;
            }
            const std::wstring& text = state.strings[static_cast<usize>(i)];
            if (exact ? text.size() != wanted.size() : text.size() < wanted.size()) return false;
            for (usize k = 0; k < wanted.size(); ++k) {
                if (fold(text[k]) != fold(wanted[k])) return false;
            }
            return true;
        };
        const i32 start = index < 0 || index >= count ? 0 : index + 1;
        for (i32 n = 0; n < count; ++n) {
            const i32 i = (start + n) % count;
            if (matches(i)) { result = i; break; }
        }
        if (msg == 0x18c && result >= 0 && result != state.selection) {
            state.selection = static_cast<i32>(result);
            if (state.selection < state.top) state.top = state.selection;
            while (state.selection >= state.top + visible_rows() && state.top < max_top()) ++state.top;
            repaint = true;
        }
        break;
    }
    case 0x188: result = state.selection; break;
    case 0x186:
        if (index < -1 || index >= static_cast<i32>(state.items.size())) { result = -1; break; }
        state.selection = index; result = index; repaint = true; break;
    case 0x199: // LB_GETITEMDATA
        result = index >= 0 && index < static_cast<i32>(state.items.size()) ? static_cast<i64>(state.items[index]) : -1; break;
    case 0x19a: // LB_SETITEMDATA
        if (index < 0 || index >= static_cast<i32>(state.items.size())) { result = -1; break; }
        state.items[index] = m.cpu_is_64bit() ? lParam : static_cast<u32>(lParam); repaint = true; break;
    case 0x1a0: { // LB_SETITEMHEIGHT: wParam is the item for a variable list
        const i32 h = static_cast<i32>(lParam & 0xffff);
        if (h > 255) { result = -1; break; }  // Learn: height > 255 is LB_ERR
        if (variable) {
            if (index < 0 || index >= static_cast<i32>(state.items.size())) { result = -1; break; }
            state.heights.resize(state.items.size(), state.item_height);
            state.heights[static_cast<usize>(index)] = std::max(1, h);
        } else {
            state.item_height = std::max(1, h);
        }
        repaint = true; break;
    }
    case 0x1a1: // LB_GETITEMHEIGHT
        if (variable) {
            result = index >= 0 && index < static_cast<i32>(state.items.size()) ? row_height(index) : -1;
        } else {
            result = state.item_height;
        }
        break;
    case 0x198: { // LB_GETITEMRECT
        if (index < 0 || index >= static_cast<i32>(state.items.size())) { result = -1; break; }
        const i32 y = row_y(index);
        (void)m.write_u32(lParam, 0); (void)m.write_u32(lParam + 4, y);
        (void)m.write_u32(lParam + 8, win->client_rect.width());
        (void)m.write_u32(lParam + 12, y + row_height(index)); break;
    }
    case 0x1a9: { // LB_ITEMFROMPOINT: LOWORD nearest item, HIWORD 1 when outside one
        const i32 x = static_cast<i16>(lParam & 0xffff);
        const i32 y = static_cast<i16>((lParam >> 16) & 0xffff);
        const i32 count = static_cast<i32>(state.items.size());
        const i32 hit = item_at(y);
        const bool inside = hit >= 0 && x >= 0 && x < win->client_rect.width();
        i32 nearest = hit;
        if (nearest < 0) nearest = y < 0 ? state.top : std::max(0, count - 1);
        result = static_cast<i64>((static_cast<u32>(inside ? 0 : 1) << 16) |
                                  (static_cast<u32>(std::max(0, nearest)) & 0xffff));
        break;
    }
    case 0x201: case 0x203: { // WM_LBUTTONDOWN / WM_LBUTTONDBLCLK
        const i32 hit = item_at(static_cast<i16>((lParam >> 16) & 0xffff));
        if (hit >= 0 && hit != state.selection) {
            state.selection = hit;
            repaint = true;
            if (repaint) { publish_scrollbar(); paint_owner_draw_list(m, hwnd); repaint = false; }
            notify_parent(1);  // LBN_SELCHANGE
        }
        if (hit >= 0 && msg == 0x203) notify_parent(2);  // LBN_DBLCLK
        break;
    }
    case 0x100: { // WM_KEYDOWN: keyboard selection
        const i32 count = static_cast<i32>(state.items.size());
        if (count == 0) break;
        i32 next = state.selection;
        switch (static_cast<u32>(wParam)) {
        case 0x26: next = std::max(0, state.selection - 1); break;              // VK_UP
        case 0x28: next = std::min(count - 1, state.selection + 1); break;      // VK_DOWN
        case 0x24: next = 0; break;                                              // VK_HOME
        case 0x23: next = count - 1; break;                                      // VK_END
        case 0x21: next = std::max(0, state.selection - visible_rows()); break; // VK_PRIOR
        case 0x22: next = std::min(count - 1, std::max(0, state.selection) + visible_rows()); break; // VK_NEXT
        default: break;
        }
        if (next != state.selection && next >= 0) {
            state.selection = next;
            if (next < state.top) state.top = next;
            while (next >= state.top + visible_rows() && state.top < max_top()) ++state.top;
            publish_scrollbar();
            paint_owner_draw_list(m, hwnd);
            notify_parent(1);  // LBN_SELCHANGE
        }
        break;
    }
    case 0x18e: result = state.top; break;
    case 0x197: { // LB_SETTOPINDEX
        // Windows scrolls until the item is at the top OR the maximum scroll
        // range is reached — you cannot scroll past the last FULL page. The
        // old clamp allowed top = count-1, so Ark's console (which scrolls to
        // the bottom after every line) painted a single row at y=0 and left
        // the rest of the client area showing stale pixels from earlier
        // repaints: the same line appeared to repeat down the whole window.
        const i32 count = static_cast<i32>(state.items.size());
        (void)count;
        state.top = std::clamp(index, 0, max_top());
        repaint = true; break;
    }
    case 0x1a8: result = std::max<u64>(wParam, state.items.size()); break; // LB_INITSTORAGE
    case 0xb: state.redraw = wParam != 0; repaint = state.redraw; break;
    case 0xf: case 0x5: repaint = true; break;
    case 0x7: case 0x8: { // WM_SETFOCUS / WM_KILLFOCUS: redraw the caret's focus state
        // LBN_SETFOCUS (4) / LBN_KILLFOCUS (5) go to the parent whatever the
        // style (Learn: only SELCHANGE / DBLCLK / SELCANCEL need LBS_NOTIFY).
        if (owner != windowing::NULL_HWND) {
            const u32 code = msg == 0x7 ? 4u : 5u;
            (void)send_message(owner, 0x111, (static_cast<u64>(code) << 16) | (control_id & 0xFFFFu), hwnd);
            if (!window_data(hwnd)) { m.set_return(0); return true; }
        }
        repaint = true;
        break;
    }
    case 0x317: case 0x318: paint_owner_draw_list(m, hwnd, wParam); break;
    default: return false;
    }
    if (repaint) {
        publish_scrollbar();
        paint_owner_draw_list(m, hwnd);
    }
    m.set_return(static_cast<u64>(result));
    return true;
}

} // namespace vitreo::winapi
