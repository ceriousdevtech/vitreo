#pragma once

/// @file nc_metrics.hpp
/// @brief The non-client frame metrics USER uses for AdjustWindowRectEx,
/// DefWindowProc's WM_NCCALCSIZE, CreateWindowEx's initial client area,
/// GetSystemMetrics and the default WM_GETMINMAXINFO tracking size.
///
/// One definition, so a window created from AdjustWindowRectEx's outer size
/// gets exactly the client area the caller asked for.
///
/// Measured on Windows 11 at 96 DPI (tools/oracle_probe/s1win_api_probe.c
/// "-- adjustwindowrect", s1win_display_probe.c "-- metrics"):
///  * a sizing frame (WS_THICKFRAME) is 8 pixels with WS_BORDER and 7
///    without; a dialog frame (WS_DLGFRAME, part of WS_CAPTION) is 3; a
///    plain WS_BORDER is 1;
///  * WS_EX_DLGMODALFRAME adds a dialog frame when WS_DLGFRAME is absent;
///    WS_EX_CLIENTEDGE adds 2, WS_EX_STATICEDGE 1;
///  * the menu bar is 20 (SM_CYMENU).
///
/// The caption heights are Vitreo's native-chrome policy
/// (.docs/windowing-native-presentation.md): a stock caption is drawn by the
/// macOS title bar (28 points), so SM_CYCAPTION is 30 rather than Windows'
/// 23 and SM_CYSMCAPTION 22 rather than 23. The maximized-window geometry
/// relies on the caption covering the title bar; this is the one documented
/// deviation (area-S1-windowing.md, "Caption metric").

#include <vitreo/common.hpp>

namespace vitreo::winapi::nc_metrics {

constexpr i32 kCaption = 30;       ///< SM_CYCAPTION (Windows: 23; native title bar)
constexpr i32 kSmallCaption = 22;  ///< SM_CYSMCAPTION (Windows: 23)
constexpr i32 kWindowsCaption = 23;
constexpr i32 kBorder = 1;         ///< SM_CXBORDER
constexpr i32 kFixedFrame = 3;     ///< SM_CXFIXEDFRAME / SM_CXDLGFRAME
constexpr i32 kSizeFrame = 4;      ///< SM_CXSIZEFRAME for a Vista+ image
constexpr i32 kPaddedBorder = 4;   ///< SM_CXPADDEDBORDER for a Vista+ image
constexpr i32 kMenu = 20;          ///< SM_CYMENU
constexpr i32 kEdge = 2;           ///< SM_CXEDGE
constexpr i32 kScrollBar = 17;     ///< SM_CXVSCROLL / SM_CYHSCROLL

constexpr u32 kWsBorder = 0x00800000u;
constexpr u32 kWsDlgFrame = 0x00400000u;
constexpr u32 kWsThickFrame = 0x00040000u;
constexpr u32 kWsCaption = 0x00C00000u;
constexpr u32 kWsVScroll = 0x00200000u;
constexpr u32 kWsHScroll = 0x00100000u;
constexpr u32 kWsExDlgModalFrame = 0x00000001u;
constexpr u32 kWsExToolWindow = 0x00000080u;
constexpr u32 kWsExClientEdge = 0x00000200u;
constexpr u32 kWsExStaticEdge = 0x00020000u;

/// MulDiv(value, dpi, 96), rounding half away from zero as MulDiv does.
inline i32 scale(i32 value, u32 dpi) noexcept {
    const i64 n = static_cast<i64>(value) * static_cast<i64>(dpi);
    return static_cast<i32>(n >= 0 ? (n + 48) / 96 : (n - 48) / 96);
}

/// SM_CXSIZEFRAME at @p dpi (measured with GetSystemMetricsForDpi at 96,
/// 108, 120, 144, 168, 192, 240, 288, 384: 4 4 4 5 5 5 6 6 7); other DPIs
/// take the nearest lower measurement.
inline i32 size_frame_for_dpi(u32 dpi) noexcept {
    static constexpr u32 kDpis[] = {96, 108, 120, 144, 168, 192, 240, 288, 384};
    static constexpr i32 kValues[] = {4, 4, 4, 5, 5, 5, 6, 6, 7};
    i32 v = kValues[0];
    for (usize i = 0; i < 9; ++i) {
        if (dpi >= kDpis[i]) v = kValues[i];
    }
    return v;
}

/// SM_CYCAPTION / SM_CYSMCAPTION at @p dpi: Windows' MulDiv(22, dpi, 96) + 1
/// (measured), plus the native-chrome difference at 96 DPI.
inline i32 caption_for_dpi(u32 dpi, bool small) noexcept {
    return scale(kWindowsCaption - 1, dpi) + 1 +
           ((small ? kSmallCaption : kCaption) - kWindowsCaption);
}

/// SM_CYMENU at @p dpi: MulDiv(19, dpi, 96) + 1 (measured).
inline i32 menu_for_dpi(u32 dpi) noexcept { return scale(19, dpi) + 1; }

/// AdjustWindowRectExForDpi's insets: the sizing frame and padded border
/// scale with the DPI, the dialog frame, border and edges do not (measured,
/// s1win_api_probe awrexfordpi_*).
inline void insets_for_dpi(u32 style, u32 ex_style, bool has_menu, u32 dpi,
                           i32& left, i32& top, i32& right, i32& bottom) noexcept;

/// Width of the frame on each side (and the bottom) for these styles.
inline i32 frame_thickness(u32 style, u32 ex_style) noexcept {
    i32 f = 0;
    if (style & kWsThickFrame) {
        f = kSizeFrame + kPaddedBorder - kBorder + ((style & kWsBorder) ? kBorder : 0);
    } else if (style & kWsDlgFrame) {
        f = kFixedFrame;
    } else if (style & kWsBorder) {
        f = kBorder;
    }
    if ((ex_style & kWsExDlgModalFrame) && !(style & kWsDlgFrame)) f += kFixedFrame;
    if (ex_style & kWsExClientEdge) f += kEdge;
    if (ex_style & kWsExStaticEdge) f += kBorder;
    return f;
}

/// Caption height above the client area (0 without a full WS_CAPTION).
inline i32 caption_height(u32 style, u32 ex_style) noexcept {
    if ((style & kWsCaption) != kWsCaption) return 0;
    return (ex_style & kWsExToolWindow) ? kSmallCaption : kCaption;
}

struct Insets {
    i32 left = 0, top = 0, right = 0, bottom = 0;
};

/// Distance from each window edge to the client edge, as AdjustWindowRectEx
/// adds it (no scroll bars) or DefWindowProc's WM_NCCALCSIZE removes it
/// (@p scroll_bars: WS_VSCROLL / WS_HSCROLL take SM_CXVSCROLL / SM_CYHSCROLL).
inline Insets insets(u32 style, u32 ex_style, bool has_menu, bool scroll_bars) noexcept {
    Insets in;
    const i32 f = frame_thickness(style, ex_style);
    in.left = in.right = in.bottom = f;
    in.top = f + caption_height(style, ex_style) + (has_menu ? kMenu : 0);
    if (scroll_bars) {
        if (style & kWsVScroll) in.right += kScrollBar;
        if (style & kWsHScroll) in.bottom += kScrollBar;
    }
    return in;
}

inline void insets_for_dpi(u32 style, u32 ex_style, bool has_menu, u32 dpi,
                           i32& left, i32& top, i32& right, i32& bottom) noexcept {
    i32 f = 0;
    if (style & kWsThickFrame) {
        f = size_frame_for_dpi(dpi) + scale(kPaddedBorder, dpi) - kBorder +
            ((style & kWsBorder) ? kBorder : 0);
    } else if (style & kWsDlgFrame) {
        f = kFixedFrame;
    } else if (style & kWsBorder) {
        f = kBorder;
    }
    if ((ex_style & kWsExDlgModalFrame) && !(style & kWsDlgFrame)) f += kFixedFrame;
    if (ex_style & kWsExClientEdge) f += kEdge;
    if (ex_style & kWsExStaticEdge) f += kBorder;
    left = right = bottom = f;
    top = f;
    if ((style & kWsCaption) == kWsCaption) {
        top += caption_for_dpi(dpi, (ex_style & kWsExToolWindow) != 0);
    }
    if (has_menu) top += menu_for_dpi(dpi);
}

} // namespace vitreo::winapi::nc_metrics
