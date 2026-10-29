#pragma once

/// @file common.hpp
/// @brief Common includes for all Vitreo source files
///
/// Include this header to get access to all fundamental types and utilities.

#include "vitreo/types.hpp"
#include "vitreo/result.hpp"

// =============================================================================
// Debug Trace
// -----------------------------------------------------------------------------
// Two-level gate:
//   * Compile-time:  -DVITREO_TRACE_ENABLED=0 strips every call site to a no-op
//                    (release builds: zero code, zero argument evaluation).
//   * Runtime:       when compiled in, VITREO_TRACE=<anything> in the env
//                    flips a process-wide flag.
// =============================================================================

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

#ifndef VITREO_TRACE_ENABLED
#define VITREO_TRACE_ENABLED 1
#endif

// Per-host-thread guest tid (mirrors ThreadManager::t_current_guest_tid).
// Updated by ThreadManager::set_current_thread_id() so trace lines can be
// attributed to the correct guest thread without dragging in thread.hpp.
inline thread_local unsigned int g_vitreo_trace_tid = 1;

namespace vitreo::detail {

/// Trace configuration is process-wide and immutable after startup. An inline
/// variable gives the program one pre-main environment lookup and leaves hot
/// JIT, dispatch, and rendering call sites with only a load and branch.
inline const bool g_trace_enabled =
    (std::getenv("VITREO_TRACE") != nullptr);

/// Process-wide diagnostic and JIT switches. Resolved once so hot dispatch,
/// memory, and WinAPI paths do not call getenv.
inline const bool g_jit_enabled =
    (std::getenv("VITREO_NO_JIT") == nullptr);
inline const bool g_int3_nop =
    (std::getenv("VITREO_INT3_NOP") != nullptr);
inline const bool g_diag_logs =
    (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
inline const bool g_fallback_hist =
    (std::getenv("VITREO_JIT_FALLBACK_HIST") != nullptr);
inline const bool g_win_diag =
    (std::getenv("VITREO_WIN_DIAG") != nullptr);
inline const bool g_gdi_diag =
    (std::getenv("VITREO_GDI_DIAG") != nullptr);

/// Format one trace line — process id, guest thread id, and the caller's
/// message — and emit it with a single write.
///
/// A guest process tree (a Chromium browser plus its GPU, utility, renderer,
/// and crash-handler children) is several host processes sharing one stderr,
/// each running many guest threads. Emitting the prefix and the body as two
/// separate unbuffered writes let other threads and processes interleave
/// between them, which detached prefixes from their messages and made the
/// combined log unattributable. One write per line keeps each line intact.
__attribute__((format(printf, 1, 2)))
inline void trace_emit(const char* format, ...) {
    static const int pid = static_cast<int>(::getpid());

    char stack_buffer[1024];
    const int prefix_length = std::snprintf(
        stack_buffer, sizeof(stack_buffer), "[p%d/T%u] ", pid,
        g_vitreo_trace_tid);
    if (prefix_length < 0) return;

    std::va_list args;
    va_start(args, format);
    std::va_list args_copy;
    va_copy(args_copy, args);
    const int body_length = std::vsnprintf(
        stack_buffer + prefix_length,
        sizeof(stack_buffer) - static_cast<std::size_t>(prefix_length),
        format, args);
    va_end(args);

    if (body_length < 0) {
        va_end(args_copy);
        return;
    }

    const std::size_t total =
        static_cast<std::size_t>(prefix_length) +
        static_cast<std::size_t>(body_length);
    if (total < sizeof(stack_buffer)) {
        va_end(args_copy);
        (void)std::fwrite(stack_buffer, 1, total, stderr);
        return;
    }

    // Rare long line: reformat into an exactly-sized buffer rather than
    // truncating diagnostics.
    std::vector<char> heap_buffer(total + 1);
    std::memcpy(heap_buffer.data(), stack_buffer,
                static_cast<std::size_t>(prefix_length));
    (void)std::vsnprintf(heap_buffer.data() + prefix_length,
                         heap_buffer.size() -
                             static_cast<std::size_t>(prefix_length),
                         format, args_copy);
    va_end(args_copy);
    (void)std::fwrite(heap_buffer.data(), 1, total, stderr);
}

} // namespace vitreo::detail

#if VITREO_TRACE_ENABLED
#define VITREO_TRACE(...) do { \
    if (::vitreo::detail::g_trace_enabled) { \
        ::vitreo::detail::trace_emit(__VA_ARGS__); \
    } \
} while (0)
#else
// Cast args to (void) inside a never-taken branch so the compiler still
// type-checks them but generates nothing. The `if (false)` is folded away.
#define VITREO_TRACE(...) do { if (false) { (void)std::fprintf(stderr, __VA_ARGS__); } } while (0)
#endif

#include <atomic>

// Global flag: set by TLS alert diagnostic to request trace ring dump
inline std::atomic<bool> g_trace_ring_dump_requested{false};

// Per-thread trace ring for RIP recording (accessible from thunk handlers)
constexpr std::size_t THREAD_TRACE_SIZE = 16384;
struct ThreadTraceRing {
    std::uint64_t rips[THREAD_TRACE_SIZE] = {};
    std::size_t idx = 0;
    bool active = false;
};
inline thread_local ThreadTraceRing t_thread_trace;

namespace vitreo {

// =============================================================================
// Version Information
// =============================================================================

constexpr u32 VERSION_MAJOR = 1;
constexpr u32 VERSION_MINOR = 0;
constexpr u32 VERSION_PATCH = 0;

// Public version shared with the Mac app. The app's CFBundleVersion is the
// separate integer build count and is not part of this string.
constexpr const char* VERSION_STRING = "1.0.0-beta.1";

// =============================================================================
// Debug Utilities
// =============================================================================

#ifdef VITREO_DEBUG
    #define VITREO_ASSERT(cond) \
        do { if (!(cond)) { __builtin_trap(); } } while (0)
    
    #define VITREO_ASSERT_MSG(cond, msg) \
        do { if (!(cond)) { __builtin_trap(); } } while (0)
#else
    #define VITREO_ASSERT(cond) ((void)0)
    #define VITREO_ASSERT_MSG(cond, msg) ((void)0)
#endif

/// Unreachable code marker
#define VITREO_UNREACHABLE() __builtin_unreachable()

// =============================================================================
// Utility Functions
// =============================================================================

/// Read an environment-controlled feature flag with an explicit default.
///
/// An absent variable uses default_enabled. The exact value "0" disables the
/// feature; any other present value enables it. This lets stable features be
/// default-on while retaining a consistent opt-out for compatibility bisection.
[[nodiscard]] inline bool environment_flag_enabled(
    const char* name, bool default_enabled = false) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr) return default_enabled;
    return !(value[0] == '0' && value[1] == '\0');
}

/// Align a value up to the specified alignment
template<typename T>
[[nodiscard]] constexpr T align_up(T value, T alignment) noexcept {
    static_assert(std::is_integral_v<T>, "align_up requires integral type");
    return (value + alignment - 1) & ~(alignment - 1);
}

/// Align a value down to the specified alignment
template<typename T>
[[nodiscard]] constexpr T align_down(T value, T alignment) noexcept {
    static_assert(std::is_integral_v<T>, "align_down requires integral type");
    return value & ~(alignment - 1);
}

/// Check if a value is aligned to the specified alignment
template<typename T>
[[nodiscard]] constexpr bool is_aligned(T value, T alignment) noexcept {
    static_assert(std::is_integral_v<T>, "is_aligned requires integral type");
    return (value & (alignment - 1)) == 0;
}

/// Check if a value is a power of two
template<typename T>
[[nodiscard]] constexpr bool is_power_of_two(T value) noexcept {
    static_assert(std::is_integral_v<T>, "is_power_of_two requires integral type");
    return value > 0 && (value & (value - 1)) == 0;
}

// =============================================================================
// Page Size Constants
// =============================================================================

/// Host page size (4KB on macOS ARM64)
constexpr usize HOST_PAGE_SIZE = 4096;

/// Guest page size (matches Windows 4KB pages)
constexpr usize GUEST_PAGE_SIZE = 4096;

/// Large page size (2MB)
constexpr usize LARGE_PAGE_SIZE = 2 * 1024 * 1024;

} // namespace vitreo
