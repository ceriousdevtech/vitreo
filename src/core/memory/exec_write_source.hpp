// SPDX-License-Identifier: LGPL-2.1-only

/// @file exec_write_source.hpp
/// @brief Diagnostic tag naming why GuestMemory::notify_executable_write runs.
///
/// Only read by VITREO_JIT_INVAL_STATS. Callers that notify for a reason other
/// than a guest store (protection change, I-cache flush, decommit) scope a tag
/// around the call; untagged notifications are classified by size.

#pragma once

#include "vitreo/types.hpp"

namespace vitreo::exec_write_source {

enum Source : u32 {
    kStore,     // untagged, at most 16 bytes
    kBulk,      // untagged, larger
    kProtect,   // VirtualProtect to an executable protection
    kFlush,     // FlushInstructionCache
    kDecommit,  // VirtualFree
    kOther,     // other tagged callers
    kCount
};

inline thread_local Source t_source = kStore;
inline thread_local bool t_tagged = false;

struct Scope {
    Source saved;
    bool saved_tagged;
    explicit Scope(Source s) noexcept : saved(t_source), saved_tagged(t_tagged) {
        t_source = s;
        t_tagged = true;
    }
    ~Scope() {
        t_source = saved;
        t_tagged = saved_tagged;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

[[nodiscard]] inline Source classify(usize size) noexcept {
    if (t_tagged) return t_source;
    return size <= 16 ? kStore : kBulk;
}

}  // namespace vitreo::exec_write_source
