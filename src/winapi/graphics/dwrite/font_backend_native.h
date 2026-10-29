// SPDX-License-Identifier: LGPL-2.1-only
// CoreText-typed entry points of font_backend.mm for the other Objective-C++
// DirectWrite sources (text_layout_engine.mm). Not for C++ callers.
#pragma once

#include "winapi/graphics/dwrite/font_backend.hpp"

#include <CoreText/CoreText.h>

namespace vitreo::winapi::dwrite {

/// The face @p font describes, at 1000 points; the caller releases it.
[[nodiscard]] CTFontRef create_native_font(const MatchedFont& font);
/// A CoreText face as DirectWrite describes it (names, traits, metrics).
[[nodiscard]] MatchedFont describe_ct_font(CTFontRef font);

}  // namespace vitreo::winapi::dwrite
