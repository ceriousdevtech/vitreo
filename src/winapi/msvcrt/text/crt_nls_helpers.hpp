// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_nls_helpers.hpp
/// @brief msvcrt's __crtCompareStringA / __crtLCMapStringA (see the .cpp).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class LocaleStringManager;

/// Register __crtCompareStringA and __crtLCMapStringA over kernel32's
/// CompareStringW / LCMapStringW logic.
DllExports create_crt_nls_helper_thunks(const LocaleStringManager& locales);

} // namespace vitreo::winapi
