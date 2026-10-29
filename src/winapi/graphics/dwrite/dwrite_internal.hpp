#pragma once
/// @file dwrite_internal.hpp
/// @brief Shared surface between the dwrite*.cpp files.
///
/// Private to the DirectWrite implementation. These helpers were file-local
/// (anonymous namespace) to dwrite.cpp before it was split by interface.
/// They live in a named namespace so that sharing them across translation
/// units cannot collide with a same-named symbol elsewhere in
/// vitreo::winapi; the using-directive below keeps unqualified use as it was.

#include "winapi/graphics/dwrite/dwrite.hpp"

#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <vitreo/common.hpp>

namespace vitreo::winapi::dwrite_internal {

// A multi-process guest spreads font work over its browser, GPU, and
// renderer processes, which all log here, so the pid is what makes the
// trace readable.
void dwrite_diag_printf(const char* format, ...)
    __attribute__((format(printf, 1, 2)));
bool dwrite_diag_enabled();

inline constexpr u32 S_OK = 0;
inline constexpr u32 E_NOINTERFACE = 0x80004002u;
inline constexpr u32 E_POINTER = 0x80004003u;
inline constexpr u32 E_INVALIDARG = 0x80070057u;
inline constexpr u32 E_OUTOFMEMORY = 0x8007000eu;
inline constexpr u32 E_NOT_SUFFICIENT_BUFFER = 0x8007007au;
inline constexpr u32 E_NOTIMPL = 0x80004001u;
inline constexpr u32 E_FAIL = 0x80004005u;
inline constexpr u32 DWRITE_E_FILEFORMAT = 0x88985000u;
inline constexpr u32 DWRITE_E_NOFONT = 0x88985002u;  // winerror.h, SDK 10.0.26100.0
inline constexpr u32 DWRITE_E_ALREADYREGISTERED = 0x88985006u;

inline constexpr usize MAX_FALLBACK_TEXT_UNITS = 0x10000;
inline constexpr usize MAX_CUSTOM_FONT_FILE_SIZE = 256 * 1024 * 1024;

// DWRITE_FONT_SIMULATIONS and DWRITE_FONT_STYLE (dwrite.h).
inline constexpr u32 DWRITE_FONT_SIMULATIONS_BOLD = 1;
inline constexpr u32 DWRITE_FONT_SIMULATIONS_OBLIQUE = 2;
inline constexpr u32 DWRITE_FONT_STYLE_OBLIQUE = 1;

inline float float_from_bits(u32 bits) {
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline float read_float_argument(Marshaler& m, usize index) {
    if (m.cpu_is_64bit() && index < 4 && m.has_cpu()) {
        return float_from_bits(m.cpu()->sse.xmm[index].u32_[0]);
    }
    return float_from_bits(static_cast<u32>(m.arg(index)));
}

inline char16_t fold_ascii(char16_t ch) {
    return ch >= u'A' && ch <= u'Z'
        ? static_cast<char16_t>(ch + (u'a' - u'A')) : ch;
}

inline std::u16string fold_ascii(std::u16string_view text) {
    std::u16string folded(text);
    for (auto& ch : folded) ch = fold_ascii(ch);
    return folded;
}

inline u32 reported_weight(const dwrite::MatchedFont& font, u32 simulations) {
    // dwrite_gdi_interop_probe: an emboldened Light, Regular, Semibold or
    // Bold face all report DWRITE_FONT_WEIGHT_BOLD.
    return (simulations & DWRITE_FONT_SIMULATIONS_BOLD) != 0
        ? std::max<u32>(font.weight, 700u) : font.weight;
}

inline u32 reported_style(const dwrite::MatchedFont& font, u32 simulations) {
    return (simulations & DWRITE_FONT_SIMULATIONS_OBLIQUE) != 0
        ? DWRITE_FONT_STYLE_OBLIQUE : font.style;
}

}  // namespace vitreo::winapi::dwrite_internal

namespace vitreo::winapi {
using namespace dwrite_internal;
}  // namespace vitreo::winapi
