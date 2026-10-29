#pragma once
/// @file crt_string_internal.hpp
/// @brief Shared surface between crt_string.cpp and crt_string_registration.cpp.
///
/// Private to msvcrt's string implementation. These were file-local before the
/// export table was split out; this header exists so the two halves can still
/// see each other, not to publish them.
///
/// All five read or write strings that live in guest memory, which is why they
/// take a GuestMemory rather than a pointer: a guest string may straddle a page
/// boundary the host cannot address contiguously, so every one of them is
/// capped and falls back to byte-at-a-time reads when host_ptr declines.

#include "winapi/core/marshaler.hpp"

#include <vitreo/common.hpp>

#include <functional>
#include <string>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// Cap on any single bulk guest string read. A guest may hand us an unbounded
/// or unterminated pointer; every walk below stops here rather than trusting it.
inline constexpr usize MAX_BULK_SIZE = 65536;

/// No bound: strlen/wcslen and friends measure the whole string.
inline constexpr usize UNBOUNDED_STRING = static_cast<usize>(-1) / 4;

/// Length of a null-terminated ANSI string in guest memory. Unbounded by
/// default (Learn strlen); stops early only at an unreadable byte.
[[nodiscard]] usize guest_strlen(GuestMemory& mem, GuestAddr addr,
                                 usize cap = UNBOUNDED_STRING);

/// Length of a null-terminated UTF-16 string in guest memory (units).
[[nodiscard]] usize guest_wcslen(GuestMemory& mem, GuestAddr addr,
                                 usize cap = UNBOUNDED_STRING);

/// Read `len` UTF-16 code units out of guest memory.
[[nodiscard]] std::wstring guest_read_wstr(GuestMemory& mem, GuestAddr addr,
                                           usize len);

} // namespace vitreo::winapi
