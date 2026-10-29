// SPDX-License-Identifier: LGPL-2.1-only

/// @file guest_com.cpp

#include "winapi/core/guest_com.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::winapi {

u64 call_guest_com_slot(GuestMemory& memory, bool is_64bit,
                        const GuestFunctionCaller& call,
                        GuestAddr interface_ptr, usize slot) {
    if (interface_ptr == 0 || !call) return 0;
    const usize pointer_size = is_64bit ? 8u : 4u;
    // A COM interface pointer points at its vtable pointer.
    GuestAddr vtable = 0;
    if (is_64bit) {
        const auto value = memory.read_u64(interface_ptr);
        if (!value.is_ok()) return 0;
        vtable = value.value();
    } else {
        const auto value = memory.read_u32(interface_ptr);
        if (!value.is_ok()) return 0;
        vtable = value.value();
    }
    if (vtable == 0) return 0;
    GuestAddr function = 0;
    if (is_64bit) {
        const auto value = memory.read_u64(vtable + slot * pointer_size);
        if (!value.is_ok()) return 0;
        function = value.value();
    } else {
        const auto value = memory.read_u32(vtable + slot * pointer_size);
        if (!value.is_ok()) return 0;
        function = value.value();
    }
    if (function == 0) return 0;
    // `this` is the only argument. stdcall on x86 and the first register
    // argument on x64 -- the injected caller owns that difference.
    const u64 args[1] = {static_cast<u64>(interface_ptr)};
    return call(function, args, 1);
}

} // namespace vitreo::winapi
