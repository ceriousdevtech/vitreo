// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {
/// ADSI provider error storage; directory-service binding is explicitly unsupported.
DllExports create_activeds_thunks();
}
