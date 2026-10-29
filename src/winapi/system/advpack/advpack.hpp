// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file advpack.hpp
/// @brief advpack.dll — Advanced INF Package Installer.
///
/// Exports names and ordinals from the real advpack.dll export table (84
/// names, identical in System32 and SysWOW64). Implemented: IsNTAdmin. The
/// INF-engine half (LaunchINFSection*, RunSetupCommand*, RegInstall*,
/// ExtractFiles*, AdvInstallFile*, TranslateInfString*, DelNode*, ...) sits on
/// a SetupAPI INF engine Vitreo does not have yet; those names stay
/// unexported, so GetProcAddress fails honestly and a static import resolves
/// to a loud missing-export stub. This header is the record of that gap.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class GuestElevationState;

/// Build the advpack.dll thunk table. `elevation` is the process token state
/// shared with shell32!IsUserAnAdmin and the advapi32 token APIs.
DllExports create_advpack_thunks(const GuestElevationState* elevation);

} // namespace vitreo::winapi
