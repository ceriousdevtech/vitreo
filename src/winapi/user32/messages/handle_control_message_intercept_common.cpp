// SPDX-License-Identifier: LGPL-2.1-only

/// @file message_send.cpp
/// @brief PostQuitMessage/PostMessageW/SendMessageW implementation (WIN32-055)

#include "message_send.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <algorithm>
#include <string>
#include <cstdio>
#include <vector>

/// @file handle_control_message_intercept_common.cpp
/// @brief handle_control_message — intercept common control messages
///
/// Closed cut from message_send.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// handle_control_message — intercept common control messages
// -----------------------------------------------------------------------------

bool MessageSendManager::show_static_icon(windowing::HWND hwnd, u64 icon) {
    // An SS_ICON static draws its icon (Learn: STM_SETIMAGE / STM_SETICON).
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    if (!m_icon_rgba_query || !m_icon_rgba_query(icon, w, h, rgba)) return false;
    return m_bridge.set_control_image_rgba(hwnd, w, h, rgba);
}

bool MessageSendManager::handle_control_message(
    windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam, u64& result
) {
    const auto* win = m_hwnd_mgr.find(hwnd);
    const auto class_eq = [&](std::wstring_view want) -> bool {
        if (!win || win->control_class_name().size() != want.size()) return false;
        for (usize i = 0; i < want.size(); ++i) {
            auto fold = [](wchar_t c) {
                return (c >= L'A' && c <= L'Z')
                           ? static_cast<wchar_t>(c - L'A' + L'a')
                           : c;
            };
            if (fold(win->control_class_name()[i]) != fold(want[i])) return false;
        }
        return true;
    };
    const bool is_progress = class_eq(L"msctls_progress32");
    const bool is_rebar = class_eq(L"rebarwindow32");
    const bool is_toolbar = class_eq(L"toolbarwindow32");

    // Progress bar messages added in comctl32 v6 (Learn PBM_*). Colors only
    // affect classic (unthemed) drawing; the host renders a themed bar, as
    // Windows does with visual styles, so they are state the caller reads
    // back. Handled before the switch: several share RB_* values.
    if (is_progress) {
        switch (msg) {
        case PBM_GETRANGE: {
            // wParam TRUE: low limit, FALSE: high limit. PBRANGE is filled by
            // dispatch_progress_range when a Marshaler is available.
            std::lock_guard lock(m_control_state_mutex);
            const auto& state = m_progress_state[hwnd];
            result = static_cast<u32>(wParam != 0 ? state.minimum : state.maximum);
            if (lParam != 0) {
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!PBM_GETRANGE: PBRANGE output on a cross-thread send\n");
                });
            }
            return true;
        }
        case PBM_SETBARCOLOR:
        case PBM_SETBKCOLOR: {
            std::lock_guard lock(m_control_state_mutex);
            auto& state = m_progress_state[hwnd];
            u32& color = (msg == PBM_SETBARCOLOR) ? state.bar_color : state.bk_color;
            result = color;  // previous color, or CLR_DEFAULT
            color = static_cast<u32>(lParam);
            return true;
        }
        case PBM_GETBARCOLOR:
        case PBM_GETBKCOLOR: {
            std::lock_guard lock(m_control_state_mutex);
            const auto& state = m_progress_state[hwnd];
            result = (msg == PBM_GETBARCOLOR) ? state.bar_color : state.bk_color;
            return true;
        }
        case PBM_GETSTEP: {
            std::lock_guard lock(m_control_state_mutex);
            result = static_cast<u32>(m_progress_state[hwnd].step);
            return true;
        }
        case PBM_SETSTATE: {
            std::lock_guard lock(m_control_state_mutex);
            auto& state = m_progress_state[hwnd];
            result = state.state;  // previous state
            state.state = static_cast<u32>(wParam);
            if (state.state != PBST_NORMAL) {
                static std::once_flag once;
                std::call_once(once, [] {
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED comctl32!PBM_SETSTATE: error/paused bar coloring (state is kept)\n");
                });
            }
            return true;
        }
        case PBM_GETSTATE: {
            std::lock_guard lock(m_control_state_mutex);
            result = m_progress_state[hwnd].state;
            return true;
        }
        default:
            break;
        }
    }

    // Handle progress bar messages
    switch (msg) {
    case BM_SETCHECK: {
        u32 check = static_cast<u32>(wParam);
        VITREO_TRACE( "[send] BM_SETCHECK hwnd=0x%x check=%u\n", hwnd, check);
        {
            std::lock_guard lock(m_control_state_mutex);
            m_button_check_state[hwnd] = check;
        }
        // Update Cocoa button state
        m_bridge.set_button_state(hwnd, check);
        result = 0;
        return true;
    }
    case BM_GETCHECK: {
        // Prefer live Cocoa NSButton state. BS_AUTORADIOBUTTON clicks update
        // AppKit before BN_CLICKED; the host-side map alone stays stale and
        // BM_GETCHECK then reports the pre-click state.
        u32 check = BST_UNCHECKED;
        const bool native = m_bridge.try_get_button_check(hwnd, check);
        std::lock_guard lock(m_control_state_mutex);
        if (native) {
            // The native button is two-state: a checked 3-state button set to
            // BST_INDETERMINATE stays indeterminate.
            auto it = m_button_check_state.find(hwnd);
            if (check != BST_UNCHECKED && it != m_button_check_state.end() &&
                it->second == BST_INDETERMINATE) {
                check = BST_INDETERMINATE;
            }
            m_button_check_state[hwnd] = check;
        } else {
            auto it = m_button_check_state.find(hwnd);
            check = (it != m_button_check_state.end()) ? it->second : BST_UNCHECKED;
        }
        VITREO_TRACE( "[send] BM_GETCHECK hwnd=0x%x -> %u\n", hwnd, check);
        result = check;
        return true;
    }
    case BM_GETSTATE: {
        u32 check = BST_UNCHECKED;
        const bool native = m_bridge.try_get_button_check(hwnd, check);
        std::lock_guard lock(m_control_state_mutex);
        if (native) {
            m_button_check_state[hwnd] = check;
        } else {
            auto it = m_button_check_state.find(hwnd);
            check = (it != m_button_check_state.end()) ? it->second : BST_UNCHECKED;
        }
        result = check;
        return true;
    }
    case BM_SETSTATE: {
        // Pushed state — just acknowledge
        result = 0;
        return true;
    }
    case BM_SETSTYLE: {
        // Style change — just acknowledge
        result = 0;
        return true;
    }
    case 0x0170: { // STM_SETICON
        // wParam = HICON. Some SS_BITMAP skins also arrive as STM_SETICON on
        // owner popups; try a Cocoa bitmap apply when the handle is a
        // registered BMP, otherwise acknowledge.
        u32 handle = static_cast<u32>(wParam);
        u32 prev_handle = 0;
        {
            std::lock_guard lock(m_control_state_mutex);
            auto& state = m_static_image_state[hwnd];
            prev_handle = state.icon;
            if (handle != 0) state.icon = handle;
        }
        VITREO_TRACE("[send] STM_SETICON hwnd=0x%x handle=0x%x\n",
                     hwnd, handle);
        if (handle != 0 && !show_static_icon(hwnd, handle)) {
            (void)m_bridge.set_control_bitmap(hwnd, handle);
        }
        result = prev_handle;
        return true;
    }
    case 0x0172: { // STM_SETIMAGE
        // wParam = IMAGE_BITMAP (0) or IMAGE_ICON (1), lParam = handle
        u32 image_type = static_cast<u32>(wParam);
        u32 handle = static_cast<u32>(lParam);
        u32 prev_handle = 0;
        {
            std::lock_guard lock(m_control_state_mutex);
            const auto& state = m_static_image_state[hwnd];
            prev_handle = (image_type == 1) ? state.icon : state.bitmap;
        }
        VITREO_TRACE( "[send] STM_SETIMAGE hwnd=0x%x type=%u handle=0x%x\n",
                     hwnd, image_type, handle);

        bool applied = false;
        if (image_type == 0) {
            // Compatibility: some installers briefly send NULL after setting an
            // SS_BITMAP image (e.g. transient load failure during startup).
            // Keep existing image instead of clearing to avoid flicker-out.
            if (handle == 0) {
                result = prev_handle;
                return true;
            }
            applied = m_bridge.set_control_bitmap(hwnd, handle);
            if (!applied) {
                // If the handle is unresolved, keep current image and report
                // success to prevent guests from clearing/retrying aggressively.
                result = prev_handle;
                return true;
            }
            std::lock_guard lock(m_control_state_mutex);
            m_static_image_state[hwnd].bitmap = handle;
        } else if (image_type == 1) {
            {
                std::lock_guard lock(m_control_state_mutex);
                m_static_image_state[hwnd].icon = handle;
            }
            if (handle == 0) {
                (void)m_bridge.set_control_image_rgba(hwnd, 0, 0, {});
            } else {
                (void)show_static_icon(hwnd, handle);
            }
        }

        // Win32 returns the previous handle, not BOOL.
        result = prev_handle;
        return true;
    }
    case 0x0173: { // STM_GETIMAGE
        // wParam = IMAGE_BITMAP (0) or IMAGE_ICON (1)
        u32 image_type = static_cast<u32>(wParam);
        std::lock_guard lock(m_control_state_mutex);
        auto it = m_static_image_state.find(hwnd);
        if (it == m_static_image_state.end()) {
            result = 0;
            return true;
        }
        result = (image_type == 1) ? it->second.icon : it->second.bitmap;
        VITREO_TRACE("[send] STM_GETIMAGE hwnd=0x%x type=%u -> 0x%llx\n",
                     hwnd, image_type, static_cast<unsigned long long>(result));
        return true;
    }
    case PBM_SETRANGE: {
        if (!is_progress) break;
        // LOWORD(lParam) = min, HIWORD(lParam) = max (16-bit values)
        u16 min_val = static_cast<u16>(lParam & 0xFFFF);
        u16 max_val = static_cast<u16>((lParam >> 16) & 0xFFFF);
        VITREO_TRACE( "[send] PBM_SETRANGE hwnd=0x%x min=%u max=%u\n",
                     hwnd, min_val, max_val);
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_progress_state[hwnd];
        result = static_cast<u16>(state.minimum) |
                 (static_cast<u64>(static_cast<u16>(state.maximum)) << 16);
        state.minimum = min_val;
        state.maximum = max_val;
        state.position = std::clamp(state.position, state.minimum, state.maximum);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.set_progress_range(hwnd, static_cast<double>(snapshot.minimum),
                                    static_cast<double>(snapshot.maximum));
        m_bridge.set_progress_value(hwnd, static_cast<double>(snapshot.position));
        return true;
    }
    case PBM_SETRANGE32: {
        if (!is_progress) break;
        // wParam = min, lParam = max (32-bit values)
        i32 min_val = static_cast<i32>(wParam);
        i32 max_val = static_cast<i32>(lParam);
        VITREO_TRACE( "[send] PBM_SETRANGE32 hwnd=0x%x min=%d max=%d\n",
                     hwnd, min_val, max_val);
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_progress_state[hwnd];
        result = static_cast<u16>(state.minimum) |
                 (static_cast<u64>(static_cast<u16>(state.maximum)) << 16);
        state.minimum = min_val;
        state.maximum = max_val;
        state.position = std::clamp(state.position, state.minimum, state.maximum);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.set_progress_range(hwnd, static_cast<double>(snapshot.minimum),
                                    static_cast<double>(snapshot.maximum));
        m_bridge.set_progress_value(hwnd, static_cast<double>(snapshot.position));
        return true;
    }
    case PBM_SETPOS: {
        if (!is_progress) break;
        // wParam = new position
        i32 pos = static_cast<i32>(wParam);
        VITREO_TRACE( "[send] PBM_SETPOS hwnd=0x%x pos=%d\n", hwnd, pos);
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_progress_state[hwnd];
        result = static_cast<u32>(state.position);
        state.position = std::clamp(pos, state.minimum, state.maximum);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.set_progress_value(hwnd, static_cast<double>(snapshot.position));
        return true;
    }
    case PBM_GETPOS: {
        if (!is_progress) break;
        std::lock_guard lock(m_control_state_mutex);
        result = static_cast<u32>(m_progress_state[hwnd].position);
        return true;
    }
    case PBM_SETMARQUEE: {
        // 0x040A is also RB_INSERTBANDW — only progress bars use PBM_SETMARQUEE.
        if (is_rebar) {
            // Learn RB_INSERTBAND: nonzero on success. Track band count so
            // RB_GETBANDCOUNT / RB_GETBARHEIGHT drive host layout.
            (void)wParam;
            (void)lParam;
            std::lock_guard lock(m_control_state_mutex);
            auto& state = m_rebar_state[hwnd];
            ++state.band_count;
            VITREO_TRACE("[send] RB_INSERTBANDW hwnd=0x%x bands=%u\n",
                         hwnd, state.band_count);
            result = 1;
            return true;
        }
        if (!is_progress) break;
        // wParam = enable (BOOL: 1 to start marquee, 0 to stop)
        // lParam = update period in ms (we ignore — Cocoa drives its own animation)
        bool enable = (wParam != 0);
        VITREO_TRACE("[send] PBM_SETMARQUEE hwnd=0x%x enable=%d period=%llu\n",
                     hwnd, enable ? 1 : 0,
                     static_cast<unsigned long long>(lParam));
        m_bridge.set_progress_marquee(hwnd, enable);
        result = 0;
        return true;
    }
    case PBM_DELTAPOS: {
        if (!is_progress) break;
        // wParam = delta to add to current position
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_progress_state[hwnd];
        const i32 delta = static_cast<i32>(wParam);
        result = static_cast<u32>(state.position);
        const i64 next = static_cast<i64>(state.position) + delta;
        state.position = static_cast<i32>(std::clamp(
            next, static_cast<i64>(state.minimum),
            static_cast<i64>(state.maximum)));
        VITREO_TRACE( "[send] PBM_DELTAPOS hwnd=0x%x delta=%d pos=%d\n",
                     hwnd, delta, state.position);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.set_progress_value(hwnd, static_cast<double>(snapshot.position));
        return true;
    }
    case PBM_SETSTEP: {
        // 0x0404 is also RB_SETBARINFO.
        if (is_rebar) {
            (void)wParam;
            (void)lParam;
            VITREO_TRACE("[send] RB_SETBARINFO hwnd=0x%x\n", hwnd);
            result = 1;
            return true;
        }
        if (!is_progress) break;
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_progress_state[hwnd];
        result = static_cast<u32>(state.step);
        state.step = static_cast<i32>(wParam);
        VITREO_TRACE("[send] PBM_SETSTEP hwnd=0x%x step=%d\n", hwnd, state.step);
        return true;
    }
    case PBM_STEPIT: {
        if (!is_progress) break;
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_progress_state[hwnd];
        result = static_cast<u32>(state.position);
        const i64 next = static_cast<i64>(state.position) + state.step;
        if (next > state.maximum) {
            state.position = state.minimum;
        } else if (next < state.minimum) {
            state.position = state.maximum;
        } else {
            state.position = static_cast<i32>(next);
        }
        VITREO_TRACE("[send] PBM_STEPIT hwnd=0x%x pos=%d step=%d\n",
                     hwnd, state.position, state.step);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.set_progress_value(hwnd, static_cast<double>(snapshot.position));
        return true;
    }
    case RB_SETBANDINFOW: {
        if (!is_rebar) break;
        (void)wParam;
        (void)lParam;
        VITREO_TRACE("[send] RB_SETBANDINFOW hwnd=0x%x\n", hwnd);
        result = 1;
        return true;
    }
    case RB_GETBANDCOUNT: {
        if (!is_rebar) break;
        std::lock_guard lock(m_control_state_mutex);
        result = m_rebar_state[hwnd].band_count;
        VITREO_TRACE("[send] RB_GETBANDCOUNT hwnd=0x%x -> %llu\n",
                     hwnd, static_cast<unsigned long long>(result));
        return true;
    }
    case RB_GETROWCOUNT: {
        if (!is_rebar) break;
        std::lock_guard lock(m_control_state_mutex);
        const auto& state = m_rebar_state[hwnd];
        result = state.band_count > 0 ? 1u : 0u;
        VITREO_TRACE("[send] RB_GETROWCOUNT hwnd=0x%x -> %llu\n",
                     hwnd, static_cast<unsigned long long>(result));
        return true;
    }
    case RB_GETBARHEIGHT: {
        if (!is_rebar) break;
        std::lock_guard lock(m_control_state_mutex);
        const auto& state = m_rebar_state[hwnd];
        const u32 rows = state.band_count > 0 ? 1u : 0u;
        result = static_cast<u64>(state.row_height) * rows;
        VITREO_TRACE("[send] RB_GETBARHEIGHT hwnd=0x%x -> %llu\n",
                     hwnd, static_cast<unsigned long long>(result));
        return true;
    }
    case RB_SHOWBAND: {
        if (!is_rebar) break;
        VITREO_TRACE("[send] RB_SHOWBAND hwnd=0x%x index=%llu show=%llu\n",
                     hwnd, static_cast<unsigned long long>(wParam),
                     static_cast<unsigned long long>(lParam));
        result = 1;
        return true;
    }
    case TB_BUTTONSTRUCTSIZE: {
        if (!is_toolbar) break;
        VITREO_TRACE("[send] TB_BUTTONSTRUCTSIZE hwnd=0x%x cb=%llu\n",
                     hwnd, static_cast<unsigned long long>(wParam));
        result = 0;
        return true;
    }
    case TB_SETBUTTONSIZE: {
        if (!is_toolbar) break;
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_toolbar_state[hwnd];
        state.button_cx = static_cast<i32>(lParam & 0xFFFF);
        state.button_cy = static_cast<i32>((lParam >> 16) & 0xFFFF);
        if (state.button_cx <= 0) state.button_cx = 24;
        if (state.button_cy <= 0) state.button_cy = 22;
        // Standard bitmap buttons need room for their chrome as well as the
        // image (Learn CToolBar::SetSizes: at least 7 extra pixels horizontally
        // and 6 vertically). This is independent of BTNS_AUTOSIZE padding.
        state.button_cx = std::max(state.button_cx, state.bitmap_cx + 7);
        state.button_cy = std::max(state.button_cy, state.bitmap_cy + 6);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.toolbar_set_size(hwnd, snapshot.button_cx, snapshot.button_cy);
        VITREO_TRACE("[send] TB_SETBUTTONSIZE hwnd=0x%x %dx%d\n",
                     hwnd, snapshot.button_cx, snapshot.button_cy);
        result = 1;
        return true;
    }
    case TB_SETBITMAPSIZE: {
        if (!is_toolbar) break;
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_toolbar_state[hwnd];
        const i32 width = lParam & 0xffff, height = (lParam >> 16) & 0xffff;
        if (width <= 0 || height <= 0) { result = 0; return true; }
        state.bitmap_cx = width; state.bitmap_cy = height;
        state.button_cx = std::max(state.button_cx, width + 7);
        state.button_cy = std::max(state.button_cy, height + 6);
        const auto snapshot = state;
        lock.unlock();
        m_bridge.toolbar_set_size(hwnd, snapshot.button_cx, snapshot.button_cy);
        result = 1;
        return true;
    }
    case 0x456: // TB_GETPADDING
    case 0x457: { // TB_SETPADDING
        if (!is_toolbar) break;
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_toolbar_state[hwnd];
        result = static_cast<u32>(state.padding_cx) | (static_cast<u32>(state.padding_cy) << 16);
        if (msg == 0x457) {
            state.padding_cx = lParam & 0xffff;
            state.padding_cy = (lParam >> 16) & 0xffff;
        }
        return true;
    }
    case TB_AUTOSIZE: {
        if (!is_toolbar) break;
        VITREO_TRACE("[send] TB_AUTOSIZE hwnd=0x%x\n", hwnd);
        auto* toolbar = m_hwnd_mgr.find(hwnd);
        auto* parent = toolbar ? m_hwnd_mgr.find(toolbar->parent) : nullptr;
        if (toolbar && parent) {
            ToolbarState state;
            {
                std::lock_guard lock(m_control_state_mutex);
                state = m_toolbar_state[hwnd];
            }
            const i32 width = parent->client_rect.right - parent->client_rect.left;
            const i32 height = state.button_cy + 4;
            toolbar->rect = {0, 0, width, height};
            toolbar->client_rect = toolbar->rect;
            m_bridge.set_control_frame(hwnd, toolbar->rect);
        }
        result = 0;
        return true;
    }
    case TB_GETBUTTONSIZE: {
        if (!is_toolbar) break;
        std::lock_guard lock(m_control_state_mutex);
        const auto& state = m_toolbar_state[hwnd];
        result = static_cast<u32>(state.button_cx & 0xFFFF) |
                 (static_cast<u32>(state.button_cy & 0xFFFF) << 16);
        VITREO_TRACE("[send] TB_GETBUTTONSIZE hwnd=0x%x -> %dx%d\n",
                     hwnd, state.button_cx, state.button_cy);
        return true;
    }
    case TB_INSERTBUTTONA:
    case TB_ADDBUTTONSW: {
        if (!is_toolbar) break;
        std::unique_lock lock(m_control_state_mutex);
        auto& state = m_toolbar_state[hwnd];
        const u32 n = msg == TB_ADDBUTTONSW
                          ? static_cast<u32>(wParam)
                          : 1u;
        state.button_count += n == 0 ? 1u : n;
        VITREO_TRACE("[send] TB_ADDBUTTONS hwnd=0x%x n=%u total=%u\n",
                     hwnd, n, state.button_count);
        result = 1;
        return true;
    }
    case WM_SETFONT: {
        // wParam = HFONT handle, lParam = redraw flag
        auto font_handle = static_cast<Handle>(wParam);
        if (auto* win = window_data(hwnd)) win->hfont = wParam;
        const auto* fi = GdiManager::lookup_font(font_handle);
        if (fi) {
            float pt = GdiManager::font_height_to_pt(fi->height);
            if (std::getenv("VITREO_WIN_DIAG"))
                fprintf(stderr, "[font] WM_SETFONT hwnd=0x%x font=0x%x -> pt=%.1f weight=%u\n",
                        hwnd, font_handle, pt, fi->weight);
            m_bridge.set_control_font_face(hwnd, fi->face_name, pt, fi->weight, fi->italic);
        } else {
            // font_handle == 0 (NULL) or unknown handle. In Win32 a NULL HFONT
            // means "use the default GUI font" — the dense ~8-9pt MS Shell Dlg
            // that the app sized its control rects for. macOS controls default to
            // 13pt, which overflows those rects (truncated radio/checkbox labels).
            // Apply a matching compact font so labels fit the Win32 layout.
            if (std::getenv("VITREO_WIN_DIAG"))
                fprintf(stderr, "[font] WM_SETFONT hwnd=0x%x font=0x%x NULL/unknown -> default 11pt\n",
                        hwnd, font_handle);
            m_bridge.set_control_font(hwnd, 11.0f, 400, false);
        }
        result = 0;
        return true;
    }
    case WM_GETFONT: {
        const auto* win = window_data(hwnd);
        result = win ? win->hfont : 0;
        return true;
    }
    case EM_SETLIMITTEXT: {
        // Learn: limits user-typed TCHARs only; wParam 0 → 0x7FFFFFFE
        // single-line, unlimited (0xFFFFFFFF) multiline. Does not truncate
        // existing text or WM_SETTEXT. Return value is unused.
        auto* win = m_hwnd_mgr.find(hwnd);
        if (!win) {
            return false;
        }
        constexpr u32 kEsMultiline = 0x0004;
        u32 limit = static_cast<u32>(wParam);
        if (limit == 0) {
            limit = (win->style & kEsMultiline) ? 0xFFFFFFFFu : 0x7FFFFFFEu;
        } else if ((win->style & kEsMultiline) == 0 && limit > 0x7FFFFFFEu) {
            limit = 0x7FFFFFFEu;
        }
        win->edit_text_limit = limit;
        result = 0;
        return true;
    }
    case EM_GETLIMITTEXT: {
        const auto* win = m_hwnd_mgr.find(hwnd);
        result = win ? win->edit_text_limit : 32767u;
        return true;
    }
    default:
        return false;  // Not a control message we handle
    }
    return false;
}

// PBM_GETRANGE with a PBRANGE* {int iLow; int iHigh;} (CommCtrl.h). Returns
// the low limit for wParam TRUE, the high limit for FALSE.
bool MessageSendManager::dispatch_progress_range(Marshaler& m, windowing::HWND hwnd,
                                                 u32 msg, u64 wParam, u64 lParam,
                                                 bool force_native) {
    if (msg != PBM_GETRANGE || lParam == 0) return false;
    const auto* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr || (!force_native && win->wndproc != 0)) return false;
    const std::wstring_view cls = win->control_class_name();
    constexpr std::wstring_view kProgress = L"msctls_progress32";
    if (cls.size() != kProgress.size()) return false;
    for (usize i = 0; i < cls.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(cls[i])) != static_cast<wint_t>(kProgress[i])) return false;
    }
    ProgressState snapshot;
    {
        std::lock_guard lock(m_control_state_mutex);
        snapshot = m_progress_state[hwnd];
    }
    (void)m.write_u32(static_cast<GuestAddr>(lParam), static_cast<u32>(snapshot.minimum));
    (void)m.write_u32(static_cast<GuestAddr>(lParam) + 4, static_cast<u32>(snapshot.maximum));
    m.set_return(static_cast<u32>(wParam != 0 ? snapshot.minimum : snapshot.maximum));
    return true;
}

} // namespace vitreo::winapi
