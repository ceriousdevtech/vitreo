// SPDX-License-Identifier: LGPL-2.1-only
/// @file synthetic_handle_registry.hpp
/// @brief Tracks FACE/CE/CC handles issued outside HandleTable so CloseHandle /
/// SetHandleInformation can reject never-issued values (Windows:
/// synthetic_handle_probe.expected).

#pragma once

#include "winapi/kernel32/handle.hpp"

#include <mutex>
#include <unordered_map>

namespace vitreo::winapi::synthetic_handles {

[[nodiscard]] inline bool is_synthetic_range(Handle h) noexcept {
    return (h & 0xFFFF0000u) == 0xFACE0000u ||
           (h & 0xFF000000u) == 0xCE000000u ||
           (h & 0xFF000000u) == 0xCC000000u;
}

/// Record that @p h was issued. Flags start at 0.
void note_issued(Handle h);

/// Record a USER object handle (desktop, window station) that lives outside
/// HandleTable and outside the synthetic ranges, with its initial
/// HANDLE_FLAG_INHERIT / PROTECT_FROM_CLOSE bits, so Get/SetHandleInformation
/// answer for it as Windows does (s4_desktop_handle_probe). @p fresh = true
/// for a newly issued handle (its flags replace any stale entry); false to
/// register a long-lived handle without resetting flags the guest already
/// changed. CloseHandle does not close these: Windows rejects CloseHandle on
/// a desktop handle with ERROR_INVALID_HANDLE.
void note_user_object(Handle h, u32 flags, bool fresh);

/// True if @p h is currently live in a synthetic namespace.
[[nodiscard]] bool is_live(Handle h);

/// Close a live synthetic handle. Returns true if it was live (and is now
/// gone); false if it was never issued or already closed.
[[nodiscard]] bool close(Handle h);

/// Apply HANDLE_FLAG_INHERIT / HANDLE_FLAG_PROTECT_FROM_CLOSE like HandleTable.
/// Returns false if @p h is not live.
[[nodiscard]] bool set_flags(Handle h, u32 mask, u32 flags);

/// Current Inherit/ProtectFromClose bits, or nullopt if not live.
[[nodiscard]] bool get_flags(Handle h, u32& out_flags);

} // namespace vitreo::winapi::synthetic_handles
