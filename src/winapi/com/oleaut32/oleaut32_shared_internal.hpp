#pragma once
/// @file oleaut32_shared_internal.hpp
/// @brief File-level constants and the first-call logger shared by the
///        oleaut32 translation units.
///
/// Private to the oleaut32 implementation. These were file-local in
/// oleaut32.cpp before it was split by responsibility. Kept apart from
/// oleaut32_internal.hpp, which the type-library code includes and which
/// declares its own file-local names.

#include <vitreo/common.hpp>

#include <cstdio>
#include <string>
#include <unordered_map>

namespace vitreo::winapi {

static constexpr u16 VT_BYREF  = 0x4000;
static constexpr u16 VT_TYPEMASK = 0x0FFF;

inline constexpr u16 kVtRecord = 36;

/// Largest byte length oleaut32 will allocate. SysAllocStringLen(NULL,
/// 0x7FFFFFFF) returns NULL on both bitnesses (oleaut32_safearray_probe
/// bstr_huge), so the length prefix is kept within a positive 32-bit size.
inline constexpr u64 kBstrMaxBytes = 0x7FFFFFF0ull;

// Log only the first call of each ordinal export (confirms invocation without
// flooding the log during a busy init).
inline void oa_log_first(const char* what) {
    static std::unordered_map<std::string, bool>* seen = new std::unordered_map<std::string, bool>();
    if ((*seen)[what]) return;
    (*seen)[what] = true;
    std::fprintf(stderr, "[oleaut32] first call: %s\n", what);
}

} // namespace vitreo::winapi
