#pragma once

/// @file oleacc.hpp
/// @brief Microsoft Active Accessibility entry points.

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::windowing {
class HwndManager;
}

namespace vitreo::winapi {

/// Create the oleacc.dll exports currently supported by Vitreo.
///
/// Provider/client COM object behavior that is not implemented fails with a
/// documented HRESULT. No synthetic IAccessible object is fabricated.
[[nodiscard]] DllExports create_oleacc_thunks(
    windowing::HwndManager& hwnd_manager);

} // namespace vitreo::winapi
