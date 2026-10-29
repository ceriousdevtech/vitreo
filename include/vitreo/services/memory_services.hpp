// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: memory management

#pragma once

#include <vitreo/types.hpp>

namespace vitreo {
class GuestMemory;
} // namespace vitreo

namespace vitreo::winapi {
class VirtualMemoryManager;
class HeapManager;
class GlobalLocalManager;
class HeapExtended;
} // namespace vitreo::winapi

namespace vitreo {

struct MemoryServices {
    static constexpr u32 kServiceId = 1;

    GuestMemory&                  guest_memory;
    winapi::VirtualMemoryManager& vmm;
    winapi::HeapManager&          heap_mgr;
    winapi::GlobalLocalManager&   gl_mgr;
    winapi::HeapExtended&         heap_ext;
};

} // namespace vitreo
