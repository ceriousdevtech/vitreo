#pragma once

/// @file guest_sentinels.hpp
/// @brief Synthetic guest return addresses shared by the CLI execution loop and
///        the WinAPI thread layer.
///
/// These live here rather than next to the execution loop because
/// `ExitThread`/`FreeLibraryAndExitThread` in `src/winapi/kernel32` must produce
/// exactly the values the loop recognises, and the WinAPI layer cannot include
/// CLI headers. Keeping one definition removes the chance of the two drifting.

#include "vitreo/types.hpp"

namespace vitreo {

/// Return address seeded on the process entry frame. A normal return from the
/// guest entrypoint lands here and terminates the process.
inline constexpr u64 kProcessReturnSentinel = 0xDEAD'C0DE'FFFF'0000ULL;

/// Return address pushed for nested and thread guest callbacks. When guest RIP
/// reaches this value the callback loop unwinds, and `ExitThread` redirects here
/// so noreturn call sites never fall into the `int3` padding MSVC emits after
/// them.
inline constexpr u64 kCallbackReturnSentinel = 0xDEAD'C0DE'DEAD'C0DEULL;

/// A 32-bit guest only ever observes the low dword of a sentinel, and a zero
/// return address is indistinguishable from a NULL call target to guest SEH and
/// stack walkers — it faults instead of unwinding.
static_assert(static_cast<u32>(kProcessReturnSentinel) != 0,
              "low dword of the process sentinel must not look like NULL");
static_assert(static_cast<u32>(kCallbackReturnSentinel) != 0,
              "low dword of the callback sentinel must not look like NULL");

/// The process sentinel as a guest of the given pointer width observes it.
[[nodiscard]] constexpr u64 process_return_sentinel(bool is_64bit) noexcept {
    return is_64bit ? kProcessReturnSentinel
                    : static_cast<u32>(kProcessReturnSentinel);
}

/// The callback sentinel as a guest of the given pointer width observes it.
[[nodiscard]] constexpr u64 callback_return_sentinel(bool is_64bit) noexcept {
    return is_64bit ? kCallbackReturnSentinel
                    : static_cast<u32>(kCallbackReturnSentinel);
}

} // namespace vitreo
