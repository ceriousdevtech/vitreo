// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ncrypt.hpp
/// @brief NCrypt.dll — CNG key storage stubs (default-fail).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Build the NCrypt.dll thunk table.
DllExports create_ncrypt_thunks();

} // namespace vitreo::winapi
