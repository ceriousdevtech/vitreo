#pragma once

#include "windowing/model/window_types.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace vitreo::windowing {

// Generated from winuser.h WHEEL_DELTA.
inline constexpr i16 WHEEL_DELTA = 120;

// AppKit supplies fractional lines or precise pixels. WM_MOUSEWHEEL permits
// divisions of WHEEL_DELTA; the guest decides whether to accumulate them.
// Keep only the fraction of one Windows wheel unit, never a whole notch.
class WheelDeltaAccumulator {
public:
    i32 translate(HWND target, double delta, bool precise) noexcept {
        if (target != m_target || precise != m_precise) {
            m_fraction = 0;
            m_target = target;
            m_precise = precise;
        }
        if (!std::isfinite(delta)) return 0;
        // Preserve the existing host calibration (12 precise pixels/notch).
        const double scale = static_cast<double>(WHEEL_DELTA) / (precise ? 12.0 : 1.0);
        const double total = std::clamp(delta * scale + m_fraction,
            static_cast<double>(std::numeric_limits<i32>::min()),
            static_cast<double>(std::numeric_limits<i32>::max()));
        const auto result = static_cast<i32>(total);
        m_fraction = total - result;
        return result;
    }

private:
    HWND m_target = NULL_HWND;
    bool m_precise = false;
    double m_fraction = 0;
};

} // namespace vitreo::windowing
