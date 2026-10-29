#pragma once

/// @file painting.hpp
/// @brief BeginPaint/EndPaint/InvalidateRect and RECT utilities (WIN32-085)

#include <vitreo/common.hpp>
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>

#include "windowing/model/hwnd_manager.hpp"

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// RECT structure
// =============================================================================

struct Rect {
    i32 left = 0;
    i32 top = 0;
    i32 right = 0;
    i32 bottom = 0;
};

// =============================================================================
// POINT structure
// =============================================================================

struct Point {
    i32 x = 0;
    i32 y = 0;
};

// =============================================================================
// PAINTSTRUCT (simplified)
// =============================================================================

struct PaintStruct {
    u64 hdc;
    i32 fErase;
    Rect rcPaint;
    i32 fRestore;
    i32 fIncUpdate;
};

// =============================================================================
// Error Codes
// =============================================================================

constexpr u32 ERROR_INVALID_WINDOW_HANDLE = 1400;

// =============================================================================
// PaintingManager
// =============================================================================

class GdiManager; // forward declaration for DC routing
class WindowClassManager; // forward declaration for background brush

class PaintingManager {
public:
    using EraseBackgroundCallback = std::function<u64(u64 hwnd, u64 hdc)>;

    explicit PaintingManager(::vitreo::windowing::HwndManager* hwnd_mgr = nullptr);

    /// Set the GdiManager for DC allocation (BeginPaint/EndPaint routing)
    /// Also teaches GDI the GetSysColorBrush handles, which Windows makes
    /// stock brushes: SelectObject, GetObject and PatBlt accept them.
    void set_gdi_manager(GdiManager* mgr);
    [[nodiscard]] GdiManager* gdi() const noexcept { return m_gdi_mgr; }

    /// The solid colour a class/FillRect brush paints: a real GDI brush first
    /// (small GDI handles overlap COLOR_xxx + 1), then the system-colour
    /// forms. nullopt for NULL or an unresolvable brush.
    [[nodiscard]] std::optional<u32> brush_color(u64 brush) const;

    /// Set the WindowClassManager for background brush lookup
    void set_wclass_manager(WindowClassManager* mgr) { m_wclass_mgr = mgr; }

    /// BeginPaint sends WM_ERASEBKGND synchronously when the update region is
    /// marked for erasing. The window procedure decides whether the background
    /// was erased; DefWindowProc performs the class-brush fill when selected.
    void set_erase_background_callback(EraseBackgroundCallback callback) {
        std::lock_guard lock(m_state_mutex);
        m_erase_background_callback = std::move(callback);
    }

    /// Erase a pending update region now: sends WM_ERASEBKGND with a window
    /// DC, as SetWindowPos does when it exposes client pixels. When the
    /// procedure erases (nonzero), BeginPaint later reports fErase FALSE; when
    /// it returns 0 the region stays marked for erasing (Learn:
    /// WM_ERASEBKGND). Returns whether WM_ERASEBKGND was sent.
    bool erase_now(u64 hwnd);

    /// BeginPaint - returns HDC (0 on failure)
    [[nodiscard]] u64 begin_paint(u64 hwnd, PaintStruct& ps);

    /// EndPaint
    bool end_paint(u64 hwnd, const PaintStruct& ps);

    /// InvalidateRect
    bool invalidate_rect(u64 hwnd, const Rect* rect, bool erase);

    /// Called with each window whose update region became non-empty, so the
    /// owning thread's message wait wakes for the WM_PAINT it now owes.
    void set_invalidate_callback(std::function<void(u64 hwnd)> callback) {
        std::lock_guard lock(m_state_mutex);
        m_invalidate_callback = std::move(callback);
    }

    /// ValidateRect
    bool validate_rect(u64 hwnd, const Rect* rect);

    /// InvalidateRgn — add a region, in client coordinates, to the window's
    /// update region. A NULL @p hrgn invalidates the entire client area.
    /// Vitreo tracks update regions as bounding rectangles, so the region's
    /// box is what accumulates.
    bool invalidate_rgn(u64 hwnd, u64 hrgn, bool erase);

    /// ValidateRgn — remove a region, in client coordinates, from the window's
    /// update region. A NULL @p hrgn validates the entire client area.
    bool validate_rgn(u64 hwnd, u64 hrgn);

    /// Return the HWND of a window that needs painting, or 0 if none.
    /// Used by the message loop to synthesize WM_PAINT (low-priority).
    /// @param owner_thread_id  Only consider windows owned by this thread.
    /// @param hwnd_filter       When non-zero, only consider this window.
    ///                          PeekMessage/GetMessage take an HWND filter and
    ///                          must not report a paint for a different window.
    [[nodiscard]] u64 find_window_needing_paint(u32 owner_thread_id = 0,
                                                u64 hwnd_filter = 0) const;

    /// Check if a specific window needs painting.
    [[nodiscard]] bool needs_paint(u64 hwnd) const;

    /// GetUpdateRgn: the update region (client coordinates) and its type
    /// (NULLREGION 1 / SIMPLEREGION 2 / COMPLEXREGION 3).
    i32 get_update_region(u64 hwnd, ClipRect& out, bool erase);

    /// True for a window of this process or a published peer window.
    [[nodiscard]] bool is_window(u64 hwnd) const;

    /// False for a window of this process that is hidden or has a hidden
    /// ancestor: such a window has no update region and never reports
    /// QS_PAINT (s3gdi_userpaint_probe "hidden: invalidate pending=0");
    /// ShowWindow invalidates it when it appears.
    [[nodiscard]] bool paints_visible(u64 hwnd) const;

    /// GetUpdateRect — write the pending update bounding rect when the window
    /// needs painting. Returns false when there is no update region.
    /// When @p erase is true and an update region exists, marks erase needed
    /// (BeginPaint performs the actual WM_ERASEBKGND-equivalent fill).
    [[nodiscard]] bool get_update_rect(u64 hwnd, Rect& rect, bool erase);

    /// GetClientRect
    bool get_client_rect(u64 hwnd, Rect& rect) const;

    /// GetWindowRect
    bool get_window_rect(u64 hwnd, Rect& rect) const;

    // -------------------------------------------------------------------------
    // RECT utility functions (static)
    // -------------------------------------------------------------------------

    static bool set_rect(Rect& r, i32 left, i32 top, i32 right, i32 bottom);
    static bool inflate_rect(Rect& r, i32 dx, i32 dy);
    static bool offset_rect(Rect& r, i32 dx, i32 dy);
    static bool pt_in_rect(const Rect& r, const Point& pt);
    static bool is_rect_empty(const Rect& r);
    static bool intersect_rect(Rect& dst, const Rect& src1, const Rect& src2);

    // -------------------------------------------------------------------------
    // FillRect / FrameRect / DrawText
    // -------------------------------------------------------------------------

    i32 fill_rect(u64 hdc, const Rect& rc, u64 hbrush);
    i32 frame_rect(u64 hdc, const Rect& rc, u64 hbrush);
    i32 draw_text(u64 hdc, const std::string& text, Rect& rc, u32 format);
    /// DrawTextW/A and DrawTextExW/A from the guest's arguments (@p ex: a
    /// DRAWTEXTPARAMS follows the format), with the measured Windows 11
    /// failure and last-error rules (s3gdi_drawtext_probe).
    static void marshal_draw_text(GdiManager* gdi, Marshaler& m, bool wide, bool ex);

private:
    // Protect the process-owned paint table, fallback DC cursor and callbacks.
    // Snapshot under this lock; release before guest or other manager calls.
    mutable std::mutex m_state_mutex;
    ::vitreo::windowing::HwndManager* m_hwnd_mgr = nullptr;
    GdiManager* m_gdi_mgr = nullptr;
    WindowClassManager* m_wclass_mgr = nullptr;
    EraseBackgroundCallback m_erase_background_callback;
    std::function<void(u64 hwnd)> m_invalidate_callback;
    u64 m_next_dc = 0x1000;

    struct WindowPaintState {
        bool needs_paint = false;  ///< the update region is not empty
        bool needs_erase = false;
        /// The update region in client coordinates (Windows keeps a region:
        /// two invalidated rectangles make a complex region, validating a
        /// part removes only that part - s3gdi_userpaint_probe).
        ClipRect update;
        i32 width = 800;
        i32 height = 600;
    };
    /// Combine @p part into the window's update region (RGN_OR / RGN_DIFF),
    /// clipped to the client area.
    void update_region_combine(WindowPaintState& state, const ClipRect& part, i32 mode);
    std::unordered_map<u64, WindowPaintState> m_windows;

    // Caller holds m_state_mutex; returned reference stays inside that scope.
    WindowPaintState& get_or_create(u64 hwnd);
    void sync_paint_state_size(WindowPaintState& state, u64 hwnd);
};

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports create_painting_thunks(PaintingManager& mgr);

} // namespace vitreo::winapi
