// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_stdio_state.cpp
/// @brief The stdio lock domain and the per-thread arena pointer.
///
/// The single definitions of the table lock, its per-thread depth, the
/// current-arena pointer and the descriptor locks; crt_stdio_internal.hpp
/// declares them for every translation unit of the stdio export table.

#include "winapi/msvcrt/io/crt_stdio_internal.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

namespace vitreo::winapi::crt_stdio_internal {

std::recursive_mutex s_stdio_state_mutex;
constinit thread_local unsigned s_stdio_lock_depth = 0;
constinit thread_local StdioArena* current_stdio_arena = nullptr;

#include "crt_lowio_limits.inc"
i32 lowio_limit() {
    return CrtDllFlavorScope::current() == CrtDllFlavor::Msvcrt
        ? kLegacyLowioLimit : kUcrtLowioLimit;
}

std::unique_lock<std::recursive_mutex> lock_standard_descriptor(i32 index) {
    if (index < 0 || index > 2) return {};
    StdioStateSuspension unlocked;
    return std::unique_lock(standard_descriptor_mutex(index));
}

std::unique_lock<std::recursive_mutex> lock_lowio_descriptor(i32 index) {
    if (index < 0 || index >= lowio_limit()) return {};
    std::recursive_mutex* mutex;
    {
        StdioStateLock lookup;
        mutex = &lowio_descriptor_mutex(index);
    }
    StdioStateSuspension unlocked;
    return std::unique_lock(*mutex);
}

std::recursive_mutex& standard_descriptor_mutex(i32 index) {
    return stdio_arena().standard_mutexes[static_cast<usize>(index)];
}
std::recursive_mutex& lowio_descriptor_mutex(i32 index) {
    if (index <= 2) return standard_descriptor_mutex(index);
    auto& mutex = stdio_arena().lowio_mutexes[index];
    if (!mutex) mutex = std::make_unique<std::recursive_mutex>();
    return *mutex;
}

} // namespace vitreo::winapi::crt_stdio_internal
