// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ktmw32.hpp
/// @brief KtmW32.dll — Kernel Transaction Manager stubs.
///
/// Vitreo has no real KTM. We return a pseudo-handle from CreateTransaction
/// and pretend Commit/Rollback succeed so that callers (e.g. installers
/// using MoveFileTransactedW / DeleteFileTransactedW) take the success
/// path. The companion *Transacted file APIs in kernel32 already perform
/// the non-transactional fallback, so the net effect is "transaction
/// silently degrades to direct operation".

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Build the KtmW32.dll thunk table.
DllExports create_ktmw32_thunks();

} // namespace vitreo::winapi
