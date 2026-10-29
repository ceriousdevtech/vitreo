#include "winapi/user32/painting/sys_colors.hpp"
#include "painting.hpp"
#include "windowing/messages/message_queue.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/user32/classes/window_class.hpp"

#include "ipc/hwnd_registry.hpp"
#include <algorithm>
#include "windowing/model/hwnd_manager.hpp"

#include <cstdio>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// PaintingManager
// =============================================================================

PaintingManager::PaintingManager(::vitreo::windowing::HwndManager* hwnd_mgr)
    : m_hwnd_mgr{hwnd_mgr} {
}

PaintingManager::WindowPaintState& PaintingManager::get_or_create(u64 hwnd) {
    return m_windows[hwnd];
}

void PaintingManager::sync_paint_state_size(WindowPaintState& state,
                                            u64 hwnd) {
    if (!m_hwnd_mgr) return;
    if (const auto* win =
            m_hwnd_mgr->find(static_cast<::vitreo::windowing::HWND>(hwnd))) {
        i32 w = win->client_rect.right - win->client_rect.left;
        i32 h = win->client_rect.bottom - win->client_rect.top;
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;
        state.width = w;
        state.height = h;
    }
}

// =============================================================================
// BeginPaint / EndPaint
// =============================================================================

std::optional<u32> PaintingManager::brush_color(u64 brush) const {
    if (brush == 0) return std::nullopt;
    // COLOR_xxx + 1 first, as on Windows: no GDI handle is that small.
    if (const auto sys = sys_colors::from_brush(brush)) return sys;
    u32 color = 0;
    if (m_gdi_mgr != nullptr &&
        m_gdi_mgr->lookup_brush_color(static_cast<Handle>(brush), &color)) {
        return color;
    }
    return std::nullopt;
}

u64 PaintingManager::begin_paint(u64 hwnd, PaintStruct& ps) {
    WindowPaintState dimensions{};
    dimensions.width = dimensions.height = 0;
    sync_paint_state_size(dimensions, hwnd);
    WindowPaintState state;
    EraseBackgroundCallback erase_callback;
    u64 fallback_dc = 0;
    {
        std::lock_guard lock(m_state_mutex);
        auto& current = get_or_create(hwnd);
        if (dimensions.width > 0) {
            current.width = dimensions.width;
            current.height = dimensions.height;
        }
        state = current;
        erase_callback = m_erase_background_callback;
        if (!m_gdi_mgr) fallback_dc = m_next_dc++;
    }

    u64 hdc;
    if (m_gdi_mgr) {
        // Allocate DC through GdiManager so it shares the handle space with
        // CreateCompatibleDC / BitBlt targets.
        hdc = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
        m_gdi_mgr->ensure_framebuffer_size(hwnd, state.width, state.height);
        VITREO_TRACE( "[paint] BeginPaint hwnd=0x%llx → hdc=0x%llx size=%dx%d\n",
                     hwnd, hdc, state.width, state.height);
    } else {
        hdc = fallback_dc;
    }
    ps.hdc = hdc;
    ps.fErase = state.needs_erase ? 1 : 0;
    // rcPaint is the update region's box and the DC is clipped to the region
    // itself; with nothing pending both are empty ("BeginPaint direct ...
    // rc=10,10,30,20", "BeginPaint clipbox=2 10,10,30,20", "BeginPaint clean
    // rc=0,0,0,0").
    ClipRect region = state.update;
    if (!state.needs_paint || region.is_empty() || !region.valid) {
        region = ClipRect{0, 0, 0, 0, true, {}};
        ps.rcPaint = {0, 0, 0, 0};
    } else {
        ps.rcPaint = {region.left, region.top, region.right, region.bottom};
    }
    if (m_gdi_mgr) m_gdi_mgr->set_dc_clip(static_cast<Handle>(hdc), region);
    ps.fRestore = 0;
    ps.fIncUpdate = 0;

    // BeginPaint synchronously sends WM_ERASEBKGND when the update region is
    // marked for erasing. The window procedure may handle it without touching
    // the class brush (Qt does this for custom-painted frames). Only
    // DefWindowProc owns the default class-brush fill.
    if (ps.fErase && erase_callback) {
        if (erase_callback(hwnd, hdc) != 0) {
            ps.fErase = 0;
        }
    } else if (ps.fErase && m_gdi_mgr && m_wclass_mgr && m_hwnd_mgr) {
        // Standalone manager fallback used when no synchronous USER32 message
        // dispatcher is wired (principally focused unit tests).
        if (const auto* win = m_hwnd_mgr->find(static_cast<::vitreo::windowing::HWND>(hwnd))) {
            if (const auto* cls = m_wclass_mgr->find_by_atom(win->class_atom)) {
                u32 hbrush = cls->hbr_background;
                if (hbrush != 0) {
                    u32 color = 0xFFFFFF;
                    if (const auto sys = sys_colors::from_brush(hbrush)) {
                        // System color brush: hbrush = COLOR_xxx + 1
                        color = *sys;
                    } else if (m_gdi_mgr->lookup_brush_color(static_cast<Handle>(hbrush), &color)) {
                        // A real GDI brush handle
                    } else {
                        color = 0xFFFFFF;
                    }
                    m_gdi_mgr->fill_rect_gdi(static_cast<Handle>(hdc),
                                              ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right,
                                              ps.rcPaint.bottom, color);
                    VITREO_TRACE("[paint] BeginPaint erase-bkgnd hwnd=0x%llx brush=0x%x color=0x%06x\n",
                                hwnd, hbrush, color);
                    ps.fErase = 0; // Background was erased
                }
            }
        }
    }

    {
        std::lock_guard lock(m_state_mutex);
        auto& current = get_or_create(hwnd);
        current.needs_paint = false;
        current.needs_erase = false;
        current.update = ClipRect{};
    }

    return hdc;
}

bool PaintingManager::erase_now(u64 hwnd) {
    EraseBackgroundCallback erase_callback;
    {
        std::lock_guard lock(m_state_mutex);
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || !it->second.needs_erase ||
            !m_erase_background_callback) return false;
        erase_callback = m_erase_background_callback;
    }
    Handle hdc = 0;
    if (m_gdi_mgr) hdc = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
    const u64 erased = erase_callback(hwnd, hdc);
    if (m_gdi_mgr && hdc != 0)
        (void)m_gdi_mgr->release_dc(static_cast<Handle>(hwnd), hdc);
    {
        std::lock_guard lock(m_state_mutex);
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end() && erased != 0) it->second.needs_erase = false;
    }
    return true;
}

bool PaintingManager::end_paint(u64 hwnd, const PaintStruct& ps) {
    if (m_gdi_mgr) {
        // Flush the window framebuffer to Cocoa for display
        m_gdi_mgr->flush_window(hwnd);
        // Release the DC that was allocated in begin_paint
        m_gdi_mgr->release_dc(static_cast<Handle>(hwnd), static_cast<Handle>(ps.hdc));
    }
    return true;
}

// =============================================================================
// InvalidateRect / ValidateRect
// =============================================================================

void PaintingManager::update_region_combine(WindowPaintState& state, const ClipRect& part, i32 mode) {
    ClipRect client{0, 0, state.width, state.height, true, {}};
    ClipRect piece = ClipRect::combine(part, client, 1);  // RGN_AND
    piece.valid = true;
    ClipRect current = state.update;
    if (!current.valid) current = ClipRect{0, 0, 0, 0, true, {}};
    state.update = ClipRect::combine(current, piece, mode);
    state.update.valid = true;
    state.needs_paint = !state.update.parts().empty();
}

bool PaintingManager::is_window(u64 hwnd) const {
    if (hwnd == 0) return false;
    if (m_hwnd_mgr && m_hwnd_mgr->find(static_cast<::vitreo::windowing::HWND>(hwnd)) != nullptr) return true;
    return ipc::hwnd_registry_is_window(static_cast<u32>(hwnd));
}

bool PaintingManager::paints_visible(u64 hwnd) const {
    if (!m_hwnd_mgr) return true;
    auto current = static_cast<::vitreo::windowing::HWND>(hwnd);
    for (int depth = 0; depth < 64; ++depth) {
        const auto* w = m_hwnd_mgr->find(current);
        if (w == nullptr) return true;  // a peer window or the desktop
        if (!w->visible) return false;
        if (w->parent == ::vitreo::windowing::NULL_HWND || w->parent == ::vitreo::windowing::HWND_DESKTOP) return true;
        current = w->parent;
    }
    return true;
}

bool PaintingManager::invalidate_rect(u64 hwnd, const Rect* rect, bool erase) {
    // Win32: InvalidateRect(NULL, ...) invalidates all windows, not hwnd 0.
    if (hwnd == 0) {
        if (m_hwnd_mgr) {
            const auto top = m_hwnd_mgr->enum_children(::vitreo::windowing::HWND_DESKTOP);
            for (const auto child : top) (void)invalidate_rect(static_cast<u64>(child), rect, erase);
        }
        VITREO_TRACE("[paint] InvalidateRect hwnd=NULL -> all top-level windows erase=%d\n",
                     erase ? 1 : 0);
        return true;
    }

    // A hidden window keeps no update region.
    if (!paints_visible(hwnd)) return true;
    WindowPaintState dimensions{};
    dimensions.width = dimensions.height = 0;
    sync_paint_state_size(dimensions, hwnd);
    std::unique_lock lock(m_state_mutex);
    auto& state = get_or_create(hwnd);
    // Sync client size before a NULL-rect InvalidateRect so the update
    // region is the window's client, not the 800×600 default.
    if (dimensions.width > 0) {
        state.width = dimensions.width;
        state.height = dimensions.height;
    }
    // The update region accumulates ("union rc=10,5,60,20", GetUpdateRgn
    // COMPLEXREGION for two rectangles), clipped to the client area.
    const ClipRect part = rect ? ClipRect{rect->left, rect->top, rect->right, rect->bottom, true, {}}
                               : ClipRect{0, 0, state.width, state.height, true, {}};
    update_region_combine(state, part, 2);  // RGN_OR
    if (erase && state.needs_paint) state.needs_erase = true;
    VITREO_TRACE("[paint] InvalidateRect hwnd=0x%llx rect=%s erase=%d\n", hwnd, rect ? "rect" : "NULL",
                 erase ? 1 : 0);
    auto callback = m_invalidate_callback;
    lock.unlock();
    if (callback) callback(hwnd);
    return true;
}

bool PaintingManager::validate_rect(u64 hwnd, const Rect* rect) {
    // Win32: ValidateRect(NULL, ...) validates all windows.
    if (hwnd == 0) {
        std::lock_guard lock(m_state_mutex);
        for (auto& [h, state] : m_windows) {
            if (h == 0) continue;
            state.needs_paint = false;
            state.needs_erase = false;
            state.update = ClipRect{};
        }
        return true;
    }
    WindowPaintState dimensions{};
    dimensions.width = dimensions.height = 0;
    sync_paint_state_size(dimensions, hwnd);
    std::unique_lock lock(m_state_mutex);
    auto& state = get_or_create(hwnd);
    if (rect == nullptr) {
        state.needs_paint = false;
        state.needs_erase = false;
        state.update = ClipRect{};
        return true;
    }
    // Only the given part leaves the update region.
    if (dimensions.width > 0) {
        state.width = dimensions.width;
        state.height = dimensions.height;
    }
    update_region_combine(state, ClipRect{rect->left, rect->top, rect->right, rect->bottom, true, {}}, 4);
    if (!state.needs_paint) state.needs_erase = false;
    return true;
}

bool PaintingManager::invalidate_rgn(u64 hwnd, u64 hrgn, bool erase) {
    // A NULL region invalidates the whole client area, exactly like a NULL
    // rect does for InvalidateRect.
    if (hrgn == 0) return invalidate_rect(hwnd, nullptr, erase);
    ClipRect region{};
    if (m_gdi_mgr == nullptr || !m_gdi_mgr->get_region_rect(static_cast<Handle>(hrgn), region)) {
        // Not a region of this process: Windows ignores the handle and the
        // documented return is nonzero; repaint everything rather than drop it.
        return invalidate_rect(hwnd, nullptr, erase);
    }
    if (hwnd == 0) return invalidate_rect(hwnd, nullptr, erase);
    if (!paints_visible(hwnd)) return true;
    WindowPaintState dimensions{};
    dimensions.width = dimensions.height = 0;
    sync_paint_state_size(dimensions, hwnd);
    std::unique_lock lock(m_state_mutex);
    auto& state = get_or_create(hwnd);
    if (dimensions.width > 0) {
        state.width = dimensions.width;
        state.height = dimensions.height;
    }
    update_region_combine(state, region, 2);
    if (erase && state.needs_paint) state.needs_erase = true;
    // Wake a GetMessage/WaitMessage blocked on this window's thread: a new
    // update region is QS_PAINT, same as InvalidateRect.
    auto callback = m_invalidate_callback;
    lock.unlock();
    if (callback) callback(hwnd);
    return true;
}

bool PaintingManager::validate_rgn(u64 hwnd, u64 hrgn) {
    if (hrgn == 0) return validate_rect(hwnd, nullptr);
    ClipRect region{};
    if (m_gdi_mgr == nullptr || !m_gdi_mgr->get_region_rect(static_cast<Handle>(hrgn), region) || hwnd == 0) {
        return validate_rect(hwnd, nullptr);
    }
    WindowPaintState dimensions{};
    dimensions.width = dimensions.height = 0;
    sync_paint_state_size(dimensions, hwnd);
    std::unique_lock lock(m_state_mutex);
    auto& state = get_or_create(hwnd);
    if (dimensions.width > 0) {
        state.width = dimensions.width;
        state.height = dimensions.height;
    }
    update_region_combine(state, region, 4);
    if (!state.needs_paint) state.needs_erase = false;
    return true;
}

i32 PaintingManager::get_update_region(u64 hwnd, ClipRect& out, bool erase) {
    out = ClipRect{0, 0, 0, 0, true, {}};
    std::lock_guard lock(m_state_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.needs_paint) return 1;  // NULLREGION
    out = it->second.update;
    out.valid = true;
    if (erase) it->second.needs_erase = true;
    return out.rectangles.empty() ? 2 : 3;
}

u64 PaintingManager::find_window_needing_paint(u32 owner_thread_id,
                                               u64 hwnd_filter) const {
    std::vector<u64> candidates;
    {
        std::lock_guard lock(m_state_mutex);
        for (const auto& [hwnd, state] : m_windows)
            if (state.needs_paint) candidates.push_back(hwnd);
    }
    for (const auto hwnd : candidates) {
        if (hwnd == 0) continue;
        // A Get/PeekMessage hWnd filter also selects the window's children
        // (s2msg_queue_probe parent_filter_child_msg).
        if (hwnd_filter != 0 && hwnd != hwnd_filter &&
            (m_hwnd_mgr == nullptr ||
             !::vitreo::windowing::MessageQueue::hwnd_filter_selects(
                 *m_hwnd_mgr, static_cast<::vitreo::windowing::HWND>(hwnd),
                 static_cast<::vitreo::windowing::HWND>(hwnd_filter)))) {
            continue;
        }
        if (owner_thread_id != 0 && m_hwnd_mgr) {
            const auto* window = m_hwnd_mgr->find(
                static_cast<::vitreo::windowing::HWND>(hwnd));
            if (!window || window->owner_thread_id != owner_thread_id) {
                continue;
            }
        }
        // A window hidden after it was invalidated does not report QS_PAINT.
        if (paints_visible(hwnd)) return hwnd;
    }
    return 0;
}

bool PaintingManager::needs_paint(u64 hwnd) const {
    const bool visible = paints_visible(hwnd);
    std::lock_guard lock(m_state_mutex);
    auto it = m_windows.find(hwnd);
    return it != m_windows.end() && it->second.needs_paint && visible;
}

bool PaintingManager::get_update_rect(u64 hwnd, Rect& rect, bool erase) {
    if (hwnd == 0) return false;
    std::lock_guard lock(m_state_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end() || !it->second.needs_paint) {
        rect = {};
        return false;
    }
    auto& state = it->second;
    // The update region's box ("GetUpdateRect=1 rc=10,10,30,20").
    rect = {state.update.left, state.update.top, state.update.right, state.update.bottom};
    if (erase) state.needs_erase = true;
    return true;
}

// =============================================================================
// GetClientRect / GetWindowRect
// =============================================================================

bool PaintingManager::get_client_rect(u64 hwnd, Rect& rect) const {
    if (m_hwnd_mgr) {
        if (const ::vitreo::windowing::WindowData* win = m_hwnd_mgr->find(static_cast<::vitreo::windowing::HWND>(hwnd))) {
            // Learn: GetClientRect reports the client width/height; a window
            // created at 0×0 stays {0,0,0,0}. Do not coerce to 1×1 — that
            // lied about the Win32 size while Cocoa uses a minimal native
            // backing, and layout code then MoveWindow's children with
            // nonsense extents.
            const i32 w = win->client_rect.right - win->client_rect.left;
            const i32 h = win->client_rect.bottom - win->client_rect.top;
            rect = {0, 0, w, h};
            return true;
        }
    }

    {
        std::lock_guard lock(m_state_mutex);
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end() && it->second.width > 0 && it->second.height > 0) {
            rect = {0, 0, it->second.width, it->second.height};
            return true;
        }
    }

    // HWNDs are desktop-global, so GetClientRect succeeds for a published
    // HWND owned by another process. A 0×0 client (CreateWindow before the
    // first SetWindowPos) is still a valid window — Learn fails only for an
    // invalid handle, not an empty client. FALSE here skips the caller's
    // StretchDIBits.
    if (auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
        i32 w = remote->client_w;
        i32 h = remote->client_h;
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;
        rect = {0, 0, w, h};
        return true;
    }

    // Learn: unknown/invalid HWND → FALSE (not a fabricated default size).
    return false;
}

bool PaintingManager::get_window_rect(u64 hwnd, Rect& rect) const {
    if (m_hwnd_mgr) {
        ::vitreo::windowing::HWND current = static_cast<::vitreo::windowing::HWND>(hwnd);
        const ::vitreo::windowing::WindowData* win = m_hwnd_mgr->find(current);
        if (win) {
            i32 left = win->rect.left;
            i32 top = win->rect.top;
            i32 right = win->rect.right;
            i32 bottom = win->rect.bottom;

            // Convert to screen coordinates by walking the parent chain. A
            // child's rect is relative to its parent's CLIENT area, so every
            // ancestor contributes its window origin plus its client offset
            // (measured, s1win_api_probe c1_in_a_client / g_in_c1_client).
            ::vitreo::windowing::HWND parent = win->parent;
            while (parent != ::vitreo::windowing::HWND_DESKTOP &&
                   parent != ::vitreo::windowing::NULL_HWND &&
                   parent != ::vitreo::windowing::HWND_MESSAGE) {
                const ::vitreo::windowing::WindowData* p = m_hwnd_mgr->find(parent);
                if (!p) break;
                const i32 dx = p->rect.left + p->client_offset.x;
                const i32 dy = p->rect.top + p->client_offset.y;
                left += dx;
                top += dy;
                right += dx;
                bottom += dy;
                parent = p->parent;
            }

            rect = {left, top, right, bottom};
            return true;
        }
    }

    // Windows registered only with this manager (no HwndManager entry) keep
    // their recorded size.
    {
        std::lock_guard lock(m_state_mutex);
        if (auto it = m_windows.find(hwnd); it != m_windows.end()) {
            rect = {0, 0, it->second.width, it->second.height};
            return true;
        }
    }
    // Another capsule process's window: only its client size is published,
    // so its screen position is unknown here. Report the size at the origin
    // rather than failing a valid HWND.
    if (auto remote = ipc::hwnd_registry_lookup(static_cast<u32>(hwnd))) {
        rect = {0, 0, std::max(remote->client_w, 0), std::max(remote->client_h, 0)};
        return true;
    }
    // Learn: an invalid HWND fails; the caller maps this to
    // ERROR_INVALID_WINDOW_HANDLE and leaves the output untouched.
    return false;
}

// =============================================================================
// RECT Utility Functions
// =============================================================================

bool PaintingManager::set_rect(Rect& r, i32 left, i32 top, i32 right, i32 bottom) {
    r.left = left;
    r.top = top;
    r.right = right;
    r.bottom = bottom;
    return true;
}

bool PaintingManager::inflate_rect(Rect& r, i32 dx, i32 dy) {
    r.left -= dx;
    r.top -= dy;
    r.right += dx;
    r.bottom += dy;
    return true;
}

bool PaintingManager::offset_rect(Rect& r, i32 dx, i32 dy) {
    r.left += dx;
    r.top += dy;
    r.right += dx;
    r.bottom += dy;
    return true;
}

bool PaintingManager::pt_in_rect(const Rect& r, const Point& pt) {
    return pt.x >= r.left && pt.x < r.right &&
           pt.y >= r.top  && pt.y < r.bottom;
}

bool PaintingManager::is_rect_empty(const Rect& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

bool PaintingManager::intersect_rect(Rect& dst, const Rect& src1, const Rect& src2) {
    dst.left   = std::max(src1.left,   src2.left);
    dst.top    = std::max(src1.top,    src2.top);
    dst.right  = std::min(src1.right,  src2.right);
    dst.bottom = std::min(src1.bottom, src2.bottom);

    if (dst.right <= dst.left || dst.bottom <= dst.top) {
        dst = {};
        return false;
    }
    return true;
}

// =============================================================================
// FillRect / FrameRect / DrawText
// =============================================================================

void PaintingManager::set_gdi_manager(GdiManager* mgr) {
    m_gdi_mgr = mgr;
    if (mgr == nullptr) return;
    mgr->set_system_brush_resolver([](u64 brush) -> std::optional<u32> {
        // Only the GetSysColorBrush form; COLOR_xxx + 1 is not a brush object.
        if ((brush & ~u64{0xFF}) != sys_colors::kSysBrushBase) return std::nullopt;
        return sys_colors::from_brush(brush);
    });
}

i32 PaintingManager::fill_rect(u64 hdc, const Rect& rc, u64 hbrush) {
    // A bad DC fails with the error untouched; a NULL brush succeeds without
    // drawing (s3gdi_userpaint_probe "FillRect(NULL dc)=0", "(NULL brush)=1").
    if (!m_gdi_mgr || !m_gdi_mgr->has_dc(static_cast<Handle>(hdc))) return 0;
    if (hbrush == 0) return 1;

    // Learn (FillRect): hbr is a brush handle or a system colour value,
    // COLOR_xxx + 1 (e.g. COLOR_BTNFACE + 1 = 16). The colour values come
    // first, as on Windows; no GDI handle is that small (kFirstGdiHandle).
    u32 color = 0;
    if (const auto sys = sys_colors::from_brush(hbrush)) {
        // COLOR_xxx + 1, or a GetSysColorBrush handle
        color = *sys;
    } else if (m_gdi_mgr->fill_rect_with_brush(
                   static_cast<Handle>(hdc), rc.left, rc.top, rc.right, rc.bottom,
                   static_cast<Handle>(hbrush))) {
        return 1;
    } else {
        // Unknown brush — default to white
        color = 0xFFFFFF;
    }

    m_gdi_mgr->fill_rect_gdi(static_cast<Handle>(hdc),
                              rc.left, rc.top, rc.right, rc.bottom, color);
    return 1;
}

i32 PaintingManager::frame_rect(u64 hdc, const Rect& rc, u64 hbrush) {
    if (!m_gdi_mgr || !m_gdi_mgr->has_dc(static_cast<Handle>(hdc))) return 0;  // "FrameRect(NULL dc)=0"
    // Draw a 1-pixel border using four thin fill_rect calls
    Rect top_r    = {rc.left, rc.top, rc.right, rc.top + 1};
    Rect bottom_r = {rc.left, rc.bottom - 1, rc.right, rc.bottom};
    Rect left_r   = {rc.left, rc.top + 1, rc.left + 1, rc.bottom - 1};
    Rect right_r  = {rc.right - 1, rc.top + 1, rc.right, rc.bottom - 1};

    fill_rect(hdc, top_r, hbrush);
    fill_rect(hdc, bottom_r, hbrush);
    fill_rect(hdc, left_r, hbrush);
    fill_rect(hdc, right_r, hbrush);
    return 1;
}

i32 PaintingManager::draw_text(u64 hdc, const std::string& text,
                               Rect& rc, u32 format) {
    if (!m_gdi_mgr) return 0;
    return m_gdi_mgr->draw_text(static_cast<Handle>(hdc), text, rc.left, rc.top, rc.right, rc.bottom,
                                format);
}

static Rect read_guest_rect(Marshaler& m, GuestAddr ptr);
static void write_guest_rect(Marshaler& m, GuestAddr ptr, const Rect& r);

void PaintingManager::marshal_draw_text(GdiManager* gdi, Marshaler& m, bool wide, bool ex) {
    constexpr u32 DT_CALCRECT = 0x400, DT_TABSTOP = 0x80, DT_MODIFYSTRING = 0x10000;
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr text_ptr = m.arg_ptr(1);
    const i32 count = static_cast<i32>(m.arg32(2));
    const GuestAddr rect_ptr = m.arg_ptr(3);
    const u32 format = m.arg32(4);
    const GuestAddr params_ptr = ex ? m.arg_ptr(5) : 0;
    // A NULL DC or string fails with the last error untouched ("NULL dc
    // ret=0 err=57005", "NULL text ret=0 err=57005").
    if (gdi == nullptr || !gdi->has_dc(hdc) || text_ptr == 0 || rect_ptr == 0) {
        m.set_return(0u);
        return;
    }
    GdiManager::DrawTextParams params;
    GdiManager::DrawTextParams* params_arg = nullptr;
    if (params_ptr != 0) {
        // DRAWTEXTPARAMS { cbSize, iTabLength, iLeftMargin, iRightMargin,
        // uiLengthDrawn }; any other size is ERROR_INVALID_PARAMETER
        // ("ex bad cbSize ret=0 err=87").
        u32 raw[5] = {};
        if (m.memory().read_bytes(params_ptr, reinterpret_cast<u8*>(raw), 20).is_err() || raw[0] != 20) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        params.tab_length = static_cast<i32>(raw[1]);
        params.left_margin = static_cast<i32>(raw[2]);
        params.right_margin = static_cast<i32>(raw[3]);
        params_arg = &params;
    }
    // The characters: count -1 means NUL-terminated.
    std::u16string text;
    if (wide) {
        for (u32 i = 0; count < 0 || i < static_cast<u32>(count); ++i) {
            const auto u = m.read_u16(text_ptr + i * 2);
            if (u.is_err() || (count < 0 && u.value() == 0)) break;
            text.push_back(static_cast<char16_t>(u.value()));
        }
    } else {
        std::vector<u8> bytes;
        for (u32 i = 0; count < 0 || i < static_cast<u32>(count); ++i) {
            const auto b = m.read_u8(text_ptr + i);
            if (b.is_err() || (count < 0 && b.value() == 0)) break;
            bytes.push_back(b.value());
        }
        std::vector<u16> units(bytes.size() + 1);
        const auto r = codepage::multi_byte_to_wide(codepage::kCpAcp, 0, bytes.data(), bytes.size(),
                                                    units.data(), units.size());
        if (r.ok()) text.assign(units.begin(), units.begin() + r.count);
    }
    Rect rc = read_guest_rect(m, rect_ptr);
    // An empty string draws nothing and returns 1 with the error untouched;
    // DT_CALCRECT collapses the rectangle onto its top-left ("empty ret=1",
    // "calcrect empty ... rc=10,5,10,5").
    if (text.empty()) {
        if ((format & DT_CALCRECT) && !(format & DT_TABSTOP)) {
            rc.right = rc.left;
            rc.bottom = rc.top;
            write_guest_rect(m, rect_ptr, rc);
        }
        m.set_return(1u);
        return;
    }
    if (format & DT_MODIFYSTRING) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!DrawText: DT_MODIFYSTRING (string left unmodified)\n");
        }
    }
    const i32 result = gdi->draw_text_ex(hdc, text, rc.left, rc.top, rc.right, rc.bottom, format, params_arg);
    write_guest_rect(m, rect_ptr, rc);
    if (params_arg != nullptr) (void)m.write_u32(params_ptr + 16, params.length_drawn);
    // A drawn string clears the last error ("left|top ret=16 err=0").
    if (result != 0) set_last_error(0);
    m.set_return(static_cast<u32>(result));
}

// =============================================================================
// Helper: Read/Write RECT (16 bytes: left, top, right, bottom)
// =============================================================================

static Rect read_guest_rect(Marshaler& m, GuestAddr ptr) {
    Rect r;
    auto l = m.read_u32(ptr + 0);
    auto t = m.read_u32(ptr + 4);
    auto ri = m.read_u32(ptr + 8);
    auto b = m.read_u32(ptr + 12);
    r.left   = l.is_ok() ? static_cast<i32>(l.value()) : 0;
    r.top    = t.is_ok() ? static_cast<i32>(t.value()) : 0;
    r.right  = ri.is_ok() ? static_cast<i32>(ri.value()) : 0;
    r.bottom = b.is_ok() ? static_cast<i32>(b.value()) : 0;
    return r;
}

static void write_guest_rect(Marshaler& m, GuestAddr ptr, const Rect& r) {
    (void)m.write_u32(ptr + 0,  static_cast<u32>(r.left));
    (void)m.write_u32(ptr + 4,  static_cast<u32>(r.top));
    (void)m.write_u32(ptr + 8,  static_cast<u32>(r.right));
    (void)m.write_u32(ptr + 12, static_cast<u32>(r.bottom));
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports create_painting_thunks(PaintingManager& mgr) {
    DllExports exports;

    // BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint) → HDC
    exports.entries.push_back(ExportEntry{
        "BeginPaint", 1,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            auto hwnd = m.arg(0);
            // Not a window: NULL, error untouched ("BeginPaint(bad)=NULL").
            if (!mgr.is_window(hwnd)) { m.set_return(0u); return; }
            GuestAddr ps_ptr = m.arg_ptr(1);
            PaintStruct ps{};
            u64 hdc = mgr.begin_paint(hwnd, ps);
            if (ps_ptr != 0) {
                // PAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL
                // fRestore; BOOL fIncUpdate; BYTE rgbReserved[32]; }: 64
                // bytes on x86, 72 on x64 where the HDC is 8 bytes and
                // rcPaint starts at 12.
                const bool wide_ptr = m.cpu_is_64bit();
                const GuestAddr at = ps_ptr + (wide_ptr ? 8u : 4u);
                if (wide_ptr) (void)m.memory().write_u64(ps_ptr, ps.hdc);
                else (void)m.write_u32(ps_ptr, static_cast<u32>(ps.hdc));
                (void)m.write_u32(at + 0,  static_cast<u32>(ps.fErase));
                (void)m.write_u32(at + 4,  static_cast<u32>(ps.rcPaint.left));
                (void)m.write_u32(at + 8,  static_cast<u32>(ps.rcPaint.top));
                (void)m.write_u32(at + 12, static_cast<u32>(ps.rcPaint.right));
                (void)m.write_u32(at + 16, static_cast<u32>(ps.rcPaint.bottom));
                (void)m.write_u32(at + 20, static_cast<u32>(ps.fRestore));
                (void)m.write_u32(at + 24, static_cast<u32>(ps.fIncUpdate));
            }
            m.set_return(static_cast<u64>(hdc));
        },
        8
    });

    // EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint) → BOOL
    exports.entries.push_back(ExportEntry{
        "EndPaint", 2,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            auto hwnd = m.arg(0);
            GuestAddr ps_ptr = m.arg_ptr(1);
            PaintStruct ps{};
            if (ps_ptr != 0) {
                if (m.cpu_is_64bit()) {
                    auto h = m.memory().read_u64(ps_ptr);
                    if (h.is_ok()) ps.hdc = h.value();
                } else {
                    auto h = m.read_u32(ps_ptr + 0);
                    if (h.is_ok()) ps.hdc = h.value();
                }
            }
            mgr.end_paint(hwnd, ps);
            m.set_return(1); // TRUE
        },
        8
    });

    // InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase) → BOOL
    exports.entries.push_back(ExportEntry{
        "InvalidateRect", 3,
        [&mgr](Marshaler& m) {
            auto hwnd = m.arg(0);
            GuestAddr rect_ptr = m.arg_ptr(1);
            auto erase = static_cast<u32>(m.arg(2));
            // A handle that is not a window still answers TRUE and records
            // nothing ("InvalidateRect(bad hwnd)=1 err=57005").
            if (hwnd != 0 && !mgr.is_window(hwnd)) { m.set_return(1); return; }
            if (rect_ptr != 0) {
                Rect r = read_guest_rect(m, rect_ptr);
                mgr.invalidate_rect(hwnd, &r, erase != 0);
            } else {
                mgr.invalidate_rect(hwnd, nullptr, erase != 0);
            }
            m.set_return(1);
        },
        12
    });

    // InvalidateRgn(HWND hWnd, HRGN hRgn, BOOL bErase) → BOOL
    // The return value is always nonzero; the region stays owned by the caller.
    exports.entries.push_back(ExportEntry{
        "InvalidateRgn", 0,
        [&mgr](Marshaler& m) {
            auto hwnd = m.arg(0);
            auto hrgn = m.arg(1);
            auto erase = static_cast<u32>(m.arg(2));
            (void)mgr.invalidate_rgn(hwnd, hrgn, erase != 0);
            m.set_return(1);
        },
        12
    });

    // ValidateRgn(HWND hWnd, HRGN hRgn) → BOOL
    exports.entries.push_back(ExportEntry{
        "ValidateRgn", 0,
        [&mgr](Marshaler& m) {
            auto hwnd = m.arg(0);
            auto hrgn = m.arg(1);
            m.set_return(mgr.validate_rgn(hwnd, hrgn) ? 1 : 0);
        },
        8
    });

    // ValidateRect(HWND hWnd, const RECT* lpRect) → BOOL
    exports.entries.push_back(ExportEntry{
        "ValidateRect", 4,
        [&mgr](Marshaler& m) {
            auto hwnd = m.arg(0);
            // "ValidateRect(bad hwnd)=0 err=57005".
            if (hwnd != 0 && !mgr.is_window(hwnd)) { m.set_return(0u); return; }
            GuestAddr rect_ptr = m.arg_ptr(1);
            if (rect_ptr != 0) {
                Rect r = read_guest_rect(m, rect_ptr);
                mgr.validate_rect(hwnd, &r);
            } else {
                mgr.validate_rect(hwnd, nullptr);
            }
            m.set_return(1);
        },
        8
    });

    // GetClientRect(HWND hWnd, LPRECT lpRect) → BOOL
    exports.entries.push_back(ExportEntry{
        "GetClientRect", 5,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            auto hwnd = m.arg(0);
            GuestAddr rect_ptr = m.arg_ptr(1);
            Rect r{};
            if (!mgr.get_client_rect(hwnd, r)) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0);
                return;
            }
            VITREO_TRACE( "[paint] GetClientRect(0x%llx) → {%d,%d,%d,%d}\n",
                         hwnd, r.left, r.top, r.right, r.bottom);
            if (rect_ptr != 0) {
                write_guest_rect(m, rect_ptr, r);
            }
            m.set_return(1);
        },
        8
    });

    // GetWindowRect(HWND hWnd, LPRECT lpRect) → BOOL
    exports.entries.push_back(ExportEntry{
        "GetWindowRect", 6,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            auto hwnd = m.arg(0);
            GuestAddr rect_ptr = m.arg_ptr(1);
            Rect r{};
            if (!mgr.get_window_rect(hwnd, r)) {
                // Measured (s1win_api_probe getwindowrect_invalid): FALSE,
                // ERROR_INVALID_WINDOW_HANDLE, output untouched.
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0);
                return;
            }
            if (rect_ptr != 0) {
                write_guest_rect(m, rect_ptr, r);
            }
            m.set_return(1);
        },
        8
    });

    // SetRect(LPRECT lprc, int left, int top, int right, int bottom) → BOOL
    exports.entries.push_back(ExportEntry{
        "SetRect", 7,
        [](Marshaler& m) {
            GuestAddr rect_ptr = m.arg_ptr(0);
            if (rect_ptr == 0) { m.set_return(0); return; }
            auto left   = static_cast<i32>(m.arg(1));
            auto top    = static_cast<i32>(m.arg(2));
            auto right  = static_cast<i32>(m.arg(3));
            auto bottom = static_cast<i32>(m.arg(4));
            Rect r{};
            PaintingManager::set_rect(r, left, top, right, bottom);
            if (rect_ptr != 0) {
                write_guest_rect(m, rect_ptr, r);
            }
            m.set_return(1);
        },
        20
    });

    // InflateRect(LPRECT lprc, int dx, int dy) → BOOL
    exports.entries.push_back(ExportEntry{
        "InflateRect", 8,
        [](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            GuestAddr rect_ptr = m.arg_ptr(0);
            auto dx = static_cast<i32>(m.arg(1));
            auto dy = static_cast<i32>(m.arg(2));
            if (rect_ptr != 0) {
                Rect r = read_guest_rect(m, rect_ptr);
                PaintingManager::inflate_rect(r, dx, dy);
                write_guest_rect(m, rect_ptr, r);
            }
            m.set_return(1);
        },
        12
    });

    // OffsetRect(LPRECT lprc, int dx, int dy) → BOOL
    exports.entries.push_back(ExportEntry{
        "OffsetRect", 9,
        [](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            GuestAddr rect_ptr = m.arg_ptr(0);
            auto dx = static_cast<i32>(m.arg(1));
            auto dy = static_cast<i32>(m.arg(2));
            if (rect_ptr != 0) {
                Rect r = read_guest_rect(m, rect_ptr);
                PaintingManager::offset_rect(r, dx, dy);
                write_guest_rect(m, rect_ptr, r);
            }
            m.set_return(1);
        },
        12
    });

    // PtInRect(const RECT* lprc, POINT pt) → BOOL
    // POINT is passed by value: two DWORDs on the stack for PE32 stdcall,
    // one packed register (RDX: x low dword, y high dword) on Win64.
    exports.entries.push_back(ExportEntry{
        "PtInRect", 10,
        [](Marshaler& m) {
            GuestAddr rect_ptr = m.arg_ptr(0);
            i32 px = 0;
            i32 py = 0;
            if (m.cpu_is_64bit()) {
                const u64 packed = m.arg(1);
                px = static_cast<i32>(static_cast<u32>(packed));
                py = static_cast<i32>(static_cast<u32>(packed >> 32));
            } else {
                px = static_cast<i32>(static_cast<u32>(m.arg(1)));
                py = static_cast<i32>(static_cast<u32>(m.arg(2)));
            }
            if (rect_ptr != 0) {
                Rect r = read_guest_rect(m, rect_ptr);
                Point pt{px, py};
                m.set_return(PaintingManager::pt_in_rect(r, pt) ? 1u : 0u);
            } else {
                m.set_return(0);
            }
        },
        12
    });

    // IsRectEmpty(const RECT* lprc) → BOOL
    exports.entries.push_back(ExportEntry{
        "IsRectEmpty", 11,
        [](Marshaler& m) {
            GuestAddr rect_ptr = m.arg_ptr(0);
            if (rect_ptr != 0) {
                Rect r = read_guest_rect(m, rect_ptr);
                m.set_return(PaintingManager::is_rect_empty(r) ? 1u : 0u);
            } else {
                m.set_return(1); // NULL rect is empty
            }
        },
        4
    });

    // IntersectRect(LPRECT dst, const RECT* src1, const RECT* src2) → BOOL
    exports.entries.push_back(ExportEntry{
        "IntersectRect", 12,
        [](Marshaler& m) {
            GuestAddr dst_ptr  = m.arg_ptr(0);
            GuestAddr src1_ptr = m.arg_ptr(1);
            GuestAddr src2_ptr = m.arg_ptr(2);
            if (dst_ptr && src1_ptr && src2_ptr) {
                Rect src1 = read_guest_rect(m, src1_ptr);
                Rect src2 = read_guest_rect(m, src2_ptr);
                Rect dst{};
                bool ok = PaintingManager::intersect_rect(dst, src1, src2);
                write_guest_rect(m, dst_ptr, dst);
                m.set_return(ok ? 1u : 0u);
            } else {
                m.set_return(0);
            }
        },
        12
    });

    // FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr) → int
    exports.entries.push_back(ExportEntry{
        "FillRect", 13,
        [&mgr](Marshaler& m) {
            auto hdc = m.arg(0);
            GuestAddr rect_ptr = m.arg_ptr(1);
            auto hbrush = m.arg(2);
            Rect r{};
            if (rect_ptr != 0) r = read_guest_rect(m, rect_ptr);
            if (std::getenv("VITREO_WIN_DIAG")) {
                u32 c = 0xDEADBEEF; const bool known = mgr.gdi() && mgr.gdi()->lookup_brush_color(static_cast<Handle>(hbrush), &c);
                fprintf(stderr, "[gdi] FillRect hdc=0x%llx rect=(%d,%d,%d,%d) brush=0x%llx known=%d color=0x%06x\n",
                        (unsigned long long)hdc, r.left, r.top, r.right, r.bottom,
                        (unsigned long long)hbrush, known ? 1 : 0, c);
            }
            m.set_return(static_cast<u64>(mgr.fill_rect(hdc, r, hbrush)));
        },
        12
    });

    // FrameRect(HDC hDC, const RECT* lprc, HBRUSH hbr) → int
    exports.entries.push_back(ExportEntry{
        "FrameRect", 14,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            auto hdc = m.arg(0);
            GuestAddr rect_ptr = m.arg_ptr(1);
            auto hbrush = m.arg(2);
            Rect r{};
            if (rect_ptr != 0) r = read_guest_rect(m, rect_ptr);
            m.set_return(static_cast<u64>(mgr.frame_rect(hdc, r, hbrush)));
        },
        12
    });

    // DrawTextW/A(HDC hdc, LPCTSTR text, int count, LPRECT lprc, UINT format) → int
    exports.entries.push_back(ExportEntry{
        "DrawTextW", 15,
        [&mgr](Marshaler& m) { PaintingManager::marshal_draw_text(mgr.gdi(), m, true, false); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "DrawTextA", 16,
        [&mgr](Marshaler& m) { PaintingManager::marshal_draw_text(mgr.gdi(), m, false, false); },
        20
    });

    return exports;
}

} // namespace vitreo::winapi
