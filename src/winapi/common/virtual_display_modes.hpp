// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/common.hpp>
#include <vector>

namespace vitreo::winapi {

struct VirtualDisplayMode {
    u32 width;
    u32 height;
};

inline std::vector<VirtualDisplayMode> virtual_display_modes(u32 width, u32 height) {
    static constexpr VirtualDisplayMode common[] = {
        {320, 200}, {320, 240}, {640, 400}, {640, 480},
        {800, 500}, {800, 600}, {960, 600}, {1024, 640},
        {1024, 768}, {1152, 864}, {1280, 720}, {1280, 800},
        {1280, 960}, {1280, 1024}, {1440, 900}, {1600, 900},
        {1600, 1000}, {1600, 1200}, {1680, 1050}, {1920, 1080},
        {1920, 1200}, {2560, 1440}, {2560, 1600}, {3840, 2160},
    };
    std::vector<VirtualDisplayMode> modes{{width, height}};
    for (const auto& mode : common) {
        if (mode.width <= width && mode.height <= height &&
            (mode.width != width || mode.height != height)) {
            modes.push_back(mode);
        }
    }
    return modes;
}

inline VirtualDisplayMode closest_virtual_display_mode(
    u32 desktop_width, u32 desktop_height, u32 width, u32 height) {
    auto best = VirtualDisplayMode{desktop_width, desktop_height};
    if (width == 0 && height == 0) return best;
    const auto distance = [width, height](VirtualDisplayMode mode) {
        const u64 dx = mode.width > width ? mode.width - width : width - mode.width;
        const u64 dy = mode.height > height ? mode.height - height : height - mode.height;
        return static_cast<long double>(dx) * dx +
               static_cast<long double>(dy) * dy;
    };
    for (const auto mode : virtual_display_modes(desktop_width, desktop_height)) {
        if (distance(mode) < distance(best)) best = mode;
    }
    return best;
}

} // namespace vitreo::winapi
