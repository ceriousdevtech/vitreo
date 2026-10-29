#pragma once

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// TAPI 2.x line APIs exported by Tapi32.dll. There is no telephony
/// hardware; functions fail with the documented LINEERR_* codes.
DllExports create_tapi32_thunks();

} // namespace vitreo::winapi
