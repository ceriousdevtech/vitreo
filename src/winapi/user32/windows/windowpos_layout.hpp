// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "core/memory/guest_memory.hpp"
namespace vitreo::winapi {
// Microsoft WINDOWPOS contains two pointer-sized HWNDs, then five 32-bit
// fields: 28 bytes on x86 and 40 bytes (including tail padding) on x64.
// https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-windowpos
struct WindowPosLayout {
    bool wide;
    constexpr u32 scalar_offset() const { return wide ? 16u : 8u; }
    void write(GuestMemory& memory, GuestAddr p, u32 hwnd, u32 after,
               i32 x, i32 y, i32 cx, i32 cy, u32 flags) const {
        if (wide) {
            (void)memory.write_u64(p, hwnd);
            const u64 insert = after >= 0xfffffffeu
                ? static_cast<u64>(static_cast<i64>(static_cast<i32>(after))) : after;
            (void)memory.write_u64(p+8, insert);
            (void)memory.write_u32(p+36, 0);
        } else {
            (void)memory.write_u32(p, hwnd);
            (void)memory.write_u32(p+4, after);
        }
        const auto base = p + scalar_offset();
        (void)memory.write_u32(base, static_cast<u32>(x));
        (void)memory.write_u32(base+4, static_cast<u32>(y));
        (void)memory.write_u32(base+8, static_cast<u32>(cx));
        (void)memory.write_u32(base+12, static_cast<u32>(cy));
        (void)memory.write_u32(base+16, flags);
    }
};
}
