#pragma once

#include "winapi/core/thunk_registry.hpp"

#include <functional>

namespace vitreo::winapi {

class Marshaler;
class GdiManager;

} // namespace vitreo::winapi

namespace vitreo::windowing {
class HwndManager;
class ThemeHost;
}

namespace vitreo::winapi {

/// What uxtheme needs from the rest of the runtime. Every member may be
/// null/empty (unit tests); the export then fails the way Windows fails
/// without that service (no DC: E_INVALIDARG, no window: E_HANDLE ...).
struct UxThemeServices {
    windowing::HwndManager* hwnd_mgr = nullptr;
    GdiManager* gdi = nullptr;
    /// AppKit's palette; null = the built-in macOS palette.
    windowing::ThemeHost* host = nullptr;
    /// SendMessageW to a window (WM_THEMECHANGED, WM_ERASEBKGND, WM_PRINTCLIENT).
    std::function<u64(u32 hwnd, u32 msg, u64 wparam, u64 lparam)> send_message;
    /// GetSystemMetrics(index).
    std::function<i32(i32 index)> system_metric;
    /// MapWindowPoints of a client point from @p from to @p to.
    std::function<bool(u32 from, u32 to, i32& x, i32& y)> map_point;
    /// GetParent.
    std::function<u32(u32 hwnd)> parent;
};

DllExports create_uxtheme_thunks(windowing::HwndManager* hwnd_mgr = nullptr);
DllExports create_uxtheme_thunks(const UxThemeServices& services);

} // namespace vitreo::winapi
