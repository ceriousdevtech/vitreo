// SPDX-License-Identifier: LGPL-2.1-only

/// @file system_metrics.cpp
/// @brief GetSystemMetrics / GetSystemMetricsForDpi.
///
/// Contract: Microsoft Learn GetSystemMetrics (index meanings) and
/// GetSystemMetricsForDpi. The values are measured on Windows 11
/// (tools/oracle_probe/s1win_display_probe.c "-- metrics", .expected):
/// every index 0..96 at 96 DPI, and every index at 96, 108, 120, 144, 168,
/// 192, 240, 288 and 384 DPI through GetSystemMetricsForDpi. Only 32 indices
/// depend on the DPI; each is either MulDiv(value, dpi, 96) or one of the
/// measured tables below.
///
/// Measured rules the table encodes:
///  * indices 97 and up, negative indices and unknown 0x1xxx values return 0
///    with ERROR_INVALID_PARAMETER; 0..96 and 0x2000..0x2004 never set it;
///  * an image linked for a subsystem older than 6.0 gets SM_CXFRAME 8 and
///    SM_CXPADDEDBORDER 0; a Vista+ image gets 4 and 4
///    (s1win_display_probe64 vs s1win_display_probe64v6);
///  * GetSystemMetricsForDpi(dpi 0) is 0 with ERROR_INVALID_PARAMETER and
///    always uses the Vista+ frame values.
/// Screen, work-area and monitor metrics come from the host display. Values
/// that describe the machine rather than USER are Vitreo's: not a remote
/// session, a wheel mouse with horizontal scrolling (Vitreo delivers
/// WM_MOUSEHWHEEL), caret blinking on. The caption heights follow the
/// native-chrome policy (nc_metrics.hpp).

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <array>

namespace vitreo::winapi {

namespace {

constexpr std::array<u32, 9> kMeasuredDpis{96, 108, 120, 144, 168, 192, 240, 288, 384};

/// MulDiv(value, dpi, 96), rounding half away from zero as MulDiv does.
i32 scale(i32 value, u32 dpi) noexcept {
    const i64 n = static_cast<i64>(value) * static_cast<i64>(dpi);
    return static_cast<i32>(n >= 0 ? (n + 48) / 96 : (n - 48) / 96);
}

/// A metric measured at kMeasuredDpis; between two measured DPIs the value is
/// interpolated, above 384 it is extrapolated proportionally.
i32 measured(const std::array<i32, 9>& values, u32 dpi) noexcept {
    for (usize i = 0; i < kMeasuredDpis.size(); ++i) {
        if (dpi == kMeasuredDpis[i]) return values[i];
    }
    if (dpi < kMeasuredDpis.front()) return scale(values.front(), dpi);
    for (usize i = 1; i < kMeasuredDpis.size(); ++i) {
        if (dpi < kMeasuredDpis[i]) {
            const i64 d0 = kMeasuredDpis[i - 1], d1 = kMeasuredDpis[i];
            const i64 v0 = values[i - 1], v1 = values[i];
            return static_cast<i32>(v0 + ((v1 - v0) * (static_cast<i64>(dpi) - d0) + (d1 - d0) / 2) /
                                               (d1 - d0));
        }
    }
    return static_cast<i32>((static_cast<i64>(values.back()) * dpi + 192) / 384);
}

constexpr std::array<i32, 9> kMinTrackWidth{136, 154, 166, 198, 232, 258, 324, 384, 510};
constexpr std::array<i32, 9> kMinTrackHeight{39, 44, 47, 56, 64, 71, 88, 103, 135};
constexpr std::array<i32, 9> kCaptionButtonWidth{36, 41, 46, 54, 64, 72, 91, 109, 145};
constexpr std::array<i32, 9> kMenuCheck{15, 19, 19, 25, 29, 31, 41, 47, 65};

/// Cursors come in 32, 48, 64, 96 and 128 pixels (measured: 32 up to 120
/// DPI, 48 at 144-168, 64 at 192-240, then the integer scale).
i32 cursor_size(u32 dpi) noexcept {
    const u32 twice = dpi * 2 / 96;  // scale in halves
    if (twice < 3) return 32;
    if (twice < 4) return 48;
    return 32 * static_cast<i32>(twice / 2);
}

} // namespace

void User32ExtManager::set_legacy_frame_metrics(bool legacy) noexcept {
    m_legacy_frame_metrics = legacy;
}

i32 User32ExtManager::system_metric(i32 index, u32 dpi, bool& valid) const {
    valid = true;
    const bool for_dpi = dpi != 0;
    if (!for_dpi) dpi = 96;
    namespace nc = nc_metrics;

    // Host display. A virtual full-screen mode (ChangeDisplaySettings) is the
    // whole desktop while it lasts.
    const bool virtual_mode = m_virtual_display_width > 0 && m_virtual_display_height > 0;
    i32 screen_w = m_virtual_display_width;
    i32 screen_h = m_virtual_display_height;
    windowing::Rect work{0, 0, screen_w, screen_h};
    if (!virtual_mode) {
        const windowing::MonitorInfo primary = m_host.get_primary_monitor_info();
        screen_w = primary.right - primary.left;
        screen_h = primary.bottom - primary.top;
        work = windowing::Rect{primary.work_left, primary.work_top, primary.work_right,
                               primary.work_bottom};
    }
    const windowing::Rect virt = guest_virtual_screen_rect();
    const i32 monitors = virtual_mode ? 1 : m_host.get_virtual_desktop_info().monitor_count;

    switch (index) {
    case 0: return screen_w;                                   // SM_CXSCREEN
    case 1: return screen_h;                                   // SM_CYSCREEN
    case 2: case 3: case 9: case 10: case 20: case 21:
        return scale(nc::kScrollBar, dpi);                     // scroll bars, thumbs
    case 4: return nc::caption_for_dpi(dpi, false);            // SM_CYCAPTION
    case 5: case 6: return nc::kBorder;                        // SM_CXBORDER
    case 7: case 8: return nc::kFixedFrame;                    // SM_CXDLGFRAME
    case 11: case 12: return scale(32, dpi);                   // SM_CXICON
    case 13: case 14: return cursor_size(dpi);                 // SM_CXCURSOR
    case 15: return nc::menu_for_dpi(dpi);                     // SM_CYMENU
    case 16: return work.width();                              // SM_CXFULLSCREEN
    case 17: return work.height() - system_metric(4, for_dpi ? dpi : 0, valid);
    case 18: return 0;                                         // SM_CYKANJIWINDOW
    case 19: return 1;                                         // SM_MOUSEPRESENT
    case 22: return 0;                                         // SM_DEBUG
    case 23: return 0;                                         // SM_SWAPBUTTON
    case 24: case 25: case 26: case 27: return 0;              // reserved
    case 28: case 34: return measured(kMinTrackWidth, dpi);    // SM_CXMIN / SM_CXMINTRACK
    case 29: case 35: return measured(kMinTrackHeight, dpi);   // SM_CYMIN / SM_CYMINTRACK
    case 30: return measured(kCaptionButtonWidth, dpi);        // SM_CXSIZE
    case 31: return scale(22, dpi);                            // SM_CYSIZE
    case 32: case 33:                                          // SM_CXFRAME
        if (!for_dpi && m_legacy_frame_metrics) return nc::kSizeFrame + nc::kPaddedBorder;
        return nc::size_frame_for_dpi(dpi);
    case 36: case 37: return 4;                                // SM_CXDOUBLECLK
    case 38: case 39: return scale(75, dpi);                   // SM_CXICONSPACING
    case 40: return 0;                                         // SM_MENUDROPALIGNMENT
    case 41: return 0;                                         // SM_PENWINDOWS
    case 42: return 0;                                         // SM_DBCSENABLED
    case 43: return 5;                                         // SM_CMOUSEBUTTONS
    case 44: return 0;                                         // SM_SECURE
    case 45: case 46: return nc::kEdge;                        // SM_CXEDGE
    case 47: return 160;                                       // SM_CXMINSPACING
    case 48: return 28;                                        // SM_CYMINSPACING
    case 49: case 50: return scale(16, dpi);                   // SM_CXSMICON
    case 51: return nc::caption_for_dpi(dpi, true);            // SM_CYSMCAPTION
    case 52: case 53: return scale(22, dpi);                   // SM_CXSMSIZE
    case 54: case 55: return scale(19, dpi);                   // SM_CXMENUSIZE
    case 56: return 8;                                         // SM_ARRANGE (ARW_HIDE)
    case 57: return 160;                                       // SM_CXMINIMIZED
    case 58: return 28;                                        // SM_CYMINIMIZED
    case 59: return virt.width() + 20;                         // SM_CXMAXTRACK
    case 60: return virt.height() + 20;                        // SM_CYMAXTRACK
    case 61: return work.width() + 16;                         // SM_CXMAXIMIZED
    case 62: return work.height() + 16;                        // SM_CYMAXIMIZED
    case 63: return 3;                                         // SM_NETWORK
    case 64: case 65: case 66: return 0;
    case 67: return 0;                                         // SM_CLEANBOOT
    case 68: case 69: return 4;                                // SM_CXDRAG
    case 70: return 0;                                         // SM_SHOWSOUNDS
    case 71: case 72: return measured(kMenuCheck, dpi);        // SM_CXMENUCHECK
    case 73: return 0;                                         // SM_SLOWMACHINE
    case 74: return 0;                                         // SM_MIDEASTENABLED
    case 75: return 1;                                         // SM_MOUSEWHEELPRESENT
    case 76: return virt.left;                                 // SM_XVIRTUALSCREEN
    case 77: return virt.top;                                  // SM_YVIRTUALSCREEN
    case 78: return virt.width();                              // SM_CXVIRTUALSCREEN
    case 79: return virt.height();                             // SM_CYVIRTUALSCREEN
    case 80: return monitors;                                  // SM_CMONITORS
    case 81: return 1;                                         // SM_SAMEDISPLAYFORMAT
    case 82: return 1;                                         // SM_IMMENABLED
    case 83: case 84: return 1;                                // SM_CXFOCUSBORDER
    case 85: case 86: case 87: case 88: case 89: case 90: return 0;  // TABLETPC, MEDIACENTER, ...
    case 91: return 1;                                         // SM_MOUSEHORIZONTALWHEELPRESENT
    case 92:                                                   // SM_CXPADDEDBORDER
        if (!for_dpi && m_legacy_frame_metrics) return 0;
        return scale(nc::kPaddedBorder, dpi);
    case 93: case 94: case 95: case 96: return 0;              // DIGITIZER, MAXIMUMTOUCHES
    case 0x1000: return 0;                                     // SM_REMOTESESSION
    case 0x2000: return 0;                                     // SM_SHUTTINGDOWN
    case 0x2001: return 0;                                     // SM_REMOTECONTROL
    case 0x2002: return 1;                                     // SM_CARETBLINKINGENABLED
    case 0x2003: return 0;                                     // SM_CONVERTIBLESLATEMODE
    case 0x2004: return 0;                                     // SM_SYSTEMDOCKED
    default:
        valid = false;
        return 0;
    }
}

void User32ExtManager::impl_GetSystemMetrics(Marshaler& m) {
    const i32 index = static_cast<i32>(m.arg(0));
    bool valid = true;
    const i32 result = system_metric(index, 0, valid);
    if (!valid) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    VITREO_TRACE("[user32_ext] GetSystemMetrics(%d) -> %d\n", index, result);
    m.set_return(static_cast<u64>(static_cast<u32>(result)));
}

void User32ExtManager::impl_GetSystemMetricsForDpi(Marshaler& m) {
    // int GetSystemMetricsForDpi(int nIndex, UINT dpi)
    const i32 index = static_cast<i32>(m.arg(0));
    const u32 dpi = static_cast<u32>(m.arg(1));
    if (dpi == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    bool valid = true;
    const i32 result = system_metric(index, dpi, valid);
    if (!valid) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(static_cast<u64>(static_cast<u32>(result)));
}

} // namespace vitreo::winapi
