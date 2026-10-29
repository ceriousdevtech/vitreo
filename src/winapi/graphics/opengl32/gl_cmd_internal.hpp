#pragma once
/// @file gl_cmd_internal.hpp
/// @brief Helpers shared by the gl_cmd_*.cpp command files.

#include "gl_commands.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace vitreo::winapi::gl::detail {

[[nodiscard]] inline float clamp01(double v) noexcept {
    return static_cast<float>(std::clamp(v, 0.0, 1.0));
}

/// An enumerant passed through a float or integer parameter, or nullopt when
/// the value is not an exact non-negative integer.
[[nodiscard]] inline std::optional<u32> as_enum(double v) noexcept {
    if (!(v >= 0.0) || v > 4294967295.0 || std::floor(v) != v) return std::nullopt;
    return static_cast<u32>(v);
}

/// Round to nearest for integer state set through a float form (spec 3.6.1).
[[nodiscard]] inline i32 round_to_int(double v) noexcept {
    if (!(v == v)) return 0;
    const double r = std::round(v);
    if (r >= 2147483647.0) return 2147483647;
    if (r <= -2147483648.0) return static_cast<i32>(-2147483647 - 1);
    return static_cast<i32>(r);
}

/// A colour component given through GlValues: integer forms use spec
/// Table 2.6's signed-int conversion.
[[nodiscard]] inline double color_component(const GlValues& values, usize i) noexcept {
    return values.from_int ? static_cast<double>(gl_int_to_float(static_cast<i32>(values[i])))
                           : values[i];
}

/// M * (x, y, z, w) for column-major M.
[[nodiscard]] inline std::array<double, 4> transform_point(const GlMatrix& m,
                                                           const std::array<double, 4>& p) noexcept {
    std::array<double, 4> r{};
    for (u32 row = 0; row < 4; ++row) {
        r[row] = m[0 * 4 + row] * p[0] + m[1 * 4 + row] * p[1] + m[2 * 4 + row] * p[2] +
                 m[3 * 4 + row] * p[3];
    }
    return r;
}

/// The row vector p * M^-1 (spec 2.10.4 eye planes, 2.11 clip planes); p
/// unchanged when the model-view matrix is singular (spec: "undefined").
[[nodiscard]] inline std::array<double, 4> plane_to_eye(const GlMatrix& modelview,
                                                        const std::array<double, 4>& p) noexcept {
    const auto inv = gl_invert(modelview);
    if (!inv) return p;
    std::array<double, 4> r{};
    for (u32 col = 0; col < 4; ++col) {
        r[col] = p[0] * (*inv)[col * 4 + 0] + p[1] * (*inv)[col * 4 + 1] +
                 p[2] * (*inv)[col * 4 + 2] + p[3] * (*inv)[col * 4 + 3];
    }
    return r;
}

/// Spec 2.13.3: with COLOR_MATERIAL enabled the selected material
/// parameters track the current colour.
void apply_color_material(GlContext& ctx) noexcept;

}  // namespace vitreo::winapi::gl::detail
