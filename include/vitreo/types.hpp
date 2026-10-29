#pragma once

/// @file types.hpp
/// @brief Fundamental type definitions for Vitreo
///
/// This header provides consistent type aliases used throughout the Vitreo
/// codebase. All code should use these types rather than raw C++ types for
/// consistency and clarity.

#include <cstddef>
#include <cstdint>

namespace vitreo {

// =============================================================================
// Sized Integer Types
// =============================================================================

/// Unsigned integers
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Signed integers
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

/// Floating point
using f32 = float;
using f64 = double;

// =============================================================================
// Size Types
// =============================================================================

/// Size type for memory/array sizes (unsigned)
using usize = std::size_t;

/// Size type for pointer differences (signed)
using isize = std::ptrdiff_t;

// =============================================================================
// Guest Address Types
// =============================================================================

/// 32-bit guest virtual address (for PE32 executables)
using GuestAddr32 = u32;

/// 64-bit guest virtual address (for PE32+ executables)
using GuestAddr64 = u64;

/// Default guest address type (64-bit)
/// Use GuestAddr32 explicitly when dealing with 32-bit PEs
using GuestAddr = u64;

/// Guest pointer represented in host memory
/// This is the host address where guest memory is mapped
using HostPtr = void*;
using HostPtrConst = const void*;

// =============================================================================
// Windows-compatible Types
// =============================================================================

/// Windows HANDLE equivalent (opaque pointer-sized value)
using WinHandle = u64;

/// Windows DWORD equivalent
using WinDword = u32;

/// Windows WORD equivalent
using WinWord = u16;

/// Windows BYTE equivalent
using WinByte = u8;

/// Windows BOOL equivalent (non-zero = true)
using WinBool = i32;

/// Invalid handle sentinel value
constexpr WinHandle INVALID_WIN_HANDLE = static_cast<WinHandle>(-1);

// =============================================================================
// Byte Array Type
// =============================================================================

/// Single byte type for raw memory operations
using Byte = u8;

// =============================================================================
// Compile-time Checks
// =============================================================================

static_assert(sizeof(u8)  == 1, "u8 must be 1 byte");
static_assert(sizeof(u16) == 2, "u16 must be 2 bytes");
static_assert(sizeof(u32) == 4, "u32 must be 4 bytes");
static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");

} // namespace vitreo
