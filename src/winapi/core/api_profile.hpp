/// @file api_profile.hpp
/// @brief Arch-neutral per-export API profile (VITREO_API_PROFILE=1).
///
/// The interpreter/JIT loop, the JIT's in-chain thunk path and the native32
/// loop all dispatch guest imports through different code, so every existing
/// thunk measurement is per-arch and the two cannot be diffed. Instrumenting
/// call sites misses whichever path a given arch prefers -- the first cut of
/// this recorded exactly one call on ARM, because the JIT never reaches the
/// interpreter's dispatch check.
///
/// So measure by construction instead, at the two places every arch shares:
/// the registered handler (all slow calls) and the hot fast path (all fast
/// calls). That yields the same table on both machines and answers one
/// question: is the slower machine making *more* API calls, or the *same*
/// calls more slowly?
#pragma once

#include <vitreo/common.hpp>

namespace vitreo::winapi {

class ThunkRegistry;

namespace api_profile {

/// True when VITREO_API_PROFILE is set.
[[nodiscard]] bool enabled() noexcept;

/// Record one completed call. `key` must be the export's dispatch address so
/// the handler and fast-path sides agree on one key per export.
void record(GuestAddr key, u64 ns) noexcept;

/// Registry used to turn keys back into "dll!Export" at dump time.
void set_name_source(const ThunkRegistry* reg) noexcept;

/// Merge all threads' tables and print the top exports.
void dump(const char* tag);

} // namespace api_profile
} // namespace vitreo::winapi
