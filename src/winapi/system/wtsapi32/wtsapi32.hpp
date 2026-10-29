#pragma once

/// @file wtsapi32.hpp
/// @brief Remote Desktop Services session-information contracts.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class EnvironmentManager;
class GlobalLocalManager;

/// Build wtsapi32 exports backed by the current Vitreo process session.
[[nodiscard]] DllExports create_wtsapi32_thunks(
    GlobalLocalManager& allocations,
    const EnvironmentManager& environment);

} // namespace vitreo::winapi
