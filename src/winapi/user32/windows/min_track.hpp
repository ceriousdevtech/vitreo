#pragma once

/// @file min_track.hpp
/// @brief The default MINMAXINFO.ptMinTrackSize USER fills in before
/// WM_GETMINMAXINFO.
///
/// Measured on Windows 11 at 96 DPI (tools/oracle_probe/s1win_api_probe.c,
/// create_*_tiny / create_overlapped_zero_mintrack): a window with a caption
/// (and not WS_EX_TOOLWINDOW) cannot be smaller than SM_CXMINTRACK x
/// SM_CYMINTRACK = 136 x 39; any other window's minimum is its two frame
/// borders (a WS_THICKFRAME pop-up 14 x 14, a captioned tool window with a
/// sizing frame 16 x 16).

#include <vitreo/common.hpp>
#include "winapi/user32/windows/nc_metrics.hpp"

namespace vitreo::winapi {

inline void default_min_track_size(u32 style, u32 ex_style, i32& width, i32& height) {
    constexpr u32 kCaption = 0x00C00000u;
    constexpr u32 kThickFrame = 0x00040000u;
    constexpr u32 kDlgFrame = 0x00400000u;
    constexpr u32 kBorder = 0x00800000u;
    constexpr u32 kToolWindow = 0x00000080u;
    if ((style & kCaption) == kCaption && (ex_style & kToolWindow) == 0) {
        width = 136;
        height = 39;
        return;
    }
    (void)kThickFrame; (void)kDlgFrame; (void)kBorder;
    const i32 frame = nc_metrics::frame_thickness(style, ex_style);
    width = 2 * frame;
    height = 2 * frame;
}

} // namespace vitreo::winapi
