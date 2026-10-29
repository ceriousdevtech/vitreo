#pragma once
/// @file crt_math.hpp
/// @brief msvcrt math functions (WIN32-083)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <string>
#include <unordered_map>
#include <utility>

namespace vitreo::winapi {

class Marshaler;
class CrtMemoryManager;

/// msvcrt math function implementations
class CrtMathManager {
public:
    CrtMathManager() = default;

    // Trigonometric
    [[nodiscard]] static f64 crt_sin(f64 x);
    [[nodiscard]] static f64 crt_cos(f64 x);
    [[nodiscard]] static f64 crt_tan(f64 x);
    [[nodiscard]] static f64 crt_asin(f64 x);
    [[nodiscard]] static f64 crt_acos(f64 x);
    [[nodiscard]] static f64 crt_atan(f64 x);
    [[nodiscard]] static f64 crt_atan2(f64 y, f64 x);

    // Power/Exponential
    [[nodiscard]] static f64 crt_sqrt(f64 x);
    [[nodiscard]] static f64 crt_pow(f64 x, f64 y);
    [[nodiscard]] static f64 crt_exp(f64 x);
    [[nodiscard]] static f64 crt_log(f64 x);
    [[nodiscard]] static f64 crt_log10(f64 x);
    [[nodiscard]] static f64 crt_ldexp(f64 x, i32 exp);
    [[nodiscard]] static f64 crt_frexp(f64 x, i32& exp);

    // Rounding
    [[nodiscard]] static f64 crt_floor(f64 x);
    [[nodiscard]] static f64 crt_ceil(f64 x);
    [[nodiscard]] static f64 crt_fmod(f64 x, f64 y);
    [[nodiscard]] static f64 crt_modf(f64 x, f64& iptr);

    // Utility
    [[nodiscard]] static f64 crt_fabs(f64 x);
    [[nodiscard]] static f64 crt_hypot(f64 x, f64 y);
    [[nodiscard]] static f64 crt_copysign(f64 x, f64 y);
    [[nodiscard]] static i32 crt_isnan(f64 x);
    [[nodiscard]] static i32 crt_finite(f64 x);

    // Float variants
    [[nodiscard]] static f32 crt_sinf(f32 x);
    [[nodiscard]] static f32 crt_cosf(f32 x);
    [[nodiscard]] static f32 crt_sqrtf(f32 x);
    [[nodiscard]] static f32 crt_powf(f32 x, f32 y);
    [[nodiscard]] static f32 crt_fabsf(f32 x);
    [[nodiscard]] static f32 crt_floorf(f32 x);
    [[nodiscard]] static f32 crt_ceilf(f32 x);

    /// `__setusermatherr` — previous handler (0 = default / none).
    GuestAddr exchange_user_matherr(GuestAddr handler) noexcept {
        return std::exchange(m_user_matherr, handler);
    }
    [[nodiscard]] GuestAddr user_matherr() const noexcept { return m_user_matherr; }

    /// Guest-side `struct _exception` scratch (40 bytes) for `_matherr`.
    [[nodiscard]] GuestAddr matherr_scratch(CrtMemoryManager& mem);

    /// Cached guest pointer to a permanent NUL-terminated function name.
    /// The raise helper fills the bytes on first insert via Marshaler.
    std::unordered_map<std::string, GuestAddr>& matherr_names() noexcept {
        return m_matherr_names;
    }

private:
    GuestAddr m_user_matherr = 0;
    GuestAddr m_matherr_scratch = 0;
    std::unordered_map<std::string, GuestAddr> m_matherr_names;
};

DllExports create_crt_math_thunks(CrtMathManager& mgr, CrtMemoryManager& mem);

} // namespace vitreo::winapi
