#pragma once

/// @file xinput.hpp
/// @brief xinput1_4.dll / xinput1_3.dll / xinput9_1_0.dll entry points.
///
/// Windows ships these as system DLLs. With no attached controller, every
/// device query returns ERROR_DEVICE_NOT_CONNECTED — the documented miss
/// result, not a fake pad.

#include "winapi/core/thunk_registry.hpp"

#include <memory>

namespace vitreo::hardware {
class GamepadHost;
}

namespace vitreo::winapi {

/// The three XInput DLLs export different subsets under different ordinals
/// (xinput9_1_0 ordinal 2 is XInputGetCapabilities; xinput1_3/1_4 ordinal 2
/// is XInputGetState), so each gets its own native export table.
enum class XInputDll { V1_4, V1_3, V9_1_0 };

DllExports create_xinput_thunks(
    XInputDll dll, std::shared_ptr<hardware::GamepadHost> host = nullptr);

} // namespace vitreo::winapi
