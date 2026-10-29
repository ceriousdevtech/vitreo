// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Apply the native msvcrt.dll export surface and ordinals for the guest ABI.
/// Names absent from that PE are removed, including UCRT-only exports.
void apply_msvcrt_windows_ordinals(DllExports& exports, bool guest_is_64bit);

/// Apply the native ucrtbase.dll export surface and ordinals for the guest ABI.
void apply_ucrtbase_windows_ordinals(DllExports& exports, bool guest_is_64bit);

}  // namespace vitreo::winapi
