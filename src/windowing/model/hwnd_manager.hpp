#pragma once
/// @file hwnd_manager.hpp
/// @brief HWND allocation, tracking, and hierarchy management (WINDOW-001)

#include "window_types.hpp"
#include <atomic>
#include <functional>
#include <unordered_map>
#include <optional>
#include <shared_mutex>

namespace vitreo::windowing {

class HwndManager {
public:
    HwndManager();

    /// Use a desktop-wide HWND allocator. The callback must return a nonzero
    /// handle that is unique across all guest processes on the desktop.
    void set_handle_allocator(std::function<HWND()> allocator);

    /// Create a window from data. Assigns a new HWND and stores it.
    HWND create(const WindowData& data);

    /// Destroy a window and remove it from its parent's children list.
    void destroy(HWND hwnd);

    /// Change a window's parent and maintain both hierarchy child lists.
    /// A parent owned by another process need not exist in this manager.
    bool set_parent(HWND hwnd, HWND parent, HWND& previous_parent);

    /// Find window data by handle.
    WindowData* find(HWND hwnd);
    const WindowData* find(HWND hwnd) const;

    /// Return a stable copy of the window's owning guest thread ID.
    ///
    /// Message routing uses this instead of retaining a WindowData pointer
    /// while other guest threads may create or destroy windows.
    std::optional<u32> owner_thread_id(HWND hwnd) const;

    /// Return a copy of the window's client rectangle, read under the
    /// manager's lock (a WindowData pointer from find() may be freed by a
    /// concurrent DestroyWindow on another thread).
    std::optional<Rect> client_rect(HWND hwnd) const;

    /// Enumerate direct children of a parent.
    std::vector<HWND> enum_children(HWND parent) const;

    /// Message-only windows (hWndParent == HWND_MESSAGE), oldest first.
    std::vector<HWND> message_only_windows() const;
    /// True if thread @p tid owns any window (SetThreadDesktop's rule).
    [[nodiscard]] bool thread_has_windows(u32 tid) const;

    /// Return the topmost immediate child containing a point in parent-client
    /// coordinates, applying the CWP_SKIP* flags. If no eligible child
    /// contains the point, return the parent; return NULL when the parent is
    /// invalid or the point lies outside its client area.
    HWND child_window_from_point(HWND parent, Point point, u32 flags) const;

    /// Learn WindowFromPoint: the topmost visible, enabled window whose
    /// window rect contains the screen point. Hidden and disabled windows
    /// are skipped; a static-text control yields the window beneath it.
    /// Returns NULL when no window contains the point.
    HWND window_from_point(Point screen) const;

    /// Reorder a window among its siblings in z-order.
    /// insert_after follows Win32 SetWindowPos semantics (HWND_TOP/BOTTOM or sibling HWND).
    bool set_z_order(HWND hwnd, HWND insert_after);

    /// Find a window by class name and/or title.
    /// Empty class_name or title means "match any".
    HWND find_window(std::wstring_view class_name, std::wstring_view title) const;

    /// Get desktop HWND (always exists)
    HWND desktop() const noexcept { return HWND_DESKTOP; }

    /// Get count of windows (excluding desktop)
    usize count() const;

    /// Invoke @p fn for every tracked HWND (excluding the desktop).
    /// Used by GDI screen-DC BitBlt to locate windows covering a screen point.
    template <typename Fn>
    void for_each_hwnd(Fn&& fn) const {
        std::shared_lock lock(m_mutex);
        for (const auto& [hwnd, data] : m_windows) {
            (void)data;
            if (hwnd == HWND_DESKTOP) {
                continue;
            }
            fn(hwnd);
        }
    }

    /// Check if an HWND is valid
    bool is_valid(HWND hwnd) const;

    /// True while any window belongs to class registration @p class_id
    /// (UnregisterClass: ERROR_CLASS_HAS_WINDOWS).
    bool any_window_of_class(u32 class_id) const;

    /// Apply masked UxTheme WTNCA_* state to a window.
    bool set_theme_nonclient_attributes(HWND hwnd, u32 mask, u32 attributes);

    /// Return a stable copy of the current UxTheme WTNCA_* state.
    std::optional<u32> theme_nonclient_attributes(HWND hwnd) const;

    /// Mouse capture: set/get/release
    HWND set_capture(HWND hwnd);
    void release_capture();
    HWND get_capture() const noexcept {
        return m_capture_hwnd.load(std::memory_order_acquire);
    }

private:
    static constexpr u32 kWsExTopmost = 0x00000008u;
    [[nodiscard]] bool is_topmost_unlocked(HWND hwnd) const;
    [[nodiscard]] usize top_of_band_unlocked(const std::vector<HWND>& siblings,
                                             bool topmost) const;
    void set_topmost_unlocked(HWND hwnd, bool topmost);
    void raise_owned_above_unlocked(std::vector<HWND>& siblings, HWND owner);
    HWND allocate_handle();
    void remove_from_parent_unlocked(HWND hwnd);

    mutable std::shared_mutex m_mutex;
    std::unordered_map<HWND, WindowData> m_windows;
    HWND m_next_handle;
    std::function<HWND()> m_handle_allocator;
    std::atomic<HWND> m_capture_hwnd{NULL_HWND};  // Mouse capture window

    static constexpr HWND FIRST_HWND = 0x00010000;
};

} // namespace vitreo::windowing
