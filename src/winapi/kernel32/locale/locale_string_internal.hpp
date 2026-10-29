#pragma once
/// @file locale_string_internal.hpp
/// @brief Shared surface between locale_string.cpp and its export table.
///
/// Private to kernel32's locale implementation. Four constants, because four
/// constants are what both halves need.

#include <vitreo/common.hpp>

namespace vitreo::winapi {

// CompareString does not return the -1/0/+1 a C programmer expects. Its three
// results are 1, 2 and 3, and 0 means the call failed — so a caller that treats
// the return as a sign reads "less than" for every successful comparison.
inline constexpr i32 CSTR_LESS_THAN    = 1;
inline constexpr i32 CSTR_EQUAL        = 2;
inline constexpr i32 CSTR_GREATER_THAN = 3;

// CompareString(Ex) dwCmpFlags — Learn stringapiset / winnls.
inline constexpr u32 NORM_IGNORECASE            = 0x00000001;
inline constexpr u32 NORM_IGNORENONSPACE         = 0x00000002;
inline constexpr u32 NORM_IGNORESYMBOLS          = 0x00000004;
inline constexpr u32 SORT_DIGITSASNUMBERS        = 0x00000008;
inline constexpr u32 LINGUISTIC_IGNORECASE       = 0x00000010;
inline constexpr u32 LINGUISTIC_IGNOREDIACRITIC  = 0x00000020;
inline constexpr u32 SORT_STRINGSORT             = 0x00001000;
inline constexpr u32 NORM_IGNOREKANATYPE         = 0x00010000;
inline constexpr u32 NORM_IGNOREWIDTH            = 0x00020000;
inline constexpr u32 NORM_LINGUISTIC_CASING      = 0x08000000;

inline constexpr u32 kCompareStringValidFlags =
    NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS |
    SORT_DIGITSASNUMBERS | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC |
    SORT_STRINGSORT | NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH |
    NORM_LINGUISTIC_CASING;

class LocaleStringManager;
class Marshaler;

// GetThreadUILanguage. The first LANGID from the thread preferred UI list,
// or the user-default UI language when the thread has not set one.
[[nodiscard]] u32 thread_ui_language(const LocaleStringManager& mgr);

// SetThreadPreferredUILanguages. FALSE means the call failed; LastError is
// already set (ERROR_INVALID_PARAMETER or ERROR_NOACCESS).
[[nodiscard]] bool set_thread_preferred_ui_languages(
    const LocaleStringManager& mgr, Marshaler& m, u32 flags,
    GuestAddr languages, GuestAddr count_ptr);

// GetThreadPreferredUILanguages. FALSE means the call failed; LastError is
// already set.
[[nodiscard]] bool get_thread_preferred_ui_languages(
    const LocaleStringManager& mgr, Marshaler& m, u32 flags,
    GuestAddr count_ptr, GuestAddr buffer, GuestAddr cch_ptr);

} // namespace vitreo::winapi
