/// @file hwnd_manager.cpp
/// @brief HWND allocation, tracking, and hierarchy management (WINDOW-001)

#include "windowing/model/hwnd_manager.hpp"
#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace vitreo::windowing {

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

HwndManager::HwndManager()
    : m_next_handle(FIRST_HWND)
{
    // Create the desktop window as a permanent root entry.  Its default
    // dimensions match USER32's fallback screen metrics; the native bridge
    // replaces them with the actual host screen dimensions during startup.
    WindowData desktop_data{};
    desktop_data.handle  = HWND_DESKTOP;
    desktop_data.parent  = HWND_DESKTOP;
    desktop_data.style   = WS::OVERLAPPED | WS::VISIBLE;
    desktop_data.rect    = Rect{0, 0, 1920, 1080};
    desktop_data.client_rect = desktop_data.rect;
    desktop_data.visible = true;
    desktop_data.enabled = true;
    desktop_data.title   = L"Desktop";
    m_windows.emplace(HWND_DESKTOP, desktop_data);
}

// -----------------------------------------------------------------------------
// Handle Allocation
// -----------------------------------------------------------------------------

HWND HwndManager::allocate_handle() {
    if (m_handle_allocator) {
        const HWND allocated = m_handle_allocator();
        if (allocated != NULL_HWND && allocated != HWND_DESKTOP &&
            !m_windows.contains(allocated)) {
            if (allocated >= m_next_handle) m_next_handle = allocated + 1;
            return allocated;
        }
    }
    while (m_windows.contains(m_next_handle)) {
        ++m_next_handle;
    }
    return m_next_handle++;
}

void HwndManager::set_handle_allocator(std::function<HWND()> allocator) {
    std::unique_lock lock(m_mutex);
    m_handle_allocator = std::move(allocator);
}

// -----------------------------------------------------------------------------
// Create / Destroy
// -----------------------------------------------------------------------------

bool HwndManager::any_window_of_class(u32 class_id) const {
    if (class_id == 0) return false;
    std::shared_lock lock(m_mutex);
    for (const auto& [hwnd, data] : m_windows) {
        (void)hwnd;
        if (data.class_id == class_id) return true;
    }
    return false;
}

HWND HwndManager::create(const WindowData& data) {
    std::unique_lock lock(m_mutex);
    HWND h = allocate_handle();

    WindowData entry = data;
    entry.handle = h;
    entry.minimized = (entry.style & WS::MINIMIZE) != 0;
    entry.maximized = !entry.minimized &&
                      (entry.style & WS::MAXIMIZE) != 0;
    entry.normal_rect = entry.rect;
    entry.normal_client_rect = entry.client_rect;

    // Register as a child of the parent window. Learn (window-features
    // z-order): a new top-level (desktop) window goes at the top of its
    // type — the topmost band for a WS_EX_TOPMOST window, otherwise just
    // below the topmost windows; child lists keep creation order (measured,
    // s1win_api_probe gw_child_a / gw_hwndnext_c1: the first child created is
    // at the top).
    m_windows.emplace(h, entry);
    auto parent_it = m_windows.find(entry.parent);
    if (parent_it != m_windows.end()) {
        const bool desktop_top =
            (entry.parent == HWND_DESKTOP) && ((entry.style & WS::CHILD) == 0);
        auto& siblings = parent_it->second.children;
        if (desktop_top) {
            siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(
                                top_of_band_unlocked(siblings, is_topmost_unlocked(h))),
                            h);
        } else {
            siblings.push_back(h);
        }
    }
    return h;
}

void HwndManager::destroy(HWND hwnd) {
    // Desktop cannot be destroyed.
    if (hwnd == HWND_DESKTOP) {
        return;
    }

    std::unique_lock lock(m_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return;
    }

    remove_from_parent_unlocked(hwnd);
    m_windows.erase(it);
}

bool HwndManager::set_parent(HWND hwnd, HWND parent, HWND& previous_parent) {
    if (hwnd == HWND_DESKTOP || hwnd == parent) return false;
    std::unique_lock lock(m_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) return false;
    previous_parent = it->second.parent;
    if (previous_parent == parent) return true;
    remove_from_parent_unlocked(hwnd);
    it->second.parent = parent;
    auto parent_it = m_windows.find(parent);
    if (parent_it != m_windows.end()) {
        auto& siblings = parent_it->second.children;
        const bool desktop_top =
            (parent == HWND_DESKTOP) && ((it->second.style & WS::CHILD) == 0);
        if (desktop_top) {
            const usize at = top_of_band_unlocked(siblings, is_topmost_unlocked(hwnd));
            siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(at), hwnd);
        } else {
            siblings.push_back(hwnd);
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Lookup
// -----------------------------------------------------------------------------

WindowData* HwndManager::find(HWND hwnd) {
    std::shared_lock lock(m_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return nullptr;
    }
    return &it->second;
}

const WindowData* HwndManager::find(HWND hwnd) const {
    std::shared_lock lock(m_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<u32> HwndManager::owner_thread_id(HWND hwnd) const {
    std::shared_lock lock(m_mutex);
    const auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return std::nullopt;
    }
    return it->second.owner_thread_id;
}

std::optional<Rect> HwndManager::client_rect(HWND hwnd) const {
    std::shared_lock lock(m_mutex);
    const auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return std::nullopt;
    }
    return it->second.client_rect;
}

// -----------------------------------------------------------------------------
// Enumeration / Search
// -----------------------------------------------------------------------------

std::vector<HWND> HwndManager::enum_children(HWND parent) const {
    std::shared_lock lock(m_mutex);
    auto it = m_windows.find(parent);
    if (it == m_windows.end()) {
        return {};
    }
    return it->second.children;
}

std::vector<HWND> HwndManager::message_only_windows() const {
    std::shared_lock lock(m_mutex);
    std::vector<HWND> out;
    for (const auto& [hwnd, data] : m_windows) {
        if (data.is_message_only || data.parent == HWND_MESSAGE) out.push_back(hwnd);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool HwndManager::thread_has_windows(u32 tid) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [hwnd, data] : m_windows) {
        if (hwnd != HWND_DESKTOP && data.owner_thread_id == tid) return true;
    }
    return false;
}

HWND HwndManager::child_window_from_point(HWND parent, Point point,
                                           u32 flags) const {
    constexpr u32 CWP_SKIPINVISIBLE = 0x0001;
    constexpr u32 CWP_SKIPDISABLED = 0x0002;
    constexpr u32 CWP_SKIPTRANSPARENT = 0x0004;
    constexpr u32 WS_EX_TRANSPARENT = 0x00000020;

    std::shared_lock lock(m_mutex);
    const auto parent_it = m_windows.find(parent);
    if (parent_it == m_windows.end()) return NULL_HWND;

    const Rect& client = parent_it->second.client_rect;
    const bool point_in_parent =
        point.x >= client.left && point.x < client.right &&
        point.y >= client.top && point.y < client.bottom;

    // GetClientRect(GetDesktopWindow()) reports the primary-monitor rectangle
    // for compatibility, while the desktop owns top-level windows throughout
    // the virtual screen. ChildWindowFromPointEx(HWND_DESKTOP, ...) must still
    // find an immediate top-level child on a secondary monitor. Ordinary child
    // windows remain clipped to their parent's client area.
    if (parent != HWND_DESKTOP && !point_in_parent) {
        return NULL_HWND;
    }

    // children is maintained in z-order with the topmost sibling first.
    for (HWND child : parent_it->second.children) {
        const auto child_it = m_windows.find(child);
        if (child_it == m_windows.end()) continue;
        const WindowData& data = child_it->second;
        if ((flags & CWP_SKIPINVISIBLE) != 0 && !data.visible) continue;
        if ((flags & CWP_SKIPDISABLED) != 0 && !data.enabled) continue;
        if ((flags & CWP_SKIPTRANSPARENT) != 0 &&
            (data.ex_style & WS_EX_TRANSPARENT) != 0) {
            continue;
        }
        if (point.x >= data.rect.left && point.x < data.rect.right &&
            point.y >= data.rect.top && point.y < data.rect.bottom) {
            return child;
        }
    }
    return point_in_parent ? parent : NULL_HWND;
}

HWND HwndManager::window_from_point(Point screen) const {
    constexpr u32 WS_EX_TRANSPARENT = 0x00000020;

    const auto contains = [](const Rect& r, Point p) {
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    };
    const auto is_static_control = [](const WindowData& data) {
        if (data.class_name.size() != 6) {
            return false;
        }
        const wchar_t* expect = L"static";
        for (int i = 0; i < 6; ++i) {
            wchar_t ch = data.class_name[static_cast<usize>(i)];
            if (ch >= L'A' && ch <= L'Z') {
                ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
            if (ch != expect[i]) {
                return false;
            }
        }
        return true;
    };
    const auto eligible = [&](const WindowData& data) {
        if (!data.visible || !data.enabled || data.minimized ||
            data.is_message_only) {
            return false;
        }
        if ((data.ex_style & WS_EX_TRANSPARENT) != 0) {
            return false;
        }
        return true;
    };

    std::shared_lock lock(m_mutex);
    const auto desktop_it = m_windows.find(HWND_DESKTOP);
    if (desktop_it == m_windows.end()) {
        return NULL_HWND;
    }

    const auto* self = this;
    const auto hit_children = [&](auto&& rec, HWND parent, Point local) -> HWND {
        const auto parent_it = self->m_windows.find(parent);
        if (parent_it == self->m_windows.end()) {
            return NULL_HWND;
        }
        for (HWND child : parent_it->second.children) {
            const auto child_it = self->m_windows.find(child);
            if (child_it == self->m_windows.end()) {
                continue;
            }
            const WindowData& data = child_it->second;
            if (!eligible(data) || !contains(data.rect, local)) {
                continue;
            }
            Point child_local = local;
            if (data.client_rect.left == 0 && data.client_rect.top == 0 &&
                (data.client_rect.right != data.rect.right ||
                 data.client_rect.bottom != data.rect.bottom ||
                 data.rect.left != 0 || data.rect.top != 0)) {
                child_local.x = local.x - data.rect.left - data.client_offset.x;
                child_local.y = local.y - data.rect.top - data.client_offset.y;
            }
            const HWND deeper = rec(rec, child, child_local);
            if (deeper != NULL_HWND) {
                return deeper;
            }
            if (is_static_control(data)) {
                continue;
            }
            return child;
        }
        return NULL_HWND;
    };

    for (HWND top : desktop_it->second.children) {
        const auto top_it = m_windows.find(top);
        if (top_it == m_windows.end()) {
            continue;
        }
        const WindowData& data = top_it->second;
        if (!eligible(data) || !contains(data.rect, screen)) {
            continue;
        }
        const Point client{
            screen.x - data.rect.left - data.client_offset.x,
            screen.y - data.rect.top - data.client_offset.y};
        const HWND child = hit_children(hit_children, top, client);
        if (child != NULL_HWND) {
            return child;
        }
        if (is_static_control(data)) {
            continue;
        }
        return top;
    }

    if (desktop_it->second.visible && desktop_it->second.enabled &&
        contains(desktop_it->second.rect, screen)) {
        return HWND_DESKTOP;
    }
    return NULL_HWND;
}

bool HwndManager::is_topmost_unlocked(HWND hwnd) const {
    const auto it = m_windows.find(hwnd);
    return it != m_windows.end() && (it->second.ex_style & kWsExTopmost) != 0;
}

usize HwndManager::top_of_band_unlocked(const std::vector<HWND>& siblings,
                                        bool topmost) const {
    // Learn (Window Features, "Z-Order"): "All topmost windows appear in the
    // z-order before any non-topmost windows." The top of the non-topmost
    // band is the first sibling that is not topmost.
    if (topmost) return 0;
    usize index = 0;
    while (index < siblings.size() && is_topmost_unlocked(siblings[index])) ++index;
    return index;
}

void HwndManager::set_topmost_unlocked(HWND hwnd, bool topmost) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) return;
    if (topmost) it->second.ex_style |= kWsExTopmost;
    else it->second.ex_style &= ~kWsExTopmost;
    // Owned windows follow their owner into and out of the topmost band
    // (measured, s1win_api_probe b_topmost_with_owner /
    // b_topmost_after_owner_notopmost); an owned window alone does not move
    // its owner (a_exstyle_topmost_owner_not_changed).
    for (auto& [other, data] : m_windows) {
        if (other != hwnd && data.owner == hwnd && (data.style & WS::CHILD) == 0) {
            set_topmost_unlocked(other, topmost);
        }
    }
}

bool HwndManager::set_z_order(HWND hwnd, HWND insert_after) {
    std::unique_lock lock(m_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return false;
    }

    HWND parent = it->second.parent;
    auto pit = m_windows.find(parent);
    if (pit == m_windows.end()) {
        return false;
    }

    auto& siblings = pit->second.children;
    auto sit = std::find(siblings.begin(), siblings.end(), hwnd);
    if (sit == siblings.end()) {
        return false;
    }
    const bool top_level =
        parent == HWND_DESKTOP && (it->second.style & WS::CHILD) == 0;

    // HWND_TOPMOST / HWND_NOTOPMOST only mean something for top-level
    // windows; a child is not moved by them (measured, s1win_api_probe
    // swp_c1_topmost_child leaves the child order unchanged).
    if (!top_level &&
        (insert_after == HWND_TOPMOST || insert_after == HWND_NOTOPMOST)) {
        return true;
    }

    if (insert_after == hwnd) return true;
    // An hWndInsertAfter that is not a sibling leaves the order unchanged
    // (measured, s1win_api_probe swp_insert_after_non_sibling: SetWindowPos
    // succeeds and the child order stays as it was).
    if (insert_after != HWND_TOP && insert_after != HWND_BOTTOM &&
        insert_after != HWND_TOPMOST && insert_after != HWND_NOTOPMOST &&
        std::find(siblings.begin(), siblings.end(), insert_after) == siblings.end()) {
        return true;
    }
    siblings.erase(sit);

    if (top_level) {
        if (insert_after == HWND_TOPMOST) set_topmost_unlocked(hwnd, true);
        // Learn SetWindowPos: HWND_NOTOPMOST places the window above all
        // non-topmost windows; HWND_BOTTOM also drops topmost status.
        if (insert_after == HWND_NOTOPMOST || insert_after == HWND_BOTTOM) {
            set_topmost_unlocked(hwnd, false);
        }
    }

    if (insert_after == HWND_TOP || insert_after == HWND_TOPMOST ||
        insert_after == HWND_NOTOPMOST) {
        const usize at = top_level
            ? top_of_band_unlocked(siblings, is_topmost_unlocked(hwnd))
            : 0;
        siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(at), hwnd);
        if (top_level) raise_owned_above_unlocked(siblings, hwnd);
        return true;
    }

    if (insert_after == HWND_BOTTOM) {
        siblings.push_back(hwnd);
        return true;
    }

    auto after_it = std::find(siblings.begin(), siblings.end(), insert_after);
    siblings.insert(std::next(after_it), hwnd);
    return true;
}

void HwndManager::raise_owned_above_unlocked(std::vector<HWND>& siblings, HWND owner) {
    // Learn (Window Features, "Owned Windows"): "An owned window is always
    // above its owner in the z-order." Keep the owned windows' relative order
    // and put them directly above the owner (measured, s1win_api_probe
    // enumwindows_ours: an owned popup stays above its owner when the owner
    // is raised).
    std::vector<HWND> owned;
    const bool owner_topmost = is_topmost_unlocked(owner);
    for (HWND h : siblings) {
        // An owned window in the other band keeps its band's order.
        if (h == owner || is_topmost_unlocked(h) != owner_topmost) continue;
        HWND cur = h;
        for (u32 guard = 0; guard < 64; ++guard) {
            const auto it = m_windows.find(cur);
            if (it == m_windows.end() || it->second.owner == NULL_HWND) break;
            if (it->second.owner == owner) { owned.push_back(h); break; }
            cur = it->second.owner;
        }
    }
    if (owned.empty()) return;
    for (HWND h : owned) {
        siblings.erase(std::remove(siblings.begin(), siblings.end(), h), siblings.end());
    }
    auto owner_it = std::find(siblings.begin(), siblings.end(), owner);
    siblings.insert(owner_it, owned.begin(), owned.end());
}

bool HwndManager::set_theme_nonclient_attributes(HWND hwnd, u32 mask,
                                                  u32 attributes) {
    std::unique_lock lock(m_mutex);
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return false;
    }

    auto& flags = it->second.theme_nonclient_flags;
    flags = (flags & ~mask) | (attributes & mask);
    return true;
}

std::optional<u32> HwndManager::theme_nonclient_attributes(HWND hwnd) const {
    std::shared_lock lock(m_mutex);
    const auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return std::nullopt;
    }
    return it->second.theme_nonclient_flags;
}

namespace {

/// Case-insensitive comparison for window class and title matching.
///
/// Win32 compares both case-insensitively (Microsoft Learn, FindWindowA).
/// Restricted to ASCII case folding deliberately: full Unicode folding needs
/// locale rules this layer does not have, and every window class that matters
/// here is ASCII. Getting a non-ASCII class wrong would be a silent mismatch,
/// so it compares those exactly rather than guessing.
[[nodiscard]] bool iequals_wide(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace

HWND HwndManager::find_window(std::wstring_view class_name,
                               std::wstring_view title) const {
    std::shared_lock lock(m_mutex);
    for (const auto& [h, wd] : m_windows) {
        // Skip desktop
        if (h == HWND_DESKTOP) {
            continue;
        }

        // Class matching. `WindowData::class_name` is recorded at creation
        // (user32/classes/window_class.cpp), so the storage this used to say
        // did not exist has been there all along -- the query simply never
        // read it and `continue`d, which made EVERY class-name lookup return
        // NULL unconditionally.
        //
        // That is not a missing optimisation, it is a Win32 contract that
        // could not succeed. FindWindow(class, ...) is how a Windows program
        // does a single-instance check, hands off to an already-running
        // instance, or waits for another process's window to appear. A query
        // that can only ever return NULL turns the third case into a wait that
        // never ends.
        //
        // This is a correctness fix, not a performance one. FindWindow is not
        // a hot path: a launcher makes 21-150 calls across a ~100 s run, and
        // the same two calls resolve before and after class matching works.
        //
        // Microsoft Learn, FindWindowA: the class name is compared
        // case-insensitively, and a NULL class matches any class.
        if (!class_name.empty() &&
            !iequals_wide(wd.class_name, class_name)) {
            continue;
        }

        // Title matching. NULL/empty title matches any window, and the
        // comparison is case-insensitive for the same reason.
        if (!title.empty() && !iequals_wide(wd.title, title)) {
            continue;
        }

        return h;
    }

    // Process-local by design. Win32 FindWindow is system-wide, but this layer
    // is below IPC and must not reach across processes: the capsule-wide
    // fallback lives in the user32 FindWindow implementation, which already
    // owns the HWND registry. Keeping it there is what stops `windowing` from
    // acquiring a dependency on `ipc`.
    return NULL_HWND;
}

// -----------------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------------

usize HwndManager::count() const {
    std::shared_lock lock(m_mutex);
    // Exclude the desktop entry.
    VITREO_ASSERT(m_windows.size() >= 1);
    return m_windows.size() - 1;
}

bool HwndManager::is_valid(HWND hwnd) const {
    std::shared_lock lock(m_mutex);
    return m_windows.find(hwnd) != m_windows.end();
}

void HwndManager::remove_from_parent_unlocked(HWND hwnd) {
    auto it = m_windows.find(hwnd);
    if (it == m_windows.end()) {
        return;
    }

    HWND parent = it->second.parent;
    auto parent_it = m_windows.find(parent);
    if (parent_it == m_windows.end()) {
        return;
    }

    auto& siblings = parent_it->second.children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), hwnd),
                   siblings.end());
}

// -----------------------------------------------------------------------------
// Mouse Capture
// -----------------------------------------------------------------------------

HWND HwndManager::set_capture(HWND hwnd) {
    return m_capture_hwnd.exchange(hwnd, std::memory_order_acq_rel);
}

void HwndManager::release_capture() {
    m_capture_hwnd.store(NULL_HWND, std::memory_order_release);
}

} // namespace vitreo::windowing
