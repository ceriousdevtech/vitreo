#pragma once

/// @file dinput8.hpp
/// @brief DINPUT8.DLL entry-point contracts.

#include "winapi/media/dinput/dinput.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Build the DirectInput 8 export table.
///
/// `DirectInput8Create` hands out an `IDirectInput8A` that shares the system
/// keyboard/mouse backend with legacy `dinput.dll`.
DllExports create_dinput8_thunks(DirectInputManager& manager);

} // namespace vitreo::winapi
