#pragma once
/// @file oleaut32_internal.hpp
/// @brief Constants shared between oleaut32.cpp and oleaut32_typelib.cpp.
///
/// Private to the oleaut32 implementation. These were file-local before the
/// type-library code was split out.

#include <vitreo/common.hpp>

namespace vitreo::winapi {

// Windows HRESULT constants.
//
// An HRESULT is not an error number: bit 31 is the severity flag, so a caller
// tests success with SUCCEEDED(hr) rather than == 0. S_OK being zero is what
// makes that work.
inline constexpr u32 S_OK                = 0x00000000;
inline constexpr u32 E_NOTIMPL           = 0x80004001;
inline constexpr u32 E_FAIL              = 0x80004005;
inline constexpr u32 DISP_E_TYPEMISMATCH = 0x80020005;

// VARTYPE constants.
//
// The tag in a VARIANT saying what its union member holds. Automation callers
// switch on it, so the numbering is part of the ABI rather than an internal
// choice.
inline constexpr u16 VT_EMPTY  = 0;
inline constexpr u16 VT_NULL   = 1;
inline constexpr u16 VT_I2     = 2;
inline constexpr u16 VT_I4     = 3;
inline constexpr u16 VT_R4     = 4;
inline constexpr u16 VT_R8     = 5;
inline constexpr u16 VT_BSTR   = 8;
inline constexpr u16 VT_DISPATCH = 9;
inline constexpr u16 VT_BOOL   = 11;
inline constexpr u16 VT_UNKNOWN = 13;
inline constexpr u16 VT_I1     = 16;
inline constexpr u16 VT_UI1    = 17;
inline constexpr u16 VT_UI2    = 18;
inline constexpr u16 VT_UI4    = 19;
inline constexpr u16 VT_VARIANT = 12;

// oaidl.h tagVARIANT: 8-byte header + union. The union is 8 bytes on x86
// (sizeof VARIANT == 16) and 16 bytes on x64 because pvRecord + pRecInfo are
// two pointers (sizeof VARIANT == 24). Payload always starts at offset 8.
[[nodiscard]] inline u32 oleaut_variant_size(bool is_64bit) noexcept {
    return is_64bit ? 24u : 16u;
}

[[nodiscard]] inline u32 oleaut_ptr_size(bool is_64bit) noexcept {
    return is_64bit ? 8u : 4u;
}

// oaidl.h tagSAFEARRAY: after cLocks (ULONG at +8), pvData is PVOID and
// therefore pointer-aligned. Bounds follow pvData.
//   x86: pvData @12, rgsabound @16
//   x64: 4-byte pad @12, pvData @16, rgsabound @24
[[nodiscard]] inline u32 oleaut_sa_pvdata_off(bool is_64bit) noexcept {
    return is_64bit ? 16u : 12u;
}

[[nodiscard]] inline u32 oleaut_sa_bounds_off(bool is_64bit) noexcept {
    return is_64bit ? 24u : 16u;
}

} // namespace vitreo::winapi
