#pragma once

#include "winapi/core/thunk_registry.hpp"

#include <functional>

namespace vitreo::winapi {

class Marshaler;

/// Invoked when DwmExtendFrameIntoClientArea changes whether the guest draws
/// into the frame area. @p client_drawn is true for non-zero / negative
/// margins (including the documented "sheet of glass" {-1} case).
using DwmClientDrawnFrameFn = std::function<void(u32 hwnd, bool client_drawn)>;

/// What DwmEnableBlurBehindWindow needs to know about an HWND: DWM accepts
/// only live top-level windows (measured: a child window is E_INVALIDARG).
enum class DwmWindowKind { Invalid, Child, TopLevel };

/// Window-manager and GDI facts the runtime injects. Unset functions mean
/// "no window manager": only a NULL HWND / NULL HRGN is rejected.
struct DwmWindowServices {
    std::function<DwmWindowKind(u32 hwnd)> classify_window;
    std::function<bool(u64 hrgn)> is_region;
    /// DWMWA_EXTENDED_FRAME_BOUNDS: the visible frame of a top-level window
    /// in screen coordinates (left, top, right, bottom). Unset or false
    /// means the bounds are not known.
    std::function<bool(u32 hwnd, i32* rect)> frame_bounds;
    /// Whether DWM would draw this window's non-client frame under
    /// DWMNCRP_USEWINDOWSTYLE (the window has a caption or a frame).
    std::function<bool(u32 hwnd)> has_frame;
};

/// Primary composition display rate captured by the runtime at registration.
DllExports create_dwmapi_thunks(DwmClientDrawnFrameFn on_client_drawn = {},
                               double refresh_rate_hz = 60.0,
                               DwmWindowServices services = {});

} // namespace vitreo::winapi
