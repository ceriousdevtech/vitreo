// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_memo_lock.hpp
/// @brief Reader/writer lock for one compile-memo shard.
///
/// The spin-then-sleep lock in core/sync/spin_rwlock.hpp; it lives there so
/// GuestMemory's sharded JIT code-page table can use the same lock without
/// depending on the JIT.

#include <core/sync/spin_rwlock.hpp>

namespace vitreo::cpu::jit {

using JitMemoLock = vitreo::SpinRwLock;

}  // namespace vitreo::cpu::jit
