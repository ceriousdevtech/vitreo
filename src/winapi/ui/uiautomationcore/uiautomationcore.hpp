#pragma once

/// @file uiautomationcore.hpp
/// @brief UIAutomationCore.dll — UI Automation provider-side entry points.
///
/// Vitreo hosts no UI Automation clients (no screen reader, no assistive tech,
/// no automation harness attached), so the honest answer to "is anyone
/// listening?" is *no*. Every export here reports that truthfully rather than
/// pretending a provider exists.
///
/// These previously resolved to the generic missing-export stub with
/// `stdcall_bytes = 0`. Because they are `WINAPI`, every call leaked the
/// caller's arguments. A setup UI raises automation events while building
/// its window, and the accumulated leak shifts the stack until a later
/// `ret` branches to garbage (`InvalidOpcode at EIP=0x00004e25`).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Create UIAutomationCore.dll export thunks.
DllExports create_uiautomationcore_thunks();

} // namespace vitreo::winapi
