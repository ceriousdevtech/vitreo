// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file crt_stdio_buffer.hpp
/// @brief Guest-visible 32-bit MSVCRT FILE buffering cells.

#include <vitreo/types.hpp>

namespace vitreo::winapi::crt_stdio_buffer {

enum class Layout : u8 {
    LegacyIobuf32,
    UcrtOpaque32,
};

struct PublicCells {
    GuestAddr ptr;
    GuestAddr count;
    GuestAddr base;
    GuestAddr flag;
    GuestAddr file;
    GuestAddr buffer_size;
};

// Legacy MSVCRT exposes struct _iobuf directly. UCRT deliberately makes FILE
// opaque, but _get_stream_buffer_pointers exposes its three buffering cells;
// Vitreo's existing UCRT data block places those at 0x00/0x08/0x10.
constexpr PublicCells cells(Layout layout) {
    return layout == Layout::UcrtOpaque32
        ? PublicCells{0x00, 0x08, 0x10, 0, 0, 0}
        : PublicCells{0x00, 0x04, 0x08, 0x0C, 0x10, 0x18};
}

// Named legacy offsets remain useful for the legacy guest emitter and its
// direct-layout tests.
constexpr GuestAddr kPtr = cells(Layout::LegacyIobuf32).ptr;
constexpr GuestAddr kCount = cells(Layout::LegacyIobuf32).count;
constexpr GuestAddr kBase = cells(Layout::LegacyIobuf32).base;
constexpr GuestAddr kFlag = cells(Layout::LegacyIobuf32).flag;
constexpr GuestAddr kFile = cells(Layout::LegacyIobuf32).file;
constexpr GuestAddr kBufSize = cells(Layout::LegacyIobuf32).buffer_size;

constexpr GuestAddr kLock     = 0x20;
constexpr GuestAddr kMagic    = 0x24;
constexpr GuestAddr kHasRead  = 0x28;
constexpr GuestAddr kPushback = 0x2C;
constexpr GuestAddr kEof      = 0x30;
constexpr GuestAddr kCapacity = 0x34;

constexpr u32 kMagicValue = 0x4C494656u; // "VFIL" in little endian
constexpr usize kDefaultBufferBytes = 64 * 1024;

} // namespace vitreo::winapi::crt_stdio_buffer
