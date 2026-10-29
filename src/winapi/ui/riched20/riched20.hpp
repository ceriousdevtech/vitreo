#pragma once
/// @file riched20.hpp
/// @brief Minimal riched20.dll / RichEdit20 class bring-up (Windows DLL)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Register a thunk module so LoadLibrary("RichEd20.dll") succeeds.
/// Window-class registration is handled via builtin_control_ordinal + Cocoa
/// mapping (same pattern as comctl32 common controls); synthetic modules do
/// not run DllMain.
[[nodiscard]] DllExports create_riched20_thunks();

} // namespace vitreo::winapi
