#pragma once

#include <vitreo/common.hpp>

namespace vitreo::graphics {

struct DisplayMode {
    u32 width{0};
    u32 height{0};
    u32 bits_per_pixel{0};
    u32 refresh_rate{0};
};

class PresentationState {
public:
    void set_target(u32 hwnd) noexcept { m_hwnd = hwnd; }
    void set_display_mode(DisplayMode mode) noexcept { m_display_mode = mode; }

    [[nodiscard]] u32 target() const noexcept { return m_hwnd; }
    [[nodiscard]] const DisplayMode& display_mode() const noexcept {
        return m_display_mode;
    }

private:
    u32 m_hwnd{0};
    DisplayMode m_display_mode{};
};

} // namespace vitreo::graphics