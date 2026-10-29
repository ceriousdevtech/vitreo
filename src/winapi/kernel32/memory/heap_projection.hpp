// Generated from Windows x86/x64 heap_identity_probe.expected; do not edit.
#pragma once
#include <vitreo/common.hpp>
namespace vitreo::winapi::heap_projection {
struct Layout {u32 flags;u32 force_flags;u32 peb_process_heap;u32 process_flags;u32 private_class;u32 force_mask;};
inline constexpr Layout layout32={0x40u, 0x44u, 0x18u, 0x2u, 0x1000u, 0x5u};
inline constexpr Layout layout64={0x70u, 0x74u, 0x30u, 0x2u, 0x1000u, 0x5u};
}
