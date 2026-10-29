#pragma once

#include <vitreo/common.hpp>

namespace vitreo::windowing {

struct MonitorInfo {
    i32 width = 1920;
    i32 height = 1080;
    i32 left = 0;
    i32 top = 0;
    i32 right = 1920;
    i32 bottom = 1080;
    i32 work_left = 0;
    i32 work_top = 0;
    i32 work_right = 1920;
    i32 work_bottom = 1040;
    double scale_factor = 2.0;
    u32 bits_per_color = 8;
    double refresh_rate_hz = 0.0;
    /// Physical panel size (CGDisplayScreenSize); 0 when unknown.
    double physical_width_mm = 0.0;
    double physical_height_mm = 0.0;
    bool primary = true;
};

struct VirtualDesktopInfo {
    i32 origin_x = 0;
    i32 origin_y = 0;
    i32 width = 1920;
    i32 height = 1080;
    i32 monitor_count = 1;
};

} // namespace vitreo::windowing
