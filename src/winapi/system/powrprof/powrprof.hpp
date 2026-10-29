#pragma once

/// @file powrprof.hpp
/// @brief POWRPROF.DLL power-information contracts.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class SysInfoManager;

/// Build the POWRPROF export table.
///
/// The system-information manager is the source of the guest-visible logical
/// processor count so ProcessorInformation agrees with GetSystemInfo.
DllExports create_powrprof_thunks(const SysInfoManager& sysinfo);

} // namespace vitreo::winapi
