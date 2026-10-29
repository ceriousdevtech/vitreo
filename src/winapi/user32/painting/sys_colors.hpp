// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file sys_colors.hpp
/// @brief The process's system colours: GetSysColor, SetSysColors, and the
///        COLOR_xxx + 1 class-brush convention all read this one table.
///
/// Defaults are a default Windows 11 install, measured on Windows
/// (tools/oracle_probe/sys_color_probe.expected), in the interactive session:
/// a service session without visual styles answers D77800 for
/// COLOR_MENUHILIGHT, a desktop session FF9933 (re-measured 2026-09-30). There used to be four
/// hand-typed copies, mostly Windows 2000 "classic" values (BTNFACE C0C0C0,
/// 3DSHADOW 808080) and one app-specific green APPWORKSPACE.

#include <vitreo/common.hpp>

#include <array>
#include <mutex>
#include <optional>

namespace vitreo::winapi::sys_colors {

/// Number of defined COLOR_xxx indices (COLOR_SCROLLBAR .. COLOR_MENUBAR).
inline constexpr u32 kCount = 31;

inline constexpr std::array<u32, kCount> kWindows11Defaults = {
    0xC8C8C8u,  //  0 COLOR_SCROLLBAR
    0x000000u,  //  1 COLOR_BACKGROUND
    0xD1B499u,  //  2 COLOR_ACTIVECAPTION
    0xDBCDBFu,  //  3 COLOR_INACTIVECAPTION
    0xF0F0F0u,  //  4 COLOR_MENU
    0xFFFFFFu,  //  5 COLOR_WINDOW
    0x646464u,  //  6 COLOR_WINDOWFRAME
    0x000000u,  //  7 COLOR_MENUTEXT
    0x000000u,  //  8 COLOR_WINDOWTEXT
    0x000000u,  //  9 COLOR_CAPTIONTEXT
    0xB4B4B4u,  // 10 COLOR_ACTIVEBORDER
    0xFCF7F4u,  // 11 COLOR_INACTIVEBORDER
    0xABABABu,  // 12 COLOR_APPWORKSPACE
    0xD77800u,  // 13 COLOR_HIGHLIGHT
    0xFFFFFFu,  // 14 COLOR_HIGHLIGHTTEXT
    0xF0F0F0u,  // 15 COLOR_BTNFACE
    0xA0A0A0u,  // 16 COLOR_BTNSHADOW
    0x6D6D6Du,  // 17 COLOR_GRAYTEXT
    0x000000u,  // 18 COLOR_BTNTEXT
    0x000000u,  // 19 COLOR_INACTIVECAPTIONTEXT
    0xFFFFFFu,  // 20 COLOR_BTNHIGHLIGHT
    0x696969u,  // 21 COLOR_3DDKSHADOW
    0xE3E3E3u,  // 22 COLOR_3DLIGHT
    0x000000u,  // 23 COLOR_INFOTEXT
    0xE1FFFFu,  // 24 COLOR_INFOBK
    0x000000u,  // 25 (unused)
    0xCC6600u,  // 26 COLOR_HOTLIGHT
    0xEAD1B9u,  // 27 COLOR_GRADIENTACTIVECAPTION
    0xF2E4D7u,  // 28 COLOR_GRADIENTINACTIVECAPTION
    0xFF9933u,  // 29 COLOR_MENUHILIGHT (interactive session; D77800 without visual styles)
    0xF0F0F0u,  // 30 COLOR_MENUBAR
};

namespace detail {
struct State {
    std::mutex mutex;
    std::array<std::optional<u32>, kCount> overrides{};
};
inline State& state() {
    static State s;
    return s;
}
}  // namespace detail

/// GetSysColor(index): SetSysColors overrides first; 0 for unknown indices.
[[nodiscard]] inline u32 get(u32 index) {
    if (index >= kCount) return 0;
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.overrides[index].value_or(kWindows11Defaults[index]);
}

/// SetSysColors for one element.
inline void set(u32 index, u32 colorref) {
    if (index >= kCount) return;
    auto& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.overrides[index] = colorref & 0xFFFFFFu;
}

/// Vitreo's GetSysColorBrush handles: kSysBrushBase | index.
inline constexpr u64 kSysBrushBase = 0x10000000u;

/// The colour a brush stands for when it is one of the two system-colour
/// forms: COLOR_xxx + 1 (Learn WNDCLASS.hbrBackground, FillRect) or a
/// GetSysColorBrush handle. nullopt for any other value. Callers check real
/// GDI brushes first: small GDI handles overlap the COLOR_xxx + 1 range.
[[nodiscard]] inline std::optional<u32> from_brush(u64 brush) {
    if (brush != 0 && brush <= kCount) return get(static_cast<u32>(brush - 1));
    if ((brush & ~u64{0xFF}) == kSysBrushBase && (brush & 0xFF) < kCount) {
        return get(static_cast<u32>(brush & 0xFF));
    }
    return std::nullopt;
}

}  // namespace vitreo::winapi::sys_colors
