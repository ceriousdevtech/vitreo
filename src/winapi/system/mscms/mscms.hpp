// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "winapi/core/thunk_registry.hpp"
namespace vitreo::capsule { class Vfs; }
namespace vitreo::winapi {
/// Process-owned ICC profiles/transforms; all profile paths remain in the VFS.
DllExports create_mscms_thunks(capsule::Vfs* vfs = nullptr);
}
